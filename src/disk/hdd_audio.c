/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Hard disk audio emulation.
 *
 * Authors: Toni Riikonen, <riikonen.toni@gmail.com>
 *
 *          Copyright 2026 Toni Riikonen.
 */

 #include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <86box/86box.h>
#include <86box/hdd.h>
#include <86box/hdd_audio.h>
#include <86box/sound.h>
#include <86box/sound_util.h>
#include <86box/thread.h>
#include <86box/plat.h>
#include <86box/path.h>
#include <86box/ini.h>
#include <86box/mem.h>
#include <86box/rom.h>
#include <86box/timer.h>
#include <86box/device.h>
#include <86box/machine.h>
#include "cpu.h"

/* Maximum number of simultaneous seek sounds per HDD */
#define HDD_MAX_SEEK_VOICES_PER_HDD 8

/* Maximum number of HDDs with audio emulation */
#define HDD_AUDIO_MAX_DRIVES 8

typedef struct {
    int    active;
    int    position;    /* Output samples played so far */
    float  volume;
    int    profile_id;  /* Which profile's seek sound to use */
    float  rate;        /* Playback speed: below 1 is deeper and longer */
    double src_pos;     /* Position in the sample, in source frames */
} hdd_seek_voice_t;

/* A little variation from one seek to the next, as no two head movements
   sound quite alike: a factor between 1 - amount and 1 + amount. */
static float
hdd_audio_jitter(float amount)
{
    return 1.0f + amount * ((float) (rand() % 2001) / 1000.0f - 1.0f);
}

/* Start a seek voice. The further the heads travel, the harder the
   actuator swings and stops: long seeks are louder and deeper, a step
   to the next track is a short, quiet, crisp tick. */
static void
hdd_seek_voice_start(hdd_seek_voice_t *voice, int profile_id, float volume, float travel)
{
    float t;

    if (travel < 0.0f)
        travel = 0.0f;
    if (travel > 1.0f)
        travel = 1.0f;
    t = sqrtf(travel);

    voice->active     = 1;
    voice->position   = 0;
    voice->src_pos    = 0.0;
    voice->profile_id = profile_id;
    voice->volume     = volume * (0.5f + 0.65f * t) * hdd_audio_jitter(0.08f);
    voice->rate       = (1.12f - 0.24f * t) * hdd_audio_jitter(0.03f);
}

/* Per-HDD audio state */
typedef struct {
    int                  hdd_index;       /* Index into hdd[] array */
    int                  profile_id;      /* Audio profile ID */
    hdd_spindle_state_t  spindle_state;
    int                  spindle_pos;
    int                  spindle_transition_pos;
    hdd_seek_voice_t     seek_voices[HDD_MAX_SEEK_VOICES_PER_HDD];
    /* Drive mechanics: power-on spin-up wait and recalibration clicks. */
    pc_timer_t           mech_timer;
    int                  mech_phase;      /* HDD_MECH_* */
    int                  mech_clicks;     /* Clicks still to play */
    int                  mech_click_num;  /* Clicks played so far in this run */
    double               mech_click_us;   /* Spacing between clicks */
    int                  mech_busy;       /* Drive reports BSY until the run ends */
    int                  mech_srst_seen;  /* An SRST has been seen since power-on */
    uint64_t             mech_srst_tsc;   /* When the last SRST was seen */
    int                  restart_after_stop; /* Power cycle: spin up again once stopped */
} hdd_audio_drive_state_t;

static void hdd_audio_mech_power_on(hdd_audio_drive_state_t *state, double extra_us);
static void hdd_audio_spindle_stopped(hdd_audio_drive_state_t *state);
static void hdd_audio_mech_warm_reset(hdd_audio_drive_state_t *state);
static int  hdd_audio_mech_eligible(const hdd_audio_drive_state_t *state);

enum {
    HDD_MECH_IDLE = 0,
    HDD_MECH_SPINUP,
    HDD_MECH_CLICKS
};

/* Clicks played after spin-up while the heads calibrate, and after a
   soft reset (SRST) of a drive that is already spinning. */
#define HDD_MECH_POWERON_CLICKS 8
#define HDD_MECH_SRST_CLICKS    4
#define HDD_MECH_SRST_QUIET_US  10000000.0
#define HDD_MECH_POWERON_HOLD_MS 2000

/* Audio samples structure for a profile */
typedef struct {
    int16_t *spindle_start_buffer;
    int      spindle_start_samples;
    float    spindle_start_volume;
    int16_t *spindle_loop_buffer;
    int      spindle_loop_samples;
    float    spindle_loop_volume;
    int16_t *spindle_stop_buffer;
    int      spindle_stop_samples;
    float    spindle_stop_volume;
    int16_t *seek_buffer;
    int      seek_samples;
    float    seek_volume;
    int      loaded;
} hdd_audio_samples_t;

/* Global audio profile configurations */
static hdd_audio_profile_config_t audio_profiles[HDD_AUDIO_PROFILE_MAX];
static int                        audio_profile_count = 0;

/* Per-profile loaded samples */
static hdd_audio_samples_t profile_samples[HDD_AUDIO_PROFILE_MAX];

/* Per-HDD audio states */
static hdd_audio_drive_state_t drive_states[HDD_AUDIO_MAX_DRIVES];
static int                     active_drive_count = 0;

static mutex_t *hdd_audio_mutex = NULL;

/* Set while the drives spin down at exit or with the power switched off,
   so the HDD sound thread keeps rendering although the machine has
   stopped. */
volatile int hdd_audio_closing = 0;

/* Set by the front panel Reset button before its hard reset: the drives
   keep spinning through a reset, unlike a power cycle. */
volatile int hdd_audio_reset_button = 0;

/* Set once the machine has been powered on by the first hard reset. */
static int hdd_audio_powered = 0;

/* At power-on the processor is held (STPCLK#) while the drives spin
   down (on a power cycle) and start spinning up, so the drive is heard
   before the POST begins. */
static pc_timer_t hdd_audio_hold_timer;
static double     hdd_audio_hold_us = 0.0;

#ifdef ENABLE_HDD_AUDIO_LOG
int hdd_audio_do_log = ENABLE_HDD_AUDIO_LOG;

static void
hdd_audio_log(const char *fmt, ...)
{
    va_list ap;

    if (hdd_audio_do_log) {
        va_start(ap, fmt);
        pclog_ex(fmt, ap);
        va_end(ap);
    }
}
#else
#    define hdd_audio_log(fmt, ...)
#endif

/* Load audio profiles from configuration file */
void
hdd_audio_load_profiles(void)
{
    ini_t profiles_ini;
    char  cfg_fn[1024] = { 0 };

    /*
     * asset_getfile returns a path from the trusted asset search paths.
     * The filename is hardcoded and validated against existing files.
     */
    int ret = asset_getfile("assets/sounds/hdd/hdd_audio_profiles.cfg", cfg_fn, 1024);
    if (!ret) {
        hdd_audio_log("HDD Audio: Could not find hdd_audio_profiles.cfg\n");
        return;
    }

    /* Validate that the path does not contain path traversal sequences */
    if (strstr(cfg_fn, "..") != NULL) {
        hdd_audio_log("HDD Audio: Invalid path detected\n");
        return;
    }

    /* Validate the path ends with our expected filename */
    const char *expected_suffix = "hdd_audio_profiles.cfg";
    size_t cfg_len = strlen(cfg_fn);
    size_t suffix_len = strlen(expected_suffix);
    if (cfg_len < suffix_len || strcmp(cfg_fn + cfg_len - suffix_len, expected_suffix) != 0) {
        pclog("HDD Audio: Unexpected config path\n");
        return;
    }

    profiles_ini = ini_read_ex(cfg_fn, 1);  /* lgtm[cpp/path-injection] */
    if (profiles_ini == NULL) {
        hdd_audio_log("HDD Audio: Failed to load hdd_audio_profiles.cfg\n");
        return;
    }

    audio_profile_count = 0;

    /* Load profiles by trying known profile section names */
    for (int i = 0; i < HDD_AUDIO_PROFILE_MAX && audio_profile_count < HDD_AUDIO_PROFILE_MAX; i++) {
        char section_name[64];
        sprintf(section_name, "Profile \"%d\"", i);

        ini_section_t cat = ini_find_section(profiles_ini, section_name);
        if (cat == NULL)
            continue;

        hdd_audio_profile_config_t *config = &audio_profiles[audio_profile_count];
        memset(config, 0, sizeof(hdd_audio_profile_config_t));

        config->id = ini_section_get_int(cat, "id", i);

        const char *name = ini_section_get_string(cat, "name", "Unknown");
        strncpy(config->name, name, sizeof(config->name) - 1);

        const char *internal_name = ini_section_get_string(cat, "internal_name", "unknown");
        strncpy(config->internal_name, internal_name, sizeof(config->internal_name) - 1);

        config->rpm = ini_section_get_int(cat, "rpm", 0);
        config->poweron_clicks = ini_section_get_int(cat, "poweron_clicks", HDD_MECH_POWERON_CLICKS);
        config->srst_clicks    = ini_section_get_int(cat, "srst_clicks", HDD_MECH_SRST_CLICKS);
        config->poweron_hold_ms = ini_section_get_int(cat, "poweron_hold_ms", HDD_MECH_POWERON_HOLD_MS);
        strncpy(config->models, ini_section_get_string(cat, "models", ""), sizeof(config->models) - 1);

        /* Load spindle motor sample files */
        const char *file = ini_section_get_string(cat, "spindlemotor_start_file", "");
        strncpy(config->spindlemotor_start.filename, file, sizeof(config->spindlemotor_start.filename) - 1);
        config->spindlemotor_start.volume = (float) ini_section_get_double(cat, "spindlemotor_start_volume", 1.0);

        file = ini_section_get_string(cat, "spindlemotor_loop_file", "");
        strncpy(config->spindlemotor_loop.filename, file, sizeof(config->spindlemotor_loop.filename) - 1);
        config->spindlemotor_loop.volume = (float) ini_section_get_double(cat, "spindlemotor_loop_volume", 1.0);

        file = ini_section_get_string(cat, "spindlemotor_stop_file", "");
        strncpy(config->spindlemotor_stop.filename, file, sizeof(config->spindlemotor_stop.filename) - 1);
        config->spindlemotor_stop.volume = (float) ini_section_get_double(cat, "spindlemotor_stop_volume", 1.0);

        /* Load seek sample file */
        file = ini_section_get_string(cat, "seek_track_file", "");
        strncpy(config->seek_track.filename, file, sizeof(config->seek_track.filename) - 1);
        config->seek_track.volume = (float) ini_section_get_double(cat, "seek_track_volume", 1.0);

        hdd_audio_log("HDD Audio: Loaded profile %d: %s (%s)\n",
                      audio_profile_count, config->name, config->internal_name);

        audio_profile_count++;
    }

    ini_close(profiles_ini);

    hdd_audio_log("HDD Audio: Loaded %d audio profiles\n", audio_profile_count);
}

/* Public API functions */
int
hdd_audio_get_profile_count(void)
{
    return audio_profile_count;
}

const hdd_audio_profile_config_t *
hdd_audio_get_profile(int id)
{
    if (id < 0 || id >= audio_profile_count)
        return NULL;
    return &audio_profiles[id];
}

const char *
hdd_audio_get_profile_name(int id)
{
    if (id < 0 || id >= audio_profile_count)
        return NULL;
    return audio_profiles[id].name;
}

const char *
hdd_audio_get_profile_internal_name(int id)
{
    if (id < 0 || id >= audio_profile_count)
        return NULL;
    return audio_profiles[id].internal_name;
}

uint32_t
hdd_audio_get_profile_rpm(int id)
{
    if (id < 0 || id >= audio_profile_count)
        return 0;
    return audio_profiles[id].rpm;
}

/* Whether a profile is offered for a drive model. A profile with a
   "models" list (comma-separated internal names; a trailing '*' matches
   a prefix) is offered for those models only; one without is offered
   when it has no RPM set or the same RPM. */
int
hdd_audio_profile_fits(int id, uint32_t rpm, const char *model)
{
    const hdd_audio_profile_config_t *config;
    const char                       *p;

    if ((id < 0) || (id >= audio_profile_count))
        return 0;

    config = &audio_profiles[id];
    if (!config->models[0])
        return (config->rpm == 0) || (config->rpm == rpm);
    if (!model || !model[0])
        return 0;

    p = config->models;
    while (*p) {
        char   tok[64];
        size_t n = 0;

        while ((*p == ',') || (*p == ' '))
            p++;
        while (*p && (*p != ',') && (n < (sizeof(tok) - 1)))
            tok[n++] = *p++;
        while ((n > 0) && (tok[n - 1] == ' '))
            n--;
        tok[n] = '\0';
        if (n == 0)
            continue;

        if (tok[n - 1] == '*') {
            if (!strncasecmp(model, tok, n - 1))
                return 1;
        } else if (!strcasecmp(model, tok))
            return 1;
    }

    return 0;
}

int
hdd_audio_get_profile_by_internal_name(const char *internal_name)
{
    if (!internal_name)
        return 0;

    for (int i = 0; i < audio_profile_count; i++) {
        if (strcmp(audio_profiles[i].internal_name, internal_name) == 0)
            return i;
    }
    return 0;
}

void
hdd_audio_close(void)
{
    /* Free all loaded profile samples */
    for (int i = 0; i < HDD_AUDIO_PROFILE_MAX; i++) {
        if (profile_samples[i].spindle_start_buffer) {
            free(profile_samples[i].spindle_start_buffer);
            profile_samples[i].spindle_start_buffer = NULL;
        }
        if (profile_samples[i].spindle_loop_buffer) {
            free(profile_samples[i].spindle_loop_buffer);
            profile_samples[i].spindle_loop_buffer = NULL;
        }
        if (profile_samples[i].spindle_stop_buffer) {
            free(profile_samples[i].spindle_stop_buffer);
            profile_samples[i].spindle_stop_buffer = NULL;
        }
        if (profile_samples[i].seek_buffer) {
            free(profile_samples[i].seek_buffer);
            profile_samples[i].seek_buffer = NULL;
        }
        profile_samples[i].loaded = 0;
    }
    
    if (hdd_audio_mutex) {
        thread_close_mutex(hdd_audio_mutex);
        hdd_audio_mutex = NULL;
    }
}

/* Load samples for a specific profile */
static void
hdd_audio_load_profile_samples(int profile_id)
{
    if (profile_id < 0 || profile_id >= audio_profile_count)
        return;
    
    hdd_audio_profile_config_t *config  = &audio_profiles[profile_id];
    hdd_audio_samples_t        *samples = &profile_samples[profile_id];
    
    /* Already loaded? */
    if (samples->loaded)
        return;
    
    /* Profile 0 is "None" - no audio */
    if (profile_id == 0 || strcmp(config->internal_name, "none") == 0) {
        samples->loaded = 1;
        return;
    }

    hdd_audio_log("HDD Audio: Loading samples for profile %d (%s)\n", profile_id, config->name);
    
    /* Load spindle loop (main running sound) */
    if (config->spindlemotor_loop.filename[0]) {
        samples->spindle_loop_buffer = sound_load_wav(
            config->spindlemotor_loop.filename,
            &samples->spindle_loop_samples);
        if (samples->spindle_loop_buffer) {
            samples->spindle_loop_volume = config->spindlemotor_loop.volume;
            hdd_audio_log("HDD Audio: Loaded spindle loop, %d frames\n", samples->spindle_loop_samples);
        } else {
            hdd_audio_log("HDD Audio: Failed to load spindle loop: %s\n", config->spindlemotor_loop.filename);
        }
    }
    
    /* Load spindle start */
    if (config->spindlemotor_start.filename[0]) {
        samples->spindle_start_buffer = sound_load_wav(
            config->spindlemotor_start.filename,
            &samples->spindle_start_samples);
        if (samples->spindle_start_buffer) {
            samples->spindle_start_volume = config->spindlemotor_start.volume;
            hdd_audio_log("HDD Audio: Loaded spindle start, %d frames\n", samples->spindle_start_samples);
        }
    }
    
    /* Load spindle stop */
    if (config->spindlemotor_stop.filename[0]) {
        samples->spindle_stop_buffer = sound_load_wav(
            config->spindlemotor_stop.filename,
            &samples->spindle_stop_samples);
        if (samples->spindle_stop_buffer) {
            samples->spindle_stop_volume = config->spindlemotor_stop.volume;
            hdd_audio_log("HDD Audio: Loaded spindle stop, %d frames\n", samples->spindle_stop_samples);
        }
    }
    
    /* Load seek sound */
    if (config->seek_track.filename[0]) {
        samples->seek_buffer = sound_load_wav(
            config->seek_track.filename,
            &samples->seek_samples);
        if (samples->seek_buffer) {
            samples->seek_volume = config->seek_track.volume;
            hdd_audio_log("HDD Audio: Loaded seek sound, %d frames (%.1f ms)\n", 
                          samples->seek_samples, (float)samples->seek_samples / 48.0f);
        } else {
            hdd_audio_log("HDD Audio: Failed to load seek sound: %s\n", config->seek_track.filename);
        }
    }
    
    samples->loaded = 1;
}

/* Find drive state for a given HDD index, or NULL if not tracked */
static hdd_audio_drive_state_t *
hdd_audio_find_drive_state(int hdd_index)
{
    for (int i = 0; i < active_drive_count; i++) {
        if (drive_states[i].hdd_index == hdd_index)
            return &drive_states[i];
    }
    return NULL;
}

void
hdd_audio_init(void)
{
    /* Initialize profile samples */
    memset(profile_samples, 0, sizeof(profile_samples));
    memset(drive_states, 0, sizeof(drive_states));
    active_drive_count = 0;
    
    hdd_audio_log("HDD Audio Init: audio_profile_count=%d\n", audio_profile_count);
    
    /* Create mutex BEFORE loading samples or calling spinup */
    if (!hdd_audio_mutex)
        hdd_audio_mutex = thread_create_mutex();

    /* Find all HDDs with valid audio profiles and initialize their states */
    for (int i = 0; i < HDD_NUM && active_drive_count < HDD_AUDIO_MAX_DRIVES; i++) {
        if (hdd[i].bus_type != HDD_BUS_DISABLED && hdd[i].audio_profile > 0) {
            hdd_audio_log("HDD Audio Init: HDD %d bus_type=%d audio_profile=%d\n", 
                          i, hdd[i].bus_type, hdd[i].audio_profile);
            
            hdd_audio_drive_state_t *state = &drive_states[active_drive_count];
            state->hdd_index = i;
            state->profile_id = hdd[i].audio_profile;
            state->spindle_state = HDD_SPINDLE_STOPPED;
            state->spindle_pos = 0;
            state->spindle_transition_pos = 0;
            
            /* Initialize seek voices for this drive */
            for (int v = 0; v < HDD_MAX_SEEK_VOICES_PER_HDD; v++) {
                state->seek_voices[v].active = 0;
                state->seek_voices[v].position = 0;
                state->seek_voices[v].volume = 1.0f;
                state->seek_voices[v].profile_id = state->profile_id;
            }
            
            /* Load samples for this profile if not already loaded */
            hdd_audio_load_profile_samples(state->profile_id);
            
            hdd_audio_log("HDD Audio: Initialized drive %d with profile %d (%s)\n", 
                          i, state->profile_id, 
                          hdd_audio_get_profile_name(state->profile_id));
            
            active_drive_count++;
        }
    }
    
    hdd_audio_log("HDD Audio Init: %d active drives with audio\n", active_drive_count);
    
    /* Start spindle motors for all active drives */
    for (int i = 0; i < active_drive_count; i++) {
        hdd_audio_spinup_drive(drive_states[i].hdd_index);
    }

    sound_hdd_thread_init();
}

void
hdd_audio_reset(void)
{
    hdd_audio_log("HDD Audio: Reset\n");
    
    /* Lock mutex to prevent audio callback from accessing buffers during reset */
    if (hdd_audio_mutex)
        thread_wait_mutex(hdd_audio_mutex);
    
    hdd_audio_hold_us = 0.0;

    /* Remember which drives were spinning: a hard reset is a power cycle,
       so they spin down before spinning up again. */
    int was_spinning[HDD_NUM];
    int was_running[HDD_NUM];
    int was_pos[HDD_NUM];
    int was_stopping[HDD_NUM];
    int was_stop_pos[HDD_NUM];
    const int reset_button = hdd_audio_reset_button;
    hdd_audio_reset_button = 0;
    for (int i = 0; i < HDD_NUM; i++) {
        was_spinning[i] = -1;
        was_running[i]  = 0;
        was_pos[i]      = 0;
        was_stopping[i] = -1;
        was_stop_pos[i] = 0;
    }
    /* Drives still spinning down after the power switch was turned off:
       the platters have to stop before the motor starts them again. */
    for (int i = 0; i < active_drive_count; i++) {
        if (drive_states[i].spindle_state == HDD_SPINDLE_STOPPING) {
            was_stopping[drive_states[i].hdd_index] = drive_states[i].profile_id;
            was_stop_pos[drive_states[i].hdd_index] = drive_states[i].spindle_transition_pos;
        }
    }
    for (int i = 0; hdd_audio_powered && (i < active_drive_count); i++) {
        if ((drive_states[i].spindle_state == HDD_SPINDLE_RUNNING) ||
            (drive_states[i].spindle_state == HDD_SPINDLE_STARTING))
            was_spinning[drive_states[i].hdd_index] = drive_states[i].profile_id;
        if (drive_states[i].spindle_state == HDD_SPINDLE_RUNNING) {
            was_running[drive_states[i].hdd_index] = 1;
            was_pos[drive_states[i].hdd_index]     = drive_states[i].spindle_pos;
        }
    }

    /* Reset all drive states */
    for (int i = 0; i < active_drive_count; i++) {
        drive_states[i].spindle_state = HDD_SPINDLE_STOPPED;
        drive_states[i].spindle_pos = 0;
        drive_states[i].spindle_transition_pos = 0;
        for (int v = 0; v < HDD_MAX_SEEK_VOICES_PER_HDD; v++) {
            drive_states[i].seek_voices[v].active = 0;
            drive_states[i].seek_voices[v].position = 0;
            drive_states[i].seek_voices[v].volume = 1.0f;
        }
    }
    active_drive_count = 0;
    
    /* Free previously loaded samples (but keep profiles) */
    for (int i = 0; i < HDD_AUDIO_PROFILE_MAX; i++) {
        if (profile_samples[i].spindle_start_buffer) {
            free(profile_samples[i].spindle_start_buffer);
            profile_samples[i].spindle_start_buffer = NULL;
        }
        if (profile_samples[i].spindle_loop_buffer) {
            free(profile_samples[i].spindle_loop_buffer);
            profile_samples[i].spindle_loop_buffer = NULL;
        }
        if (profile_samples[i].spindle_stop_buffer) {
            free(profile_samples[i].spindle_stop_buffer);
            profile_samples[i].spindle_stop_buffer = NULL;
        }
        if (profile_samples[i].seek_buffer) {
            free(profile_samples[i].seek_buffer);
            profile_samples[i].seek_buffer = NULL;
        }
        profile_samples[i].loaded = 0;
    }
    
    if (hdd_audio_mutex)
        thread_release_mutex(hdd_audio_mutex);
    
    /* Find all HDDs with valid audio profiles and initialize their states */
    for (int i = 0; i < HDD_NUM && active_drive_count < HDD_AUDIO_MAX_DRIVES; i++) {
        if (hdd[i].bus_type != HDD_BUS_DISABLED && hdd[i].audio_profile > 0) {
            hdd_audio_log("HDD Audio Reset: HDD %d audio_profile=%d\n", i, hdd[i].audio_profile);
            
            hdd_audio_drive_state_t *state = &drive_states[active_drive_count];
            state->hdd_index = i;
            state->profile_id = hdd[i].audio_profile;
            state->spindle_state = HDD_SPINDLE_STOPPED;
            state->spindle_pos = 0;
            state->spindle_transition_pos = 0;
            
            /* Initialize seek voices for this drive */
            for (int v = 0; v < HDD_MAX_SEEK_VOICES_PER_HDD; v++) {
                state->seek_voices[v].active = 0;
                state->seek_voices[v].position = 0;
                state->seek_voices[v].volume = 1.0f;
                state->seek_voices[v].profile_id = state->profile_id;
            }
            
            /* Load samples for this profile if not already loaded */
            hdd_audio_load_profile_samples(state->profile_id);
            
            hdd_audio_log("HDD Audio: Reset drive %d with profile %d (%s)\n", 
                          i, state->profile_id, 
                          hdd_audio_get_profile_name(state->profile_id));
            
            active_drive_count++;
        }
    }
    
    hdd_audio_log("HDD Audio Reset: %d active drives with audio\n", active_drive_count);
    
    /* Start spindle motors for all active drives */
    for (int i = 0; i < active_drive_count; i++) {
        hdd_audio_drive_state_t   *state   = &drive_states[i];
        const hdd_audio_samples_t *samples = &profile_samples[state->profile_id];
        double                     stop_us = 0.0;

        state->restart_after_stop = 0;
        if (reset_button && was_running[state->hdd_index] &&
            (was_spinning[state->hdd_index] == state->profile_id)) {
            /* The Reset button: the platters keep turning and the heads
               recalibrate, as after a soft reset. */
            if (hdd_audio_mutex)
                thread_wait_mutex(hdd_audio_mutex);
            state->spindle_state = HDD_SPINDLE_RUNNING;
            state->spindle_pos   = was_pos[state->hdd_index];
            if (hdd_audio_mutex)
                thread_release_mutex(hdd_audio_mutex);
            hdd_audio_mech_warm_reset(state);
            continue;
        }
        if (!reset_button && (was_stopping[state->hdd_index] == state->profile_id) && samples->spindle_stop_buffer &&
            (was_stop_pos[state->hdd_index] < samples->spindle_stop_samples) && hdd_audio_mutex) {
            /* Switched back on while spinning down: finish the spin-down,
               then spin up. */
            thread_wait_mutex(hdd_audio_mutex);
            state->spindle_state          = HDD_SPINDLE_STOPPING;
            state->spindle_transition_pos = was_stop_pos[state->hdd_index];
            state->restart_after_stop     = 1;
            thread_release_mutex(hdd_audio_mutex);
            stop_us = (double) (samples->spindle_stop_samples - was_stop_pos[state->hdd_index]) * 1000000.0 / 48000.0;
        } else if (!reset_button && (was_spinning[state->hdd_index] == state->profile_id) && samples->spindle_stop_buffer &&
            (samples->spindle_stop_samples > 0) && hdd_audio_mutex) {
            thread_wait_mutex(hdd_audio_mutex);
            state->spindle_state          = HDD_SPINDLE_STOPPING;
            state->spindle_transition_pos = 0;
            state->restart_after_stop     = 1;
            thread_release_mutex(hdd_audio_mutex);
            stop_us = (double) samples->spindle_stop_samples * 1000000.0 / 48000.0;
        } else
            hdd_audio_spinup_drive(state->hdd_index);
        hdd_audio_mech_power_on(state, stop_us);

        /* A drive still spinning down does not hold the POST up: the BIOS
           starts at once and waits at the drive check while it stops,
           spins up again and calibrates. */
        if (hdd_audio_mech_eligible(state) && (stop_us <= 0.0)) {
            double hold_us = (double) audio_profiles[state->profile_id].poweron_hold_ms * 1000.0;
            if (hold_us > hdd_audio_hold_us)
                hdd_audio_hold_us = hold_us;
        }
    }

    hdd_audio_powered = 1;
}

/* Start a seek voice regardless of the minimum spacing. */
static void
hdd_audio_play_click(hdd_audio_drive_state_t *state, float scale)
{
    int                  profile_id = state->profile_id;
    hdd_audio_samples_t *samples;

    if ((profile_id <= 0) || (profile_id >= audio_profile_count) || !hdd_audio_mutex)
        return;

    samples = &profile_samples[profile_id];
    if (!samples->seek_buffer || (samples->seek_samples == 0))
        return;

    thread_wait_mutex(hdd_audio_mutex);
    for (int v = 0; v < HDD_MAX_SEEK_VOICES_PER_HDD; v++) {
        if (!state->seek_voices[v].active) {
            /* Calibration and long-seek clicks: full strokes. */
            hdd_seek_voice_start(&state->seek_voices[v], profile_id, samples->seek_volume * scale, 0.75f);
            break;
        }
    }
    thread_release_mutex(hdd_audio_mutex);
}

/* Only drives set to a real model (not RAM-disk speed) behave mechanically. */
static int
hdd_audio_mech_eligible(const hdd_audio_drive_state_t *state)
{
    return (state->profile_id > 0) && (state->profile_id < audio_profile_count) &&
           (hdd[state->hdd_index].speed_preset != 0);
}

static void
hdd_audio_mech_start_clicks(hdd_audio_drive_state_t *state, int clicks, double first_us,
                            double spacing_us, int first_num, int busy)
{
    state->mech_phase     = HDD_MECH_CLICKS;
    state->mech_clicks    = clicks;
    state->mech_click_num = first_num;
    state->mech_click_us  = spacing_us;
    state->mech_busy      = busy;
    timer_set_delay_u64(&state->mech_timer, (uint64_t) (first_us * TIMER_USEC));
}

static void
hdd_audio_mech_timer(void *priv)
{
    hdd_audio_drive_state_t *state = (hdd_audio_drive_state_t *) priv;
    double                   delay;

    switch (state->mech_phase) {
        case HDD_MECH_SPINUP:
            /* Platters are at speed: the heads now calibrate. */
            hdd_audio_mech_start_clicks(state, audio_profiles[state->profile_id].poweron_clicks,
                                        1.0, 70000.0, 0, 1);
            break;

        case HDD_MECH_CLICKS:
            if (state->mech_clicks <= 0) {
                state->mech_phase = HDD_MECH_IDLE;
                state->mech_busy  = 0;
                break;
            }
            /* Alternate a full and a softer click, like the heads
               sweeping out and back. */
            hdd_audio_play_click(state, (state->mech_click_num & 1) ? 0.75f : 1.0f);
            state->mech_clicks--;
            state->mech_click_num++;
            delay = state->mech_click_us;
            /* A short pause in the middle of a calibration run. */
            if (state->mech_busy && (state->mech_click_num == 4) && (state->mech_clicks > 0))
                delay += 180000.0;
            timer_set_delay_u64(&state->mech_timer, (uint64_t) (delay * TIMER_USEC));
            break;

        default:
            break;
    }
}

/* Set up the power-on sequence: the drive is busy while it spins up
   (after extra_us of spinning down, on a power cycle), then calibrates.
   Called on hard reset after the spindles are set going. */
static void
hdd_audio_mech_power_on(hdd_audio_drive_state_t *state, double extra_us)
{
    const hdd_audio_samples_t *samples;
    double                     spinup_us;

    if (state->mech_timer.callback)
        timer_stop(&state->mech_timer);
    timer_add(&state->mech_timer, hdd_audio_mech_timer, state, 0);

    state->mech_phase     = HDD_MECH_IDLE;
    state->mech_busy      = 0;
    state->mech_clicks    = 0;
    state->mech_srst_seen = 0;

    if (!hdd_audio_mech_eligible(state))
        return;

    samples   = &profile_samples[state->profile_id];
    spinup_us = 4000000.0;
    if (samples->spindle_start_buffer && (samples->spindle_start_samples > 0))
        spinup_us = (double) samples->spindle_start_samples * 1000000.0 / 48000.0;

    state->mech_phase = HDD_MECH_SPINUP;
    state->mech_busy  = 1;
    timer_set_delay_u64(&state->mech_timer, (uint64_t) ((extra_us + spinup_us) * TIMER_USEC));
}

/* The Reset button: the drive stays spinning and recalibrates. */
static void
hdd_audio_mech_warm_reset(hdd_audio_drive_state_t *state)
{
    if (state->mech_timer.callback)
        timer_stop(&state->mech_timer);
    timer_add(&state->mech_timer, hdd_audio_mech_timer, state, 0);

    state->mech_phase     = HDD_MECH_IDLE;
    state->mech_busy      = 0;
    state->mech_clicks    = 0;
    state->mech_srst_seen = 0;

    if (hdd_audio_mech_eligible(state) && (audio_profiles[state->profile_id].srst_clicks > 0))
        hdd_audio_mech_start_clicks(state, audio_profiles[state->profile_id].srst_clicks,
                                    1.0, 60000.0, 0, 1);
}

/* The power switch is turned off: every drive spins down. The machine is
   stopped, so the HDD sound thread is kept rendering until it is turned
   back on; the next hard reset then spins the drives up from rest. */
void
hdd_audio_power_off(void)
{
    hdd_audio_closing = 1;

    for (int i = 0; i < active_drive_count; i++) {
        if (hdd_audio_mutex)
            thread_wait_mutex(hdd_audio_mutex);
        drive_states[i].restart_after_stop = 0;
        drive_states[i].mech_busy          = 0;
        drive_states[i].mech_phase         = HDD_MECH_IDLE;
        if ((drive_states[i].spindle_state == HDD_SPINDLE_RUNNING) ||
            (drive_states[i].spindle_state == HDD_SPINDLE_STARTING)) {
            drive_states[i].spindle_state          = HDD_SPINDLE_STOPPING;
            drive_states[i].spindle_transition_pos = 0;
        }
        if (hdd_audio_mutex)
            thread_release_mutex(hdd_audio_mutex);
    }

    hdd_audio_powered = 0;
}

/* The power switch is turned back on, just before the hard reset. */
void
hdd_audio_power_on(void)
{
    hdd_audio_closing = 0;
}

/* Spin every drive down and wait (in real time) for the spin-down
   sounds to finish. Called when the emulator is closing. */
void
hdd_audio_spindown_wait(void)
{
    int max_ms = 0;

    if (!hdd_audio_mutex || (active_drive_count == 0))
        return;

    for (int i = 0; i < active_drive_count; i++) {
        const hdd_audio_samples_t *samples = &profile_samples[drive_states[i].profile_id];
        int                        ms;

        if ((drive_states[i].spindle_state != HDD_SPINDLE_RUNNING) &&
            (drive_states[i].spindle_state != HDD_SPINDLE_STARTING))
            continue;
        if (!samples->spindle_stop_buffer || (samples->spindle_stop_samples <= 0))
            continue;

        ms = (int) ((int64_t) samples->spindle_stop_samples * 1000 / 48000);
        if (ms > max_ms)
            max_ms = ms;

        thread_wait_mutex(hdd_audio_mutex);
        drive_states[i].restart_after_stop     = 0;
        drive_states[i].spindle_state          = HDD_SPINDLE_STOPPING;
        drive_states[i].spindle_transition_pos = 0;
        thread_release_mutex(hdd_audio_mutex);
    }

    if (max_ms == 0)
        return;
    if (max_ms > 8000)
        max_ms = 8000;

    hdd_audio_closing = 1;

    /* Allow for the audio buffer in flight. */
    for (int t = 0; t < (max_ms + 250); t += 20) {
        if (hdd_audio_get_spindle_state() == HDD_SPINDLE_STOPPED)
            break;
        plat_delay_ms(20);
    }
    plat_delay_ms(250);
}

static void
hdd_audio_hold_release(UNUSED(void *priv))
{
    cpu_stpclk = 0;
}

/* Called after the processor has been reset at power-on. */
void
hdd_audio_power_on_hold(void)
{
    /* An LFTech extra, unless Hard Disk Spin-Up Wait is off in LFTech
       Features Setup: other machines start the POST at once. */
    if ((hdd_audio_hold_us <= 0.0) || !machine_lftech || !lftech_spinup_wait)
        return;

    timer_add(&hdd_audio_hold_timer, hdd_audio_hold_release, NULL, 0);
    cpu_stpclk = 1;
    timer_set_delay_u64(&hdd_audio_hold_timer, (uint64_t) (hdd_audio_hold_us * TIMER_USEC));
}

/* Returns non-zero while the drive is spinning up or calibrating. */
int
hdd_audio_drive_busy(int hdd_index)
{
    const hdd_audio_drive_state_t *state = hdd_audio_find_drive_state(hdd_index);

    /* An LFTech extra: other machines see a ready drive, as 86Box has it. */
    return machine_lftech && lftech_spinup_wait && state && state->mech_busy;
}

/* Soft reset (SRST): a spinning drive recalibrates its heads. */
void
hdd_audio_drive_srst(int hdd_index)
{
    hdd_audio_drive_state_t *state = hdd_audio_find_drive_state(hdd_index);
    double                   quiet_us;

    if (!state || !hdd_audio_mech_eligible(state) || !state->mech_timer.callback)
        return;

    /* Software resets come in clusters (the BIOS at POST, then DOS and
       drivers resetting through INT 13h); only recalibrate audibly when
       the drive has been left alone for a while, as after a warm boot. */
    quiet_us = state->mech_srst_seen ?
               ((double) (tsc - state->mech_srst_tsc) * 4294967296.0 / (double) TIMER_USEC) : 1e12;
    state->mech_srst_seen = 1;
    state->mech_srst_tsc  = tsc;
    if (quiet_us < HDD_MECH_SRST_QUIET_US)
        return;

    /* Still spinning up or calibrating: the reset changes nothing. */
    if (state->mech_busy)
        return;

    if (audio_profiles[state->profile_id].srst_clicks > 0)
        hdd_audio_mech_start_clicks(state, audio_profiles[state->profile_id].srst_clicks,
                                    1.0, 60000.0, 0, 1);
}

/* A long seek on a real drive is heard as a short run of clicks rather
   than a single tick. */
static void
hdd_audio_long_seek(hdd_audio_drive_state_t *state, const hard_disk_t *hdd_drive, uint32_t cylinder_diff)
{
    uint32_t cyls = hdd_drive->phy_cyl;
    int      extra;
    double   spacing;

    if (!hdd_audio_mech_eligible(state) || !state->mech_timer.callback ||
        (state->mech_phase != HDD_MECH_IDLE) || (cyls < 64))
        return;

    if (cylinder_diff < (cyls / 4))
        return;

    extra   = (cylinder_diff >= ((cyls * 3) / 4)) ? 2 : 1;
    spacing = hdd_drive->full_stroke_usec / 3.0;
    if (spacing < 8000.0)
        spacing = 8000.0;

    hdd_audio_mech_start_clicks(state, extra, spacing, spacing, 1, 0);
}

void
hdd_audio_seek(hard_disk_t *hdd_drive, uint32_t new_cylinder)
{
    uint32_t cylinder_diff = abs((int) hdd_drive->cur_cylinder - (int) new_cylinder);

    if (cylinder_diff == 0)
        return;

    /* Find the drive state for this HDD */
    hdd_audio_drive_state_t *drive_state = NULL;
    for (int i = 0; i < active_drive_count; i++) {
        if (&hdd[drive_states[i].hdd_index] == hdd_drive) {
            drive_state = &drive_states[i];
            break;
        }
    }
    
    /* If no drive state found, drive has no audio profile */
    if (!drive_state)
        return;
    
    int profile_id = drive_state->profile_id;
    
    /* No audio profile selected */
    if (profile_id == 0 || profile_id >= audio_profile_count)
        return;
    
    /* Load samples if not already loaded */
    if (!profile_samples[profile_id].loaded)
        hdd_audio_load_profile_samples(profile_id);
    
    hdd_audio_samples_t *samples = &profile_samples[profile_id];
    
    if (!samples->seek_buffer || samples->seek_samples == 0) {
        return;
    }

    /* Mutex must exist */
    if (!hdd_audio_mutex)
        return;

    int min_seek_spacing = 0;
    if (hdd_drive->cyl_switch_usec > 0)
        min_seek_spacing = (int)(hdd_drive->cyl_switch_usec * 48000.0 / 1000000.0);

    thread_wait_mutex(hdd_audio_mutex);

    /* Check if we should skip due to minimum spacing (per-drive) */
    for (int v = 0; v < HDD_MAX_SEEK_VOICES_PER_HDD; v++) {
        if (drive_state->seek_voices[v].active) {
            int pos = drive_state->seek_voices[v].position;
            if (pos >= 0 && pos < min_seek_spacing) {
                thread_release_mutex(hdd_audio_mutex);
                return;
            }
        }
    }

    /* Find a free seek voice for this drive */
    for (int v = 0; v < HDD_MAX_SEEK_VOICES_PER_HDD; v++) {
        if (!drive_state->seek_voices[v].active) {
            hdd_seek_voice_start(&drive_state->seek_voices[v], profile_id, samples->seek_volume,
                                 (hdd_drive->phy_cyl > 0) ? ((float) cylinder_diff / (float) hdd_drive->phy_cyl) : 0.5f);
            thread_release_mutex(hdd_audio_mutex);
            hdd_audio_long_seek(drive_state, hdd_drive, cylinder_diff);
            return;
        }
    }

    thread_release_mutex(hdd_audio_mutex);
}

/* Spinup a specific drive by HDD index */
void
hdd_audio_spinup_drive(int hdd_index)
{
    hdd_audio_drive_state_t *state = hdd_audio_find_drive_state(hdd_index);
    if (!state)
        return;
    
    if (state->spindle_state == HDD_SPINDLE_RUNNING || state->spindle_state == HDD_SPINDLE_STARTING)
        return;

    hdd_audio_log("HDD Audio: Spinup requested for drive %d (current state: %d)\n", hdd_index, state->spindle_state);

    if (hdd_audio_mutex)
        thread_wait_mutex(hdd_audio_mutex);
    state->spindle_state = HDD_SPINDLE_STARTING;
    state->spindle_transition_pos = 0;
    if (hdd_audio_mutex)
        thread_release_mutex(hdd_audio_mutex);
}

/* Spindown a specific drive by HDD index */
void
hdd_audio_spindown_drive(int hdd_index)
{
    hdd_audio_drive_state_t *state = hdd_audio_find_drive_state(hdd_index);
    if (!state)
        return;
    
    if (state->spindle_state == HDD_SPINDLE_STOPPED || state->spindle_state == HDD_SPINDLE_STOPPING)
        return;

    hdd_audio_log("HDD Audio: Spindown requested for drive %d (current state: %d)\n", hdd_index, state->spindle_state);

    if (hdd_audio_mutex)
        thread_wait_mutex(hdd_audio_mutex);
    state->spindle_state = HDD_SPINDLE_STOPPING;
    state->spindle_transition_pos = 0;
    if (hdd_audio_mutex)
        thread_release_mutex(hdd_audio_mutex);
}

/* Legacy functions for backward compatibility - operate on all drives */
void
hdd_audio_spinup(void)
{
    for (int i = 0; i < active_drive_count; i++) {
        hdd_audio_spinup_drive(drive_states[i].hdd_index);
    }
}

void
hdd_audio_spindown(void)
{
    for (int i = 0; i < active_drive_count; i++) {
        hdd_audio_spindown_drive(drive_states[i].hdd_index);
    }
}

hdd_spindle_state_t
hdd_audio_get_spindle_state(void)
{
    /* Return running if any drive is running */
    for (int i = 0; i < active_drive_count; i++) {
        if (drive_states[i].spindle_state == HDD_SPINDLE_RUNNING)
            return HDD_SPINDLE_RUNNING;
    }
    for (int i = 0; i < active_drive_count; i++) {
        if (drive_states[i].spindle_state == HDD_SPINDLE_STARTING)
            return HDD_SPINDLE_STARTING;
    }
    for (int i = 0; i < active_drive_count; i++) {
        if (drive_states[i].spindle_state == HDD_SPINDLE_STOPPING)
            return HDD_SPINDLE_STOPPING;
    }
    return HDD_SPINDLE_STOPPED;
}

hdd_spindle_state_t
hdd_audio_get_drive_spindle_state(int hdd_index)
{
    hdd_audio_drive_state_t *state = hdd_audio_find_drive_state(hdd_index);
    if (!state)
        return HDD_SPINDLE_STOPPED;
    return state->spindle_state;
}

/* Helper: Mix spindle start sound into float buffer */
static void
hdd_audio_mix_spindle_start_float(hdd_audio_drive_state_t *state, hdd_audio_samples_t *samples,
                                   float *float_buffer, int frames_in_buffer)
{
    if (!samples->spindle_start_buffer || samples->spindle_start_samples <= 0) {
        state->spindle_state = HDD_SPINDLE_RUNNING;
        state->spindle_pos = 0;
        return;
    }
    
    float start_volume = samples->spindle_start_volume;
    for (int i = 0; i < frames_in_buffer && state->spindle_transition_pos < samples->spindle_start_samples; i++) {
        float left_sample = (float) samples->spindle_start_buffer[state->spindle_transition_pos * 2] / 131072.0f * start_volume;
        float right_sample = (float) samples->spindle_start_buffer[state->spindle_transition_pos * 2 + 1] / 131072.0f * start_volume;
        float_buffer[i * 2]     += left_sample;
        float_buffer[i * 2 + 1] += right_sample;
        state->spindle_transition_pos++;
    }
    
    if (state->spindle_transition_pos >= samples->spindle_start_samples) {
        state->spindle_state = HDD_SPINDLE_RUNNING;
        state->spindle_pos = 0;
        hdd_audio_log("HDD Audio: Drive %d spinup complete, now running\n", state->hdd_index);
    }
}

/* Helper: Mix spindle loop sound into float buffer */
static void
hdd_audio_mix_spindle_loop_float(hdd_audio_drive_state_t *state, hdd_audio_samples_t *samples,
                                  float *float_buffer, int frames_in_buffer)
{
    if (!samples->spindle_loop_buffer || samples->spindle_loop_samples <= 0)
        return;
    
    float spindle_volume = samples->spindle_loop_volume;
    for (int i = 0; i < frames_in_buffer; i++) {
        float left_sample = (float) samples->spindle_loop_buffer[state->spindle_pos * 2] / 131072.0f * spindle_volume;
        float right_sample = (float) samples->spindle_loop_buffer[state->spindle_pos * 2 + 1] / 131072.0f * spindle_volume;
        float_buffer[i * 2]     += left_sample;
        float_buffer[i * 2 + 1] += right_sample;

        state->spindle_pos++;
        if (state->spindle_pos >= samples->spindle_loop_samples) {
            state->spindle_pos = 0;
        }
    }
}

/* The spin-down sample has finished: stop, or after a power cycle start
   spinning up again. Called from the mixer with the mutex held. */
static void
hdd_audio_spindle_stopped(hdd_audio_drive_state_t *state)
{
    if (state->restart_after_stop) {
        state->restart_after_stop     = 0;
        state->spindle_state          = HDD_SPINDLE_STARTING;
        state->spindle_transition_pos = 0;
    } else
        state->spindle_state = HDD_SPINDLE_STOPPED;
}

/* Helper: Mix spindle stop sound into float buffer */
static void
hdd_audio_mix_spindle_stop_float(hdd_audio_drive_state_t *state, hdd_audio_samples_t *samples,
                                  float *float_buffer, int frames_in_buffer)
{
    if (!samples->spindle_stop_buffer || samples->spindle_stop_samples <= 0) {
        hdd_audio_spindle_stopped(state);
        return;
    }
    
    float stop_volume = samples->spindle_stop_volume;
    for (int i = 0; i < frames_in_buffer && state->spindle_transition_pos < samples->spindle_stop_samples; i++) {
        float left_sample = (float) samples->spindle_stop_buffer[state->spindle_transition_pos * 2] / 131072.0f * stop_volume;
        float right_sample = (float) samples->spindle_stop_buffer[state->spindle_transition_pos * 2 + 1] / 131072.0f * stop_volume;
        float_buffer[i * 2]     += left_sample;
        float_buffer[i * 2 + 1] += right_sample;
        state->spindle_transition_pos++;
    }
    
    if (state->spindle_transition_pos >= samples->spindle_stop_samples) {
        hdd_audio_spindle_stopped(state);
        hdd_audio_log("HDD Audio: Drive %d spindown complete, now stopped\n", state->hdd_index);
    }
}

/* Helper: Mix seek sounds into float buffer */
static void
hdd_audio_mix_seek_float(hdd_audio_drive_state_t *state, float *float_buffer, int frames_in_buffer)
{
    for (int v = 0; v < HDD_MAX_SEEK_VOICES_PER_HDD; v++) {
        if (!state->seek_voices[v].active)
            continue;

        int seek_profile_id = state->seek_voices[v].profile_id;
        hdd_audio_samples_t *seek_samples = &profile_samples[seek_profile_id];
        if (!seek_samples->seek_buffer || seek_samples->seek_samples == 0)
            continue;

        float  voice_vol = state->seek_voices[v].volume;
        double rate      = (state->seek_voices[v].rate > 0.0f) ? state->seek_voices[v].rate : 1.0;
        double src       = state->seek_voices[v].src_pos;
        int    out       = state->seek_voices[v].position;
        const int last   = seek_samples->seek_samples - 1;

        for (int i = 0; (i < frames_in_buffer) && (src < last); i++, out++, src += rate) {
            const int   p  = (int) src;
            const float fr = (float) (src - p);
            float seek_left  = ((float) seek_samples->seek_buffer[p * 2] * (1.0f - fr) +
                                (float) seek_samples->seek_buffer[p * 2 + 2] * fr) / 131072.0f * voice_vol;
            float seek_right = ((float) seek_samples->seek_buffer[p * 2 + 1] * (1.0f - fr) +
                                (float) seek_samples->seek_buffer[p * 2 + 3] * fr) / 131072.0f * voice_vol;

            float_buffer[i * 2]     += seek_left;
            float_buffer[i * 2 + 1] += seek_right;
        }

        if (src >= last) {
            state->seek_voices[v].active   = 0;
            state->seek_voices[v].position = 0;
            state->seek_voices[v].src_pos  = 0.0;
        } else {
            state->seek_voices[v].position = out;
            state->seek_voices[v].src_pos  = src;
        }
    }
}

/* Helper: Mix spindle start sound into int16 buffer */
static void
hdd_audio_mix_spindle_start_int16(hdd_audio_drive_state_t *state, hdd_audio_samples_t *samples,
                                   int16_t *buffer, int frames_in_buffer)
{
    if (!samples->spindle_start_buffer || samples->spindle_start_samples <= 0) {
        state->spindle_state = HDD_SPINDLE_RUNNING;
        state->spindle_pos = 0;
        return;
    }
    
    float start_volume = samples->spindle_start_volume;
    for (int i = 0; i < frames_in_buffer && state->spindle_transition_pos < samples->spindle_start_samples; i++) {
        int32_t left = buffer[i * 2] + (int32_t)(samples->spindle_start_buffer[state->spindle_transition_pos * 2] * start_volume);
        int32_t right = buffer[i * 2 + 1] + (int32_t)(samples->spindle_start_buffer[state->spindle_transition_pos * 2 + 1] * start_volume);
        if (left > 32767) left = 32767;
        if (left < -32768) left = -32768;
        if (right > 32767) right = 32767;
        if (right < -32768) right = -32768;
        buffer[i * 2]     = (int16_t) left;
        buffer[i * 2 + 1] = (int16_t) right;
        state->spindle_transition_pos++;
    }
    
    if (state->spindle_transition_pos >= samples->spindle_start_samples) {
        state->spindle_state = HDD_SPINDLE_RUNNING;
        state->spindle_pos = 0;
        hdd_audio_log("HDD Audio: Drive %d spinup complete, now running\n", state->hdd_index);
    }
}

/* Helper: Mix spindle loop sound into int16 buffer */
static void
hdd_audio_mix_spindle_loop_int16(hdd_audio_drive_state_t *state, hdd_audio_samples_t *samples,
                                  int16_t *buffer, int frames_in_buffer)
{
    if (!samples->spindle_loop_buffer || samples->spindle_loop_samples <= 0)
        return;
    
    float spindle_volume = samples->spindle_loop_volume;
    for (int i = 0; i < frames_in_buffer; i++) {
        int32_t left = buffer[i * 2] + (int32_t)(samples->spindle_loop_buffer[state->spindle_pos * 2] * spindle_volume);
        int32_t right = buffer[i * 2 + 1] + (int32_t)(samples->spindle_loop_buffer[state->spindle_pos * 2 + 1] * spindle_volume);
        if (left > 32767) left = 32767;
        if (left < -32768) left = -32768;
        if (right > 32767) right = 32767;
        if (right < -32768) right = -32768;
        buffer[i * 2]     = (int16_t) left;
        buffer[i * 2 + 1] = (int16_t) right;

        state->spindle_pos++;
        if (state->spindle_pos >= samples->spindle_loop_samples) {
            state->spindle_pos = 0;
        }
    }
}

/* Helper: Mix spindle stop sound into int16 buffer */
static void
hdd_audio_mix_spindle_stop_int16(hdd_audio_drive_state_t *state, hdd_audio_samples_t *samples,
                                  int16_t *buffer, int frames_in_buffer)
{
    if (!samples->spindle_stop_buffer || samples->spindle_stop_samples <= 0) {
        hdd_audio_spindle_stopped(state);
        return;
    }
    
    float stop_volume = samples->spindle_stop_volume;
    for (int i = 0; i < frames_in_buffer && state->spindle_transition_pos < samples->spindle_stop_samples; i++) {
        int32_t left = buffer[i * 2] + (int32_t)(samples->spindle_stop_buffer[state->spindle_transition_pos * 2] * stop_volume);
        int32_t right = buffer[i * 2 + 1] + (int32_t)(samples->spindle_stop_buffer[state->spindle_transition_pos * 2 + 1] * stop_volume);
        if (left > 32767) left = 32767;
        if (left < -32768) left = -32768;
        if (right > 32767) right = 32767;
        if (right < -32768) right = -32768;
        buffer[i * 2]     = (int16_t) left;
        buffer[i * 2 + 1] = (int16_t) right;
        state->spindle_transition_pos++;
    }
    
    if (state->spindle_transition_pos >= samples->spindle_stop_samples) {
        hdd_audio_spindle_stopped(state);
        hdd_audio_log("HDD Audio: Drive %d spindown complete, now stopped\n", state->hdd_index);
    }
}

/* Helper: Mix seek sounds into int16 buffer */
static void
hdd_audio_mix_seek_int16(hdd_audio_drive_state_t *state, int16_t *buffer, int frames_in_buffer)
{
    for (int v = 0; v < HDD_MAX_SEEK_VOICES_PER_HDD; v++) {
        if (!state->seek_voices[v].active)
            continue;

        int seek_profile_id = state->seek_voices[v].profile_id;
        hdd_audio_samples_t *seek_samples = &profile_samples[seek_profile_id];
        if (!seek_samples->seek_buffer || seek_samples->seek_samples == 0)
            continue;

        float  voice_vol = state->seek_voices[v].volume;
        double rate      = (state->seek_voices[v].rate > 0.0f) ? state->seek_voices[v].rate : 1.0;
        double src       = state->seek_voices[v].src_pos;
        int    out       = state->seek_voices[v].position;
        const int last   = seek_samples->seek_samples - 1;

        for (int i = 0; (i < frames_in_buffer) && (src < last); i++, out++, src += rate) {
            const int   p  = (int) src;
            const float fr = (float) (src - p);
            int32_t left  = buffer[i * 2] + (int32_t) (((float) seek_samples->seek_buffer[p * 2] * (1.0f - fr) +
                                                        (float) seek_samples->seek_buffer[p * 2 + 2] * fr) * voice_vol);
            int32_t right = buffer[i * 2 + 1] + (int32_t) (((float) seek_samples->seek_buffer[p * 2 + 1] * (1.0f - fr) +
                                                            (float) seek_samples->seek_buffer[p * 2 + 3] * fr) * voice_vol);

            if (left > 32767) left = 32767;
            if (left < -32768) left = -32768;
            if (right > 32767) right = 32767;
            if (right < -32768) right = -32768;

            buffer[i * 2]     = (int16_t) left;
            buffer[i * 2 + 1] = (int16_t) right;
        }

        if (src >= last) {
            state->seek_voices[v].active   = 0;
            state->seek_voices[v].position = 0;
            state->seek_voices[v].src_pos  = 0.0;
        } else {
            state->seek_voices[v].position = out;
            state->seek_voices[v].src_pos  = src;
        }
    }
}

/* Process a single drive's audio in float mode */
static void
hdd_audio_process_drive_float(hdd_audio_drive_state_t *state, float *float_buffer, int frames_in_buffer)
{
    int profile_id = state->profile_id;
    
    if (profile_id <= 0 || profile_id >= HDD_AUDIO_PROFILE_MAX)
        return;
    
    hdd_audio_samples_t *samples = &profile_samples[profile_id];
    if (!samples->loaded)
        return;

    /* Handle spindle states for this drive */
    switch (state->spindle_state) {
        case HDD_SPINDLE_STARTING:
            hdd_audio_mix_spindle_start_float(state, samples, float_buffer, frames_in_buffer);
            break;
        case HDD_SPINDLE_RUNNING:
            hdd_audio_mix_spindle_loop_float(state, samples, float_buffer, frames_in_buffer);
            break;
        case HDD_SPINDLE_STOPPING:
            hdd_audio_mix_spindle_stop_float(state, samples, float_buffer, frames_in_buffer);
            break;
        case HDD_SPINDLE_STOPPED:
        default:
            break;
    }

    /* Seek sounds - only play when spindle is running */
    if (samples->seek_buffer && samples->seek_samples > 0 && 
        hdd_audio_mutex && state->spindle_state == HDD_SPINDLE_RUNNING) {
        thread_wait_mutex(hdd_audio_mutex);
        hdd_audio_mix_seek_float(state, float_buffer, frames_in_buffer);
        thread_release_mutex(hdd_audio_mutex);
    }
}

/* Process a single drive's audio in int16 mode */
static void
hdd_audio_process_drive_int16(hdd_audio_drive_state_t *state, int16_t *buffer, int frames_in_buffer)
{
    int profile_id = state->profile_id;
    
    if (profile_id <= 0 || profile_id >= HDD_AUDIO_PROFILE_MAX)
        return;
    
    hdd_audio_samples_t *samples = &profile_samples[profile_id];
    if (!samples->loaded)
        return;

    /* Handle spindle states for this drive */
    switch (state->spindle_state) {
        case HDD_SPINDLE_STARTING:
            hdd_audio_mix_spindle_start_int16(state, samples, buffer, frames_in_buffer);
            break;
        case HDD_SPINDLE_RUNNING:
            hdd_audio_mix_spindle_loop_int16(state, samples, buffer, frames_in_buffer);
            break;
        case HDD_SPINDLE_STOPPING:
            hdd_audio_mix_spindle_stop_int16(state, samples, buffer, frames_in_buffer);
            break;
        case HDD_SPINDLE_STOPPED:
        default:
            break;
    }

    /* Seek sounds - only play when spindle is running */
    if (samples->seek_buffer && samples->seek_samples > 0 && 
        hdd_audio_mutex && state->spindle_state == HDD_SPINDLE_RUNNING) {
        thread_wait_mutex(hdd_audio_mutex);
        hdd_audio_mix_seek_int16(state, buffer, frames_in_buffer);
        thread_release_mutex(hdd_audio_mutex);
    }
}

void
hdd_audio_callback(int16_t *buffer, int length)
{
    int frames_in_buffer = length / 2;

    if (sound_is_float) {
        float *float_buffer = (float *) buffer;

        /* Initialize buffer to silence */
        for (int i = 0; i < length; i++) {
            float_buffer[i] = 0.0f;
        }

        /* Process each active drive */
        for (int d = 0; d < active_drive_count; d++) {
            hdd_audio_process_drive_float(&drive_states[d], float_buffer, frames_in_buffer);
        }
    } else {
        /* Initialize buffer to silence */
        for (int i = 0; i < length; i++) {
            buffer[i] = 0;
        }

        /* Process each active drive */
        for (int d = 0; d < active_drive_count; d++) {
            hdd_audio_process_drive_int16(&drive_states[d], buffer, frames_in_buffer);
        }
    }
}
/* The front panel's hard disk LED, kept in step with the drive sounds.
   The LED is lit from the start to the end of each command. The sounds of
   a seek are heard only once the sound output has played what is queued
   ahead of them, so while a drive has an audio profile, the LED shows the
   activity of that long ago instead of the activity of now. */
#define HDD_LED_SLOTS 2048 /* One per millisecond */
#define HDD_LED_HOLD  20   /* The shortest flash that is seen */

volatile int hdd_audio_latency_ms = 0;

static volatile uint32_t hdd_led_slot[HDD_LED_SLOTS];
static volatile int      hdd_led_on    = 0; /* Bit 0: read, bit 1: write */
static volatile uint32_t hdd_led_since = 0;

static void
hdd_led_mark(uint32_t from, uint32_t to)
{
    if ((to - from) >= HDD_LED_SLOTS)
        from = to - HDD_LED_SLOTS + 1;
    for (uint32_t t = from; t != (to + 1); t++)
        hdd_led_slot[t % HDD_LED_SLOTS] = t;
}

void
hdd_led_activity(int write, int on)
{
    const uint32_t now = plat_get_ticks();
    const int      bit = write ? 2 : 1;
    const int      was = hdd_led_on;

    if (on) {
        if (!was)
            hdd_led_since = now;
        hdd_led_on = was | bit;
        hdd_led_mark(now, now);
    } else if (was & bit) {
        hdd_led_on = was & ~bit;
        if (!hdd_led_on)
            hdd_led_mark(hdd_led_since, now);
    }
}

/* The delay between a seek and its sound: until the sound thread starts
   the next block (half a block on average), then the output's own queue,
   measured by the sound backend where it can, else a typical value. */
static int
hdd_led_delay(void)
{
    int in_use = 0;

    for (int i = 0; i < active_drive_count; i++) {
        if (drive_states[i].profile_id > 0) {
            in_use = 1;
            break;
        }
    }
    if (!in_use)
        return 0;
    return (SOUNDBUFLEN * 500 / SOUND_FREQ) + ((hdd_audio_latency_ms > 0) ? hdd_audio_latency_ms : 70);
}

int
hdd_led_lit(void)
{
    const uint32_t t = plat_get_ticks() - (uint32_t) hdd_led_delay();

    if (hdd_led_on && ((int32_t) (t - hdd_led_since) >= 0))
        return 1;
    for (int i = 0; i < HDD_LED_HOLD; i++) {
        if (hdd_led_slot[(t - i) % HDD_LED_SLOTS] == (t - i))
            return 1;
    }
    return 0;
}

/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Emulation of the PC speaker.
 *
 * Authors: Sarah Walker, <https://pcem-emulator.co.uk/>
 *          Miran Grca, <mgrca8@gmail.com>
 *
 *          Copyright 2008-2019 Sarah Walker.
 *          Copyright 2016-2019 Miran Grca.
 */
#include <stdint.h>
#include <stdlib.h>
#include <86box/device.h>
#include <86box/snd_speaker.h>
#include <86box/sound.h>

int speaker_mute       = 0;
int speaker_gated      = 0;
int speaker_enable     = 0;
int was_speaker_enable = 0;

int speakon;

typedef struct speaker_t {
    int32_t buffer[MUSICBUFLEN];
    int     pos;

    uint8_t mode;
    double  count;
    double  acc;  /* level x time so far in the sample at pos */
    double  frac; /* how far into that sample acc covers */
    double  peak; /* highest level in that sample */
    int     zrun; /* silent samples written before it */
    double  hold; /* level to carry into the next sample, for a lone click */
    double  pend; /* level of a lone click still on at the end of the last sample */
} speaker_t;

speaker_t *speaker = NULL;

void
speaker_set_count(const uint8_t new_m, const int new_count)
{
    if (speaker != NULL) {
        speaker->mode  = new_m;
        speaker->count = (double) new_count;
    }
}

/* The speaker's level as the current gate, data and PIT state give it. */
static double
speaker_level(void)
{
    double amplitude = ((speaker->count / 256.0) * 10240.0) - 5120.0;
    int    enabled   = speaker_enable;

    if (amplitude > 5120.0)
        amplitude = 5120.0;

    if (speaker_gated && enabled) {
        if ((speaker->mode == 0) || (speaker->mode == 4))
            return amplitude;
        else if (speaker->count < 64.0)
            return 0xa00;
        else
            return speakon ? 0x1400 : 0;
    } else if (speaker->mode == 1)
        return enabled ? amplitude : 0;

    return enabled ? 0x1400 : 0;
}

/* Called before anything that changes the level: each sample is the
   average level over its whole length, so a pulse shorter than a sample
   (the BIOS memory count click is a few microseconds) always gives the same
   click, in proportion to its width, rather than one or two whole samples
   depending on where it falls. */
void
speaker_update(void)
{
    if (speaker != NULL) {
        double level = speaker_level();
        double frac  = music_get_pos_frac();

        if (speaker->pos > music_pos_global) {
            /* the buffer was handed out: start again from its beginning */
            speaker->acc  = 0.0;
            speaker->frac = 0.0;
            speaker->peak = 0.0;
            speaker->pend = 0.0;
        }

        while (speaker->pos < music_pos_global) {
            int32_t val;

            if (speaker->frac < 1.0) {
                speaker->acc += level * (1.0 - speaker->frac);
                if (level > speaker->peak)
                    speaker->peak = level;
            }
            val = (int32_t) speaker->acc;
            /* A lone click shorter than a sample (the 1993 AMI memory count
               click is about 14 us) after a stretch of silence plays as two
               whole samples at its level, wherever it falls against the
               sample boundaries, about as loud as the longer clicks of older
               BIOSes rather than scaled down by its width. Back-to-back
               pulses, as in PWM sample playback, keep the plain average. */
            if (speaker->hold > 0.0) {
                if (val == 0)
                    val = (int32_t) speaker->hold;
                speaker->hold = 0.0;
            } else if (speaker->pend > 0.0) {
                /* the click began in the last sample: if it ended in this
                   one, both samples play it at full level */
                if ((level == 0.0) && (speaker->acc < speaker->pend)) {
                    if (speaker->pos > 0)
                        speaker->buffer[speaker->pos - 1] = (int32_t) speaker->pend;
                    else
                        speaker->hold = speaker->pend;
                    val = (int32_t) speaker->pend;
                }
                speaker->pend = 0.0;
            } else if ((speaker->zrun >= 8) && (speaker->peak > 0.0)) {
                if (level == 0.0) {
                    /* the click began and ended in this sample */
                    val           = (int32_t) speaker->peak;
                    speaker->hold = speaker->peak;
                } else if (speaker->acc < speaker->peak)
                    speaker->pend = speaker->peak;
            }
            speaker->buffer[speaker->pos++] = val;
            speaker->zrun = val ? 0 : ((speaker->zrun < 1000) ? (speaker->zrun + 1) : 1000);
            speaker->acc  = 0.0;
            speaker->frac = 0.0;
            speaker->peak = 0.0;
        }

        if (frac > speaker->frac) {
            speaker->acc += level * (frac - speaker->frac);
            speaker->frac = frac;
            if (level > speaker->peak)
                speaker->peak = level;
        }

        was_speaker_enable = speaker_enable;
    }
}

void
speaker_get_buffer(int32_t *buffer, uint16_t len, void *priv)
{
    speaker_t *dev   = (speaker_t *) priv;

    double     val_l;
    double     val_r;

    speaker_update();

    if (!speaker_mute) {
        for (uint16_t c = 0; c < len * 2; c += 2) {
            val_l = val_r = (double) dev->buffer[c >> 1];
            /* Apply PC speaker volume and filters */
            if (filter_pc_speaker != NULL) {
                filter_pc_speaker(0, &val_l, filter_pc_speaker_p);
                filter_pc_speaker(1, &val_r, filter_pc_speaker_p);
            }
            buffer[c] += (int32_t) val_l;
            buffer[c + 1] += (int32_t) val_r;
        }
    }

    dev->pos = 0;
}

static void
speaker_close(void *priv)
{
    speaker_t *dev = (speaker_t *) priv;

    free(dev);

    speaker = NULL;
}

static void *
speaker_init(const device_t *info)
{
    speaker_t *dev     = (speaker_t *) calloc(1, sizeof(speaker_t));

    music_add_handler(speaker_get_buffer, dev);

    speaker_mute       = 0;
    speaker_gated      = 0;
    speaker_enable     = 0;
    was_speaker_enable = 0;

    dev->count         = 65535.0;

    speaker            = dev;

    return dev;
}

const device_t speaker_device = {
    .name          = "PC Speaker",
    .internal_name = "speaker",
    .flags         = 0,
    .local         = 0,
    .init          = speaker_init,
    .close         = speaker_close,
    .reset         = NULL,
    .available     = NULL,
    .speed_changed = NULL,
    .force_redraw  = NULL,
    .config        = NULL
};

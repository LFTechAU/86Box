/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Handling of the emulated machines.
 *
 * Authors: Miran Grca, <mgrca8@gmail.com>
 *          Fred N. van Kempen, <decwiz@yahoo.com>
 *
 *          Copyright 2016-2020 Miran Grca.
 *          Copyright 2017-2020 Fred N. van Kempen.
 */
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#define HAVE_STDARG_H
#include <86box/86box.h>
#include <86box/device.h>
#include <86box/timer.h>
#include <86box/cassette.h>
#include <86box/cartridge.h>
#include <86box/dma.h>
#include <86box/pic.h>
#include <86box/pit.h>
#include <86box/mem.h>
#include <86box/rom.h>
#include <86box/lpt.h>
#include <86box/serial.h>
#include <86box/gameport.h>
#include "cpu.h"
#include <86box/io.h>
#include <86box/video.h>
#include <86box/machine.h>
#include <86box/isamem.h>
#include <86box/isarom.h>
#include <86box/pci.h>
#include <86box/gdbstub.h>
#include <86box/plat_unused.h>

int bios_only = 0;
int machine;
// int AT, PCI;

#ifdef ENABLE_MACHINE_LOG
int machine_do_log = ENABLE_MACHINE_LOG;

static void
machine_log(const char *fmt, ...)
{
    va_list ap;

    if (machine_do_log) {
        va_start(ap, fmt);
        pclog_ex(fmt, ap);
        va_end(ap);
    }
}
#else
#    define machine_log(fmt, ...)
#endif

static int
machine_init_ex(int m)
{
    int ret = 0;

    is_pcjr = !!machine_has_bus(machine, MACHINE_BUS_SIDECAR);

    if (!bios_only) {
        machine_log("Initializing as \"%s\"\n", machine_getname(machine));

        machine_init_p1();

        machine_init_gpio();
        machine_init_gpio_acpi();

        machine_snd = NULL;

        is_vpc = 0;

        standalone_gameport_type = NULL;
        gameport_instance_id     = 0;

        /* Set up the architecture flags. */
#if 0
        AT = IS_AT(machine);
        PCI = IS_ARCH(machine, MACHINE_BUS_PCI);
#endif

        cpu_set();
        pc_speed_changed();

        /* Reset the memory state. */
        if (!dump_missing)
            mem_reset();
        smbase = is_am486dxl ? 0x00060000 : 0x00030000;

        if (cassette_enable)
            device_add(&cassette_device);

        cart_reset();

        /* Prepare some video-related things if we're using internal
           or no video. */
        video_pre_reset(gfxcard[0]);

        /* Reset any ISA memory cards. */
        isamem_reset();

#if 0
        /* Reset any ISA ROM cards. */
        isarom_reset();
#endif

        /* Reset the fast off stuff. */
        cpu_fast_off_reset();

        pci_flags = 0x00000000;

        if (machines[m].nvr_device)
            device_add_params(machines[m].nvr_device, (void *) (uintptr_t) machines[m].nvr_params);
    }

    /* All good, boot the machine! */
    if (machines[m].init)
        ret = machines[m].init(&machines[m]);

    if (bios_only || !ret)
        return ret;

    video_post_reset();

    return ret;
}

/* Front panel turbo switch: 1 is turbo. The user interface sets it and the
   chipset reads it, so it only needs to be a plain flag. */
volatile int machine_turbo_switch = 1;

static int turbo_switch_present = 0;

volatile int machine_speed_pct = 100;

/* Set by a board's init when its LFTech BIOS is selected: only then does it
   get the LFTech extras (Turbo, the front panel, DRAM timing). */
int machine_lftech = 0;

char front_panel_hi[4] = "";
char front_panel_lo[4] = "";

/* The CPU clock in whole MHz, as a front panel display shows it (66 for a
   66.67 MHz part). */
int
machine_get_cpu_mhz(void)
{
    const CPU *c = cpu_s;

    return (c != NULL) ? (int) (c->rspeed / 1000000) : 0;
}

/* As the SiS 85C460/461 de-turbo (sis_85c4xx.c): while the front panel
   Turbo switch is off, the processor clock control throttle holds the CPU
   part of the time, so it runs at the De-Turbo Speed picked in LFTech
   Features Setup (1/3 unless set) against the timers and the clock. The
   BIOS's Ctrl+Alt+minus/plus is left alone: Turbo is only switched from
   the front panel. The timer polls, which also picks up the switch from
   the user interface thread. */
static void
machine_deturbo_timer(void *priv)
{
    pc_timer_t *timer  = (pc_timer_t *) priv;
    int         period = 3;
    int         run    = 0;

    machine_speed_pct = 100;
    if (!machine_turbo_switch)
        machine_speed_pct = lftech_deturbo_throttle(&period, &run);
    if (run >= period)
        run = 0;

    if ((cpu_throttle_run != run) || (run && (cpu_throttle_period != period))) {
        cpu_throttle_period = period;
        cpu_throttle_break  = 0;
        cpu_throttle_run    = run;
    }

    timer_advance_u64(timer, (uint64_t) (1000.0 * TIMER_USEC));
}

static void *
machine_deturbo_init(UNUSED(const device_t *info))
{
    pc_timer_t *timer = (pc_timer_t *) calloc(1, sizeof(pc_timer_t));

    machine_set_turbo_switch_present(1);

    timer_add(timer, machine_deturbo_timer, timer, 0);
    timer_set_delay_u64(timer, (uint64_t) (1000.0 * TIMER_USEC));

    return timer;
}

static void
machine_deturbo_close(void *priv)
{
    pc_timer_t *timer = (pc_timer_t *) priv;

    timer_disable(timer);
    cpu_throttle_run  = 0;
    machine_speed_pct = 100;
    free(timer);
}

const device_t machine_deturbo_device = {
    .name          = "Turbo switch",
    .internal_name = "machine_deturbo",
    .flags         = 0,
    .local         = 0,
    .init          = machine_deturbo_init,
    .close         = machine_deturbo_close,
    .reset         = NULL,
    .available     = NULL,
    .speed_changed = NULL,
    .force_redraw  = NULL,
    .config        = NULL
};

void
machine_set_turbo_switch_present(int present)
{
    turbo_switch_present = present;
}

int
machine_has_turbo_switch(void)
{
    return turbo_switch_present;
}

/* The POST memory count, for the "Extra DRAM wait" setting. Some LFTech
   BIOSes (the OPTi 495SX's) turn the external cache on early in POST, so
   the count would run at cache speed whatever the setting. A real count
   tests each block of DRAM, so while the AMI BIOS counts (POST codes 4Fh
   to 51h, base and extended memory) the cache is left out of the timing.
   This only happens once per power-on: the count ends with code 52h, and
   after that software that writes port 80h (as a delay) changes nothing. */
static int machine_memcount_armed;

static void
machine_memcount_post(UNUSED(uint16_t port), uint8_t val, UNUSED(void *priv))
{
    int on = machine_memcount_armed && (val >= 0x4f) && (val <= 0x51);

    if (machine_memcount_armed && (val == 0x52))
        machine_memcount_armed = 0;

    if (cpu_mem_count_uncached != on) {
        cpu_mem_count_uncached = on;
        cpu_update_waitstates();
    }
}

void
machine_init(void)
{
    bios_only            = 0;
    cpu_mem_extra_cycles = 0;
    turbo_switch_present = 0;
    machine_speed_pct    = 100;
    machine_lftech       = 0;

    cpu_mem_count_uncached = 0;

    machine_set_p1_default(machines[machine].kbc_p1);
    machine_set_ps2();

    /* Create the GDB Stub socket before gdbstub_cpu_init looks for it.
       This is done outside of machine_init_ex so we only occupy the socket
       if we're actually starting a machine. */
    static int gdbstub_started = 0;
    if (!gdbstub_started) {
        gdbstub_started = 1;
        gdbstub_init();
    }

    (void) machine_init_ex(machine);

    machine_memcount_armed = 1;
    if (machine_lftech)
        io_sethandler(0x0080, 1, NULL, NULL, NULL, machine_memcount_post, NULL, NULL, NULL);
}

int
machine_available(int m)
{
    int             ret = 0;
    const device_t *dev = machine_get_device(m);

    if (dev != NULL)
        ret = machine_device_available(dev);
    /*
       Only via machine_init_ex() if the device is NULL or
       it lacks a CONFIG_BIOS field (or the CONFIG_BIOS field
       is not the first in list.
     */
    if (ret == 0) {
        bios_only = 1;

        ret = machine_init_ex(m);

        bios_only = 0;
    } else if (ret == -2)
        ret = 0;

    return !!ret;
}

void
pit_irq0_timer(int new_out, int old_out, UNUSED(void *priv))
{
    if (new_out && !old_out)
        picint(1);

    if (!new_out)
        picintc(1);
}

/* The system devices every machine has, but for those flags says this one doesn't (MACHINE_NONSTANDARD_*). */
void
machine_common_init_ex(UNUSED(const machine_t *model), uint32_t flags)
{
    /* System devices first. */
    pic_init();
    dma_core_init();
    if (!(flags & MACHINE_NONSTANDARD_DMA))
        dma_init();

    if (!(flags & MACHINE_NONSTANDARD_PIT)) {
        uint8_t cpu_requires_fast_pit = is486 || (!is286 && is8086 && (cpu_s->rspeed >= 8000000));
        cpu_requires_fast_pit         = cpu_requires_fast_pit && !cpu_16bitbus;

        int pit_type = IS_AT(machine) ? PIT_8254 : PIT_8253;
        /* Select fast PIT if needed */
        if (((pit_mode == -1) && cpu_requires_fast_pit) || (pit_mode == 1))
            pit_type += 2;

        pit_common_init(pit_type, pit_irq0_timer, NULL);
    }
}

void
machine_common_init(const machine_t *model)
{
    machine_common_init_ex(model, 0);
}

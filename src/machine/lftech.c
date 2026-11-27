/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          LFTech Features Setup: the settings page of the LFTech BIOSes
 *          and what the board does with its settings.
 *
 *          The BIOS's "LFTECH FEATURES SETUP" menu item writes 'L' to port
 *          EEh; the board then copies the setup page program
 *          (lftech_setup.asm) to 8000:0000 for the BIOS to call. The page
 *          keeps its settings in two CMOS bytes, read back here:
 *
 *            byte A: 10xx dddd  bit 3 logo off, bit 2 spin-up wait off,
 *                               bits 1:0 de-turbo speed
 *            byte B: 101i iiii  extra DRAM wait, index into dram_tab,
 *                               1Fh = Auto (from the CPU bus speed)
 *
 *          A read of port EEh tells the BIOS's logo code whether to draw
 *          the logo (1Ah, as INT 10h AX=1A00h answers on VGA) or not.
 *
 *          Copyright 2026 LFTech.
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#define HAVE_STDARG_H
#include <86box/86box.h>
#include "cpu.h"
#include <86box/timer.h>
#include <86box/io.h>
#include <86box/mem.h>
#include <86box/device.h>
#include <86box/nvr.h>
#include <86box/ini.h>
#include <86box/config.h>
#include <86box/machine.h>
#include <86box/plat_unused.h>

#include "lftech_setup_rom.h"

#define LFTECH_PORT     0x00ee
#define LFTECH_SETUP_AT 0x00080000

static const int dram_tab[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 14, 16, 20, 24, 32, 48, 64 };
#define DRAM_N    ((int) (sizeof(dram_tab) / sizeof(dram_tab[0])))
#define DRAM_AUTO 0x1f

int lftech_deturbo_speed = 0; /* 0 = 1/3, 1 = 1/2, 2 = 2/3, 3 = 8 MHz */
int lftech_spinup_wait   = 1;
int lftech_logo          = 1;

static struct {
    uint8_t    cmos_a;
    uint8_t    cmos_b;
    int        dram_max;
    int        dram_def;
    int        dram_33;
    int        dram;
    pc_timer_t timer;
} lft;

/* DRAM chips take the same time whatever the bus speed, so the wait in bus
   clocks grows with the bus: the board's own 33 MHz value (which matches a
   real DX-33's memory count), scaled to this CPU's bus. */
static int
dram_auto(void)
{
    double mhz    = cpu_busspeed / 1000000.0;
    int    clocks = (int) (lft.dram_33 * mhz / 33.333 + 0.5);

    if (clocks > dram_tab[lft.dram_max])
        clocks = dram_tab[lft.dram_max];
    return clocks;
}

static int
dram_index(int clocks)
{
    int i;

    for (i = DRAM_N - 1; i > 0; i--)
        if (dram_tab[i] <= clocks)
            break;

    return i;
}

/* Read the settings from CMOS; write the defaults there if they are not
   valid (a new CMOS, or one from before the setup page). */
static void
lftech_read_cmos(int fix)
{
    uint8_t a = nvr_peek(lft.cmos_a);
    uint8_t b = nvr_peek(lft.cmos_b);
    int     dram;
    int     clocks;

    if (((a & 0xf0) != 0x80) || ((b & 0xe0) != 0xa0) || (((b & 0x1f) >= DRAM_N) && ((b & 0x1f) != DRAM_AUTO))) {
        if (!fix)
            return;
        a = 0x80;
        b = 0xa0 | lft.dram_def;
        nvr_poke(lft.cmos_a, a);
        nvr_poke(lft.cmos_b, b);
    }

    lftech_deturbo_speed = a & 0x03;
    lftech_spinup_wait   = !(a & 0x04);
    lftech_logo          = !(a & 0x08);

    dram = b & 0x1f;
    if (dram == DRAM_AUTO)
        clocks = dram_auto();
    else
        clocks = dram_tab[(dram > lft.dram_max) ? lft.dram_max : dram];
    if (clocks != lft.dram) {
        lft.dram             = clocks;
        cpu_mem_extra_cycles = clocks;
        cpu_update_waitstates();
    }
}

static void
lftech_timer(UNUSED(void *priv))
{
    lftech_read_cmos(0);
    timer_advance_u64(&lft.timer, (uint64_t) (20000.0 * TIMER_USEC));
}

static void
lftech_write(UNUSED(uint16_t port), uint8_t val, UNUSED(void *priv))
{
    /* Save and Restart on the setup page: a reset like the Reset button,
       so the chipset (its cache and DRAM settings) starts over too. */
    if (val == 'R') {
        pc_reset_button();
        return;
    }

    if (val != 'L')
        return;

    /* Copy the setup page program in, with this board's header. */
    for (uint32_t i = 0; i < sizeof(lftech_setup_rom); i++) {
        uint8_t b = lftech_setup_rom[i];

        switch (i) {
            case 0x0a:
                b = lft.cmos_a;
                break;
            case 0x0b:
                b = lft.cmos_b;
                break;
            case 0x0c:
                b = (uint8_t) lft.dram_max;
                break;
            case 0x0d:
                b = (uint8_t) dram_auto();
                break;
            default:
                break;
        }
        mem_writeb_phys(LFTECH_SETUP_AT + i, b);
    }
}

static uint8_t
lftech_read(UNUSED(uint16_t port), UNUSED(void *priv))
{
    return lftech_logo ? 0x1a : 0x00;
}

static void *
lftech_init(UNUSED(const device_t *info))
{
    lft.dram = -1;
    lftech_read_cmos(1);

    io_sethandler(LFTECH_PORT, 1, lftech_read, NULL, NULL, lftech_write, NULL, NULL, NULL);
    timer_add(&lft.timer, lftech_timer, NULL, 0);
    timer_set_delay_u64(&lft.timer, (uint64_t) (20000.0 * TIMER_USEC));

    return &lft;
}

static void
lftech_close(UNUSED(void *priv))
{
    timer_disable(&lft.timer);
    lftech_deturbo_speed = 0;
    lftech_spinup_wait   = 1;
    lftech_logo          = 1;
}

static const device_t lftech_setup_device = {
    .name          = "LFTech Features Setup",
    .internal_name = "lftech_setup",
    .flags         = 0,
    .local         = 0,
    .init          = lftech_init,
    .close         = lftech_close,
    .reset         = NULL,
    .available     = NULL,
    .speed_changed = NULL,
    .force_redraw  = NULL,
    .config        = NULL
};

/* Called by an LFTech board with its LFTech BIOS: dram_def is the board's
   extra DRAM wait in clocks for a 33 MHz bus (Auto scales it to the CPU's
   bus; a machine setting from before the setup page is taken over instead),
   dram_max the most it offers, and cmos_a/cmos_b the two CMOS bytes its
   BIOS leaves free. */
void
lftech_setup_add(int dram_def, int dram_max, uint8_t cmos_a, uint8_t cmos_b)
{
    const device_t *dev = machine_get_device(machine);
    int             old = -1;

    if ((dev != NULL) && (dev->name != NULL))
        old = config_get_int((char *) dev->name, "dram_extra", -1);

    lft.cmos_a   = cmos_a;
    lft.cmos_b   = cmos_b;
    lft.dram_33  = dram_def;
    lft.dram_max = dram_index(dram_max);
    if (old >= 0) {
        lft.dram_def = dram_index(old);
        if (lft.dram_def > lft.dram_max)
            lft.dram_def = lft.dram_max;
    } else
        lft.dram_def = DRAM_AUTO;

    device_add(&lftech_setup_device);
}

/* The de-turbo hold for the selected speed: the processor runs *run parts
   of every *period, and the speed in percent is returned. */
int
lftech_deturbo_throttle(int *period, int *run)
{
    int mhz;

    switch (lftech_deturbo_speed) {
        default:
        case 0:
            *period = 3;
            *run    = 1;
            break;
        case 1:
            *period = 2;
            *run    = 1;
            break;
        case 2:
            *period = 3;
            *run    = 2;
            break;
        case 3:
            mhz     = machine_get_cpu_mhz();
            *period = (mhz + 4) / 8;
            if (*period < 1)
                *period = 1;
            *run = 1;
            break;
    }

    return (*run * 100) / *period;
}

/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Implementation of Socket 3/4 machines.
 *
 * Authors: Miran Grca, <mgrca8@gmail.com>
 *
 *          Copyright 2016-2025 Miran Grca.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <86box/86box.h>
#include "cpu.h"
#include <86box/mem.h>
#include <86box/io.h>
#include <86box/rom.h>
#include <86box/pci.h>
#include <86box/device.h>
#include <86box/chipset.h>
#include <86box/fdc_ext.h>
#include <86box/hdc.h>
#include <86box/hdc_ide.h>
#include <86box/timer.h>
#include <86box/fdd.h>
#include <86box/fdc.h>
#include <86box/keyboard.h>
#include <86box/flash.h>
#include <86box/nvr.h>
#include <86box/scsi_ncr53c8xx.h>
#include <86box/sio.h>
#include <86box/timer.h>
#include <86box/video.h>
#include <86box/machine.h>

/* OPTi 571 */
static const device_config_t pat45pv_config[] = {
    // clang-format off
    MACHINE_LFTECH_BIOS_CONFIG("AMI 08/08/93 by LFTech"),
    {
        .name        = "cache_size",
        .description = "External cache",
        .type        = CONFIG_SELECTION,
        .default_int = 256,
        .selection   = { { .description = "64 KB", .value = 64 },
                         { .description = "128 KB", .value = 128 },
                         { .description = "256 KB", .value = 256 },
                         { .description = "512 KB", .value = 512 },
                         { .description = "" } }
    },
    { .name = "", .description = "", .type = CONFIG_END }
    // clang-format on
};

const device_t pat45pv_device = {
    .name          = "TMC PAT45PV",
    .internal_name = "pat45pv",
    .flags         = 0,
    .local         = 0,
    .init          = NULL,
    .close         = NULL,
    .reset         = NULL,
    .available     = NULL,
    .speed_changed = NULL,
    .force_redraw  = NULL,
    .config        = pat45pv_config
};

int
machine_at_pat45pv_init(const machine_t *model)
{
    int ret;

    ret = bios_load_linear(machine_get_config_int("bios_lftech") ?
                               "roms/machines/pat45pv/tmc-pat45pv-lftech.bin" :
                               "roms/machines/pat45pv/tmc-pat45pv.bin",
                           0x000f0000, 65536, 0);

    if (bios_only || !ret)
        return ret;

    machine_at_common_init(model);

    /* The LFTech extras (Turbo, the front panel, DRAM timing) are only on
       the LFTech BIOS; the stock BIOS runs the board as 86Box has it. */
    machine_lftech = machine_get_config_int("bios_lftech") != 0;

    /* The cache size the BIOS reads from the chipset (register 02h bits
       7:5, 64 KB << n); on the LFTech BIOS only, like the other extras. */
    if (machine_lftech) {
        int kb = machine_get_config_int("cache_size");
        int n  = (kb >= 512) ? 3 : (kb >= 256) ? 2 : (kb >= 128) ? 1 : 0;

        device_add_params(&opti5x7_device, (void *) (uintptr_t) (0x100 | (n << 5)));
    } else
        device_add(&opti5x7_device);

    /* Extra DRAM Wait in the LFTech BIOS's LFTech Features Setup (the
       board's 33 MHz value, the most it offers, the two CMOS bytes). */
    cpu_mem_extra_cycles = 0;
    if (machine_lftech)
        lftech_setup_add(3, 64, 0x70, 0x71);
    cpu_update_waitstates();

    device_add_params(machine_get_kbc_device(machine), (void *) model->kbc_params);
    if (machine_lftech)
        device_add(&machine_deturbo_device);

    if (fdc_current[0] == FDC_INTERNAL)
        device_add(&fdc_at_device);

    return ret;
}
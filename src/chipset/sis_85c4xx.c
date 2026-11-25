/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Emulation of the SiS 85c401/85c402, 85c460, 85c461, and
 *          85c407/85c471 chipsets.
 *
 * Authors: Miran Grca, <mgrca8@gmail.com>
 *
 *          Copyright 2019-2020 Miran Grca.
 */
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#define HAVE_STDARG_H
#include <86box/86box.h>
#include "cpu.h"
#include "x86.h"
#include "x86seg.h"
#include <86box/timer.h>
#include <86box/io.h>
#include <86box/device.h>
#include <86box/plat_unused.h>
#include <86box/port_92.h>
#include <86box/mem.h>
#include <86box/smram.h>
#include <86box/pic.h>
#include <86box/plat_fallthrough.h>
#include <86box/keyboard.h>
#include <86box/machine.h>
#include <86box/chipset.h>

typedef struct ram_bank_t {
    uint32_t      virt_base;
    uint32_t      virt_size;
    uint32_t      phys_base;
    uint32_t      phys_size;

    mem_mapping_t mapping;
} ram_bank_t;

typedef struct sis_85c4xx_t {
    uint8_t       cur_reg;
    uint8_t       tries;
    uint8_t       reg_base;
    uint8_t       reg_last;
    uint8_t       reg_00;
    uint8_t       is_471;
    uint8_t       ram_banks_val;
    uint8_t       force_flush;
    uint8_t       shadowed;
    uint8_t       smram_enabled;
    uint8_t       pad;
    uint8_t       force_cache_size;
    uint8_t       post_romcs_e;   /* board ROM is 128 KB: decode E0000 during POST */
    uint8_t       romcs_e_active;
    uint8_t       cache_size_code;
    uint8_t       regs[39];
    uint8_t       scratch[2];
    uint32_t      mem_state[8];
    ram_bank_t    ram_banks[8];
    smram_t *     smram;
    port_92_t *   port_92;

    /* 85C460/461 external cache data SRAM, for the cache sizing mode. */
    uint8_t *     cache_sram;
    uint32_t      cache_sram_mask;
    mem_mapping_t cache_sram_mapping;
    int           cache_sizing;

    /* 85C460/461 de-turbo: the chipset holds the CPU off the bus for 4 or
       8 us out of every 12 us. */
    pc_timer_t    deturbo_timer;
    int           has_deturbo;
} sis_85c4xx_t;

static uint8_t ram_4xx[64] = { 0x00, 0x00, 0x01, 0x00, 0x02, 0x00, 0x03, 0x00,
                               0x04, 0x00, 0x05, 0x00, 0x0b, 0x00, 0x00, 0x00,
                               0x19, 0x00, 0x06, 0x00, 0x14, 0x00, 0x00, 0x00,
                               0x15, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                               0x1b, 0x00, 0x00, 0x00, 0x16, 0x00, 0x00, 0x00,
                               0x17, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                               0x1e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                               0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
static uint8_t ram_471[64] = { 0x00, 0x00, 0x01, 0x01, 0x02, 0x20, 0x09, 0x09,
                               0x04, 0x04, 0x05, 0x05, 0x0b, 0x0b, 0x0b, 0x0b,
                               0x13, 0x21, 0x06, 0x06, 0x0d, 0x0d, 0x0d, 0x0d,
                               0x0e, 0x0e, 0x0e, 0x0e, 0x0e, 0x0e, 0x0e, 0x0e,
                               0x1b, 0x1b, 0x1b, 0x1b, 0x0f, 0x0f, 0x0f, 0x0f,
                               0x17, 0x17, 0x17, 0x17, 0x17, 0x17, 0x17, 0x17,
                               0x3d, 0x3d, 0x3d, 0x3d, 0x3d, 0x3d, 0x3d, 0x3d,
                               0x3d, 0x3d, 0x3d, 0x3d, 0x3d, 0x3d, 0x3d, 0x3d };
static uint8_t ram_asus[64] = { 0x00, 0x00, 0x01, 0x10, 0x10, 0x20, 0x03, 0x11,
                                0x11, 0x05, 0x05, 0x12, 0x12, 0x13, 0x13, 0x13,
                                0x13, 0x21, 0x06, 0x14, 0x14, 0x15, 0x15, 0x15,
                                0x15, 0x1d, 0x1d, 0x1d, 0x1d, 0x1d, 0x1d, 0x1d,
                                0x1d, 0x16, 0x16, 0x16, 0x16, 0x17, 0x17, 0x17,
                                0x17, 0x1e, 0x1e, 0x1e, 0x1e, 0x1e, 0x1e, 0x1e,
                                0x1e, 0x1f, 0x1f, 0x1f, 0x1f, 0x1f, 0x1f, 0x1f,
                                0x1f, 0x1f, 0x1f, 0x1f, 0x1f, 0x1f, 0x1f, 0x1f };
static uint8_t ram_tg486g[64] = { 0x10, 0x10, 0x10, 0x10, 0x10, 0x11, 0x11, 0x11,
                                  0x11, 0x12, 0x12, 0x12, 0x12, 0x13, 0x13, 0x13,
                                  0x13, 0x14, 0x14, 0x14, 0x14, 0x15, 0x15, 0x15,
                                  0x15, 0x1d, 0x1d, 0x1d, 0x1d, 0x1d, 0x1d, 0x1d,
                                  0x1d, 0x16, 0x16, 0x16, 0x16, 0x17, 0x17, 0x17,
                                  0x17, 0x1e, 0x1e, 0x1e, 0x1e, 0x1e, 0x1e, 0x1e,
                                  0x1e, 0x1f, 0x1f, 0x1f, 0x1f, 0x1f, 0x1f, 0x1f,
                                  0x1f, 0x1f, 0x1f, 0x1f, 0x1f, 0x1f, 0x1f, 0x1f };

static uint32_t banks_471[64][4] = { { 0x00100000, 0x00000000, 0x00000000, 0x00000000 }, /* 0x00 */
                                     { 0x00100000, 0x00100000, 0x00000000, 0x00000000 },
                                     { 0x00100000, 0x00100000, 0x00200000, 0x00000000 },
                                     { 0x00100000, 0x00100000, 0x00400000, 0x00000000 },
                                     { 0x00100000, 0x00100000, 0x00200000, 0x00400000 },
                                     { 0x00100000, 0x00100000, 0x00400000, 0x00400000 },
                                     { 0x00100000, 0x00100000, 0x01000000, 0x00000000 },
                                     { 0x00200000, 0x00000000, 0x00000000, 0x00000000 },
                                     { 0x00200000, 0x00200000, 0x00000000, 0x00000000 }, /* 0x08 */
                                     { 0x00200000, 0x00400000, 0x00000000, 0x00000000 },
                                     { 0x00200000, 0x00200000, 0x00400000, 0x00000000 },
                                     { 0x00200000, 0x00200000, 0x00400000, 0x00400000 },
                                     { 0x00200000, 0x01000000, 0x00000000, 0x00000000 },
                                     { 0x00200000, 0x00200000, 0x01000000, 0x00000000 },
                                     { 0x00200000, 0x00200000, 0x00400000, 0x01000000 },
                                     { 0x00200000, 0x00200000, 0x01000000, 0x01000000 },
                                     { 0x00400000, 0x00000000, 0x00000000, 0x00000000 }, /* 0x10 */
                                     { 0x00400000, 0x00400000, 0x00000000, 0x00000000 },
                                     { 0x00400000, 0x00400000, 0x00400000, 0x00000000 },
                                     { 0x00400000, 0x00400000, 0x00400000, 0x00400000 },
                                     { 0x00400000, 0x01000000, 0x00000000, 0x00000000 },
                                     { 0x00400000, 0x00400000, 0x01000000, 0x00000000 },
                                     { 0x00400000, 0x01000000, 0x01000000, 0x00000000 },
                                     { 0x00400000, 0x00400000, 0x01000000, 0x01000000 },
                                     { 0x00800000, 0x00000000, 0x00000000, 0x00000000 }, /* 0x18 */
                                     { 0x00800000, 0x00800000, 0x00000000, 0x00000000 },
                                     { 0x00800000, 0x00800000, 0x00800000, 0x00000000 },
                                     { 0x00800000, 0x00800000, 0x00800000, 0x00800000 },
                                     { 0x01000000, 0x00000000, 0x00000000, 0x00000000 },
                                     { 0x01000000, 0x01000000, 0x00000000, 0x00000000 },
                                     { 0x01000000, 0x01000000, 0x01000000, 0x00000000 },
                                     { 0x01000000, 0x01000000, 0x01000000, 0x01000000 },
                                     { 0x00100000, 0x00400000, 0x00000000, 0x00000000 }, /* 0x20 */
                                     { 0x00100000, 0x01000000, 0x00000000, 0x00000000 },
                                     { 0x00100000, 0x04000000, 0x00000000, 0x00000000 },
                                     { 0x00400000, 0x00800000, 0x00000000, 0x00000000 },
                                     { 0x00400000, 0x04000000, 0x00000000, 0x00000000 },
                                     { 0x00400000, 0x00400000, 0x04000000, 0x00000000 },
                                     { 0x01000000, 0x04000000, 0x00000000, 0x00000000 },
                                     { 0x01000000, 0x01000000, 0x04000000, 0x00000000 },
                                     { 0x04000000, 0x00000000, 0x00000000, 0x00000000 }, /* 0x28 */
                                     { 0x04000000, 0x04000000, 0x00000000, 0x00000000 },
                                     { 0x00400000, 0x02000000, 0x00000000, 0x00000000 },
                                     { 0x00400000, 0x02000000, 0x02000000, 0x00000000 },
                                     { 0x00400000, 0x00400000, 0x02000000, 0x00000000 },
                                     { 0x00400000, 0x00400000, 0x02000000, 0x02000000 },
                                     { 0x01000000, 0x02000000, 0x00000000, 0x00000000 },
                                     { 0x01000000, 0x02000000, 0x02000000, 0x00000000 },
                                     { 0x01000000, 0x01000000, 0x02000000, 0x00000000 }, /* 0x30 */
                                     { 0x01000000, 0x01000000, 0x02000000, 0x02000000 },
                                     { 0x02000000, 0x00000000, 0x00000000, 0x00000000 },
                                     { 0x02000000, 0x02000000, 0x00000000, 0x00000000 },
                                     { 0x02000000, 0x02000000, 0x02000000, 0x00000000 },
                                     { 0x02000000, 0x02000000, 0x02000000, 0x02000000 },
                                     { 0x00400000, 0x00800000, 0x00800000, 0x00000000 },
                                     { 0x00400000, 0x00800000, 0x00800000, 0x00800000 },
                                     { 0x00400000, 0x00400000, 0x00800000, 0x00000000 }, /* 0x38 */
                                     { 0x00400000, 0x00400000, 0x00800000, 0x00800000 },
                                     { 0x00800000, 0x01000000, 0x00000000, 0x00000000 },
                                     { 0x00800000, 0x00800000, 0x00800000, 0x01000000 },
                                     { 0x00800000, 0x00800000, 0x01000000, 0x00000000 },
                                     { 0x00800000, 0x00800000, 0x01000000, 0x01000000 },
                                     { 0x00800000, 0x00800000, 0x02000000, 0x00000000 },
                                     { 0x00800000, 0x00800000, 0x02000000, 0x02000000 } };

static uint32_t
sis_85c471_get_row(ram_bank_t *dev, uint32_t addr)
{
    uint32_t ret = 0x00000000;

    switch (dev->virt_size) {
        case 0x00100000:
        case 0x00200000:
            ret |= (addr >> 13) & 0x00000001;
            ret |= ((addr >> 12) & 0x00000001) << 1;
            ret |= ((addr >> 14) & 0x0000003f) << 2;
            ret |= ((addr >> 11) & 0x00000001) << 8;
            ret |= ((addr >> 20) & 0x00000001) << 9;
            ret |= ((addr >> 22) & 0x00000001) << 10;
            ret |= ((addr >> 24) & 0x00000001) << 11;
            break;
        case 0x00400000:
        case 0x00800000:
            ret |= (addr >> 13) & 0x00000001;
            ret |= ((addr >> 12) & 0x00000001) << 1;
            ret |= ((addr >> 14) & 0x000000ff) << 2;
            ret |= ((addr >> 22) & 0x00000001) << 10;
            ret |= ((addr >> 24) & 0x00000001) << 11;
            break;
        case 0x01000000:
        case 0x02000000:
        case 0x04000000:
            ret |= (addr >> 13) & 0x00000001;
            ret |= ((addr >> 22) & 0x00000001) << 1;
            ret |= ((addr >> 14) & 0x000000ff) << 2;
            ret |= ((addr >> 23) & 0x00000001) << 10;
            ret |= ((addr >> 24) & 0x00000001) << 11;
            break;
    }

    return ret;
}

static uint32_t
sis_85c471_get_col(ram_bank_t *dev, uint32_t addr)
{
    uint32_t ret = 0x00000000;

    switch (dev->virt_size) {
        case 0x00100000:
        case 0x00200000:
            ret |= (addr >> 3) & 0x00000001;
            ret |= ((addr >> 2) & 0x00000001) << 1;
            ret |= ((addr >> 4) & 0x0000003f) << 2;
            ret |= ((addr >> 10) & 0x00000001) << 8;
            ret |= ((addr >> 21) & 0x00000001) << 9;
            ret |= ((addr >> 23) & 0x00000001) << 10;
            ret |= ((addr >> 25) & 0x00000001) << 11;
            break;
        case 0x00400000:
        case 0x00800000:
            ret |= (addr >> 3) & 0x00000001;
            ret |= ((addr >> 2) & 0x00000001) << 1;
            ret |= ((addr >> 4) & 0x000000ff) << 2;
            ret |= ((addr >> 23) & 0x00000001) << 10;
            ret |= ((addr >> 25) & 0x00000001) << 11;
            break;
        case 0x01000000:
        case 0x02000000:
        case 0x04000000:
            ret |= (addr >> 3) & 0x00000001;
            ret |= ((addr >> 2) & 0x00000001) << 1;
            ret |= ((addr >> 4) & 0x000001ff) << 2;
            ret |= ((addr >> 25) & 0x00000001) << 11;
            break;
    }

    return ret;
}

static uint32_t
sis_85c471_set_row(ram_bank_t *dev, uint32_t addr)
{
    uint32_t ret = 0x00000000;

    switch (dev->phys_size) {
        case 0x00100000:
            ret = (addr & 0x1ff) << 11;
            break;
        case 0x00200000:
            ret = (addr & 0x3ff) << 11;
            break;
        case 0x00400000:
            ret = (addr & 0x3ff) << 12;
            break;
        case 0x00800000:
            ret = (addr & 0x7ff) << 12;
            break;
        case 0x01000000:
            ret = (addr & 0x7ff) << 13;
            break;
        case 0x02000000:
            ret = (addr & 0xfff) << 13;
            break;
        case 0x04000000:
            ret = (addr & 0xfff) << 14;
            break;
    }

    return ret;
}

static uint32_t
sis_85c471_set_col(ram_bank_t *dev, uint32_t addr)
{
    uint32_t ret = 0x00000000;

    switch (dev->phys_size) {
        case 0x00100000:
        case 0x00200000:
            ret = (addr & 0x1ff) << 2;
            break;
        case 0x00400000:
        case 0x00800000:
            ret = (addr & 0x3ff) << 2;
            break;
        case 0x01000000:
        case 0x02000000:
            ret = (addr & 0x7ff) << 2;
            break;
        case 0x04000000:
            ret = (addr & 0xfff) << 2;
            break;
    }

    return ret;
}

uint8_t reg09 = 0x00;

static uint8_t
sis_85c471_read_ram(uint32_t addr, void *priv)
{
    ram_bank_t *dev = (ram_bank_t *) priv;
    uint32_t    rel = addr - dev->virt_base;
    uint8_t     ret = 0xff;

    uint32_t row = sis_85c471_set_row(dev, sis_85c471_get_row(dev, rel));
    uint32_t col = sis_85c471_set_col(dev, sis_85c471_get_col(dev, rel));
    uint32_t dw  = rel & 0x00000003;
    rel = row | col | dw;

    addr = (rel + dev->phys_base);

    if ((addr < (mem_size << 10)) && (rel < dev->phys_size))
        ret = ram[addr];

    return ret;
}

static uint16_t
sis_85c471_read_ramw(uint32_t addr, void *priv)
{
    ram_bank_t *dev = (ram_bank_t *) priv;
    uint32_t    rel = addr - dev->virt_base;
    uint16_t    ret = 0xffff;

    /* An access that crosses a doubleword is two bus cycles, each with its
       own row and column (a 386SX fetches code this way). */
    if ((rel & 0x03) == 0x03)
        return sis_85c471_read_ram(addr, priv) | (sis_85c471_read_ram(addr + 1, priv) << 8);

    uint32_t row = sis_85c471_set_row(dev, sis_85c471_get_row(dev, rel));
    uint32_t col = sis_85c471_set_col(dev, sis_85c471_get_col(dev, rel));
    uint32_t dw  = rel & 0x00000003;
    rel = row | col | dw;

    addr = (rel + dev->phys_base);

    if ((addr < (mem_size << 10)) && (rel < dev->phys_size))
        ret = *(uint16_t *) &(ram[addr]);

    return ret;
}

static uint32_t
sis_85c471_read_raml(uint32_t addr, void *priv)
{
    ram_bank_t *dev = (ram_bank_t *) priv;
    uint32_t    rel = addr - dev->virt_base;
    uint32_t    ret = 0xffffffff;

    if (rel & 0x03)
        return sis_85c471_read_ramw(addr, priv) | (sis_85c471_read_ramw(addr + 2, priv) << 16);

    uint32_t row = sis_85c471_set_row(dev, sis_85c471_get_row(dev, rel));
    uint32_t col = sis_85c471_set_col(dev, sis_85c471_get_col(dev, rel));
    uint32_t dw  = rel & 0x00000003;
    rel = row | col | dw;

    addr = (rel + dev->phys_base);

    if ((addr < (mem_size << 10)) && (rel < dev->phys_size))
        ret = *(uint32_t *) &(ram[addr]);

    return ret;
}

static void
sis_85c471_write_ram(uint32_t addr, uint8_t val, void *priv)
{
    ram_bank_t *dev = (ram_bank_t *) priv;
    uint32_t    rel = addr - dev->virt_base;

    uint32_t row = sis_85c471_set_row(dev, sis_85c471_get_row(dev, rel));
    uint32_t col = sis_85c471_set_col(dev, sis_85c471_get_col(dev, rel));
    uint32_t dw  = rel & 0x00000003;
    rel = row | col | dw;

    addr = (rel + dev->phys_base);

    if ((addr < (mem_size << 10)) && (rel < dev->phys_size))
        ram[addr] = val;
}

static void
sis_85c471_write_ramw(uint32_t addr, uint16_t val, void *priv)
{
    ram_bank_t *dev = (ram_bank_t *) priv;
    uint32_t    rel = addr - dev->virt_base;


    if ((rel & 0x03) == 0x03) {
        sis_85c471_write_ram(addr, val & 0xff, priv);
        sis_85c471_write_ram(addr + 1, val >> 8, priv);
        return;
    }
    uint32_t row = sis_85c471_set_row(dev, sis_85c471_get_row(dev, rel));
    uint32_t col = sis_85c471_set_col(dev, sis_85c471_get_col(dev, rel));
    uint32_t dw  = rel & 0x00000003;
    rel = row | col | dw;

    addr = (rel + dev->phys_base);

    if ((addr < (mem_size << 10)) && (rel < dev->phys_size))
        *(uint16_t *) &(ram[addr]) = val;
}

static void
sis_85c471_write_raml(uint32_t addr, uint32_t val, void *priv)
{
    ram_bank_t *dev = (ram_bank_t *) priv;
    uint32_t    rel = addr - dev->virt_base;


    if (rel & 0x03) {
        sis_85c471_write_ramw(addr, val & 0xffff, priv);
        sis_85c471_write_ramw(addr + 2, val >> 16, priv);
        return;
    }
    uint32_t row = sis_85c471_set_row(dev, sis_85c471_get_row(dev, rel));
    uint32_t col = sis_85c471_set_col(dev, sis_85c471_get_col(dev, rel));
    uint32_t dw  = rel & 0x00000003;
    rel = row | col | dw;

    addr = (rel + dev->phys_base);

    if ((addr < (mem_size << 10)) && (rel < dev->phys_size))
        *(uint32_t *) &(ram[addr]) = val;
}

static void
sis_85c4xx_recalcremap(sis_85c4xx_t *dev)
{
    if (dev->is_471) {
        if ((mem_size > 8192) || (dev->shadowed & 0x3c) || (dev->regs[0x0b] & 0x02))
            mem_remap_top(0);
        else
            mem_remap_top(-256);
    } else if (dev->reg_base == 0x50) {
        /* 85C460/461: A0000-BFFFF and D0000-EFFFF are relocated to the top of
           1, 2, 4, 6 or 8 MB of DRAM when D and E segment shadowing is off. */
        if (((mem_size == 1024) || (mem_size == 2048) || (mem_size == 4096) ||
             (mem_size == 6144) || (mem_size == 8192)) && !(dev->shadowed & 0x3c))
            mem_remap_top(-256);
        else
            mem_remap_top(0);
    }
}

static void
sis_85c4xx_recalcmapping(sis_85c4xx_t *dev)
{
    uint32_t base;
    uint32_t n         = 0;
    uint32_t shflags   = 0;
    uint32_t readext;
    uint32_t writeext;
    uint8_t  romcs     = 0xc0;
    uint8_t  cur_romcs;

    dev->shadowed = 0x00;

    if (dev->romcs_e_active)
        romcs |= 0x30;

    shadowbios       = 0;
    shadowbios_write = 0;

    if (dev->regs[0x03] & 0x40)
        romcs |= 0x01;
    if (dev->regs[0x03] & 0x80)
        romcs |= 0x30;
    if (dev->regs[0x08] & 0x04)
        romcs |= 0x02;

    for (uint8_t i = 0; i < 8; i++) {
        base      = 0xc0000 + (i << 15);
        cur_romcs = romcs & (1 << i);
        readext   = cur_romcs ? MEM_READ_EXTANY : MEM_READ_EXTERNAL;
        writeext  = cur_romcs ? MEM_WRITE_EXTANY : MEM_WRITE_EXTERNAL;

        if ((i > 5) || (dev->regs[0x02] & (1 << i))) {
            shadowbios |= (base >= 0xe0000) && (dev->regs[0x02] & 0x80);
            shadowbios_write |= (base >= 0xe0000) && !(dev->regs[0x02] & 0x40);
            shflags = (dev->regs[0x02] & 0x80) ? MEM_READ_INTERNAL : readext;
            shflags |= (dev->regs[0x02] & 0x40) ? writeext : MEM_WRITE_INTERNAL;
            if (dev->regs[0x02] & 0x80)
                dev->shadowed |= (1 << i);
            if (!(dev->regs[0x02] & 0x40))
                dev->shadowed |= (1 << i);
            if (dev->force_flush || (dev->mem_state[i] != shflags)) {
                n++;
                mem_set_mem_state_both(base, 0x8000, shflags);
                if ((base >= 0xf0000) && (dev->mem_state[i] & MEM_READ_INTERNAL) && !(shflags & MEM_READ_INTERNAL))
                    mem_invalidate_range(base, base + 0x7fff);
                dev->mem_state[i] = shflags;
            }
        } else {
            shflags = readext | writeext;
            if (dev->force_flush || (dev->mem_state[i] != shflags)) {
                n++;
                mem_set_mem_state_both(base, 0x8000, shflags);
                dev->mem_state[i] = shflags;
            }
        }
    }

    if (dev->force_flush) {
        flushmmucache();
        dev->force_flush = 0;
    } else if (n > 0)
        flushmmucache_nopc();

    sis_85c4xx_recalcremap(dev);
}

/* A CPU reset (the keyboard controller's reset pulse, Ctrl+Alt+Del) does
   not reset the chipset, but the BIOS runs its POST again and needs its
   extension ROM at E0000. A CPU with a 24-bit address bus starts at F000:FFF0
   with a base below 1 MB, so the reset vector does not depend on A20 (the
   BIOS may reset with A20 off, which would put FFFFF0h at EFFFF0h in RAM). */
static void
sis_85c4xx_cpu_reset(void *priv)
{
    sis_85c4xx_t *dev = (sis_85c4xx_t *) priv;

    if (!dev->romcs_e_active) {
        dev->romcs_e_active = 1;
        dev->force_flush    = 1;
        sis_85c4xx_recalcmapping(dev);
    }

    if (cpu_16bitbus) {
        if (is486)
            loadcs(0xf000);
        else
            loadcs_2386(0xf000);
        cpu_state.pc = 0xfff0;
    }
}

static const device_t sis_85c4xx_cpu_reset_device = {
    .name          = "SiS 85C471 CPU reset",
    .internal_name = "sis_85c471_cpu_reset",
    .flags         = DEVICE_SOFTRESET,
    .local         = 0,
    .init          = NULL,
    .close         = NULL,
    .reset         = sis_85c4xx_cpu_reset,
    .available     = NULL,
    .speed_changed = NULL,
    .force_redraw  = NULL,
    .config        = NULL
};

/* POST code 00h is the BIOS handing over to the boot loader: stop decoding
   the E000 half of a 128 KB board ROM so that segment is free for UMBs. */
static void
sis_85c4xx_post_out(UNUSED(uint16_t port), uint8_t val, void *priv)
{
    sis_85c4xx_t *dev = (sis_85c4xx_t *) priv;

    if (dev->romcs_e_active && (val == 0x00)) {
        dev->romcs_e_active = 0;
        dev->force_flush    = 1;
        sis_85c4xx_recalcmapping(dev);
    }
}

static void
sis_85c4xx_sw_smi_out(UNUSED(uint16_t port), UNUSED(uint8_t val), void *priv)
{
    sis_85c4xx_t *dev = (sis_85c4xx_t *) priv;

    if (dev->regs[0x18] & 0x02) {
        if (dev->regs[0x0b] & 0x10)
            smi_raise();
        else
            picint(1 << ((dev->regs[0x0b] & 0x08) ? 15 : 12));
        soft_reset_mask = 1;
        dev->regs[0x19] |= 0x02;
    }
}

static void
sis_85c4xx_sw_smi_handler(sis_85c4xx_t *dev)
{
    uint16_t addr;

    if (!dev->is_471)
        return;

    addr = dev->regs[0x14] | (dev->regs[0x15] << 8);

    io_handler((dev->regs[0x0b] & 0x80) && (dev->regs[0x18] & 0x02), addr, 0x0001,
               NULL, NULL, NULL, sis_85c4xx_sw_smi_out, NULL, NULL, dev);
}

static void
sis_85c471_banks_split(uint32_t *b_ex, uint32_t *banks)
{
    for (uint8_t i = 0; i < 4; i++) {
        if ((banks[i] == 0x00200000) || (banks[i] == 0x00800000) ||
            (banks[i] == 0x02000000))
            b_ex[i << 1] = b_ex[(i << 1) + 1] = banks[i] >> 1;
        else {
            b_ex[i << 1] = banks[i];
            b_ex[(i << 1) + 1] = 0x00000000;
        }
    }
}

static void
sis_85c471_banks_recalc(sis_85c4xx_t *dev)
{
    uint32_t ext_len;

    reg09 = dev->regs[0x09];

    for (uint8_t i = 0; i < 8; i++)
        mem_mapping_disable(&dev->ram_banks[i].mapping);

    mem_mapping_disable(&ram_low_mapping);
    mem_mapping_disable(&ram_high_mapping);
    /* A CPU with a 24-bit address bus (386SX-class) finds its reset vector
       in the BIOS alias just below 16 MB, so leave that alone. */
    ext_len = cpu_16bitbus ? (0x00fe0000 - (1 << 20)) : (127 << 20);
    mem_set_mem_state_both(1 << 20, ext_len, MEM_READ_EXTERNAL | MEM_WRITE_EXTERNAL);

    if ((dev->regs[0x09] & 0x3f) == dev->ram_banks_val) {
        if (mem_size > 1024) {
            mem_mapping_enable(&ram_low_mapping);
            mem_mapping_enable(&ram_high_mapping);
            mem_set_mem_state_both(1 << 20, MIN((mem_size << 10) - (1 << 20), ext_len),
                                   MEM_READ_INTERNAL | MEM_WRITE_INTERNAL);
        }
    } else {
        uint8_t   banks_val = dev->regs[0x09] & 0x3f;
        uint32_t *banks     = banks_471[banks_val];
        uint32_t  b_ex[8]   = { 0x00000000 };
        uint32_t  size      = 0x00000000;

        sis_85c471_banks_split(b_ex, banks);

        for (uint8_t i = 0; i < 8; i++)  if (b_ex[i] != 0x00000000) {
            dev->ram_banks[i].virt_base = size;
            dev->ram_banks[i].virt_size = b_ex[i];

            mem_mapping_set_addr(&dev->ram_banks[i].mapping, size, b_ex[i]);

            size += b_ex[i];
        }

        mem_set_mem_state_both(1 << 20, ext_len, MEM_READ_INTERNAL | MEM_WRITE_INTERNAL);
    }

    flushmmucache_nopc();
}

/* With register 57h bit 0 (Cache Sizing Enable) set, the 85C460/461 forces
   every cycle to be a cache hit, so memory reads and writes go to the cache
   data SRAM, which wraps around at its size. The BIOS sizes the cache by
   writing a pattern and looking for where it reappears. */
static uint8_t
sis_85c46x_cache_sram_read(uint32_t addr, void *priv)
{
    const sis_85c4xx_t *dev = (sis_85c4xx_t *) priv;

    /* A board without cache SRAM: the data lines float. */
    if (dev->cache_sram == NULL)
        return 0xff;

    return dev->cache_sram[addr & dev->cache_sram_mask];
}

static uint16_t
sis_85c46x_cache_sram_readw(uint32_t addr, void *priv)
{
    return sis_85c46x_cache_sram_read(addr, priv) |
           (sis_85c46x_cache_sram_read(addr + 1, priv) << 8);
}

static uint32_t
sis_85c46x_cache_sram_readl(uint32_t addr, void *priv)
{
    return sis_85c46x_cache_sram_readw(addr, priv) |
           (sis_85c46x_cache_sram_readw(addr + 2, priv) << 16);
}

static void
sis_85c46x_cache_sram_write(uint32_t addr, uint8_t val, void *priv)
{
    const sis_85c4xx_t *dev = (sis_85c4xx_t *) priv;

    if (dev->cache_sram != NULL)
        dev->cache_sram[addr & dev->cache_sram_mask] = val;
}

static void
sis_85c46x_cache_sram_writew(uint32_t addr, uint16_t val, void *priv)
{
    sis_85c46x_cache_sram_write(addr, val & 0xff, priv);
    sis_85c46x_cache_sram_write(addr + 1, val >> 8, priv);
}

static void
sis_85c46x_cache_sram_writel(uint32_t addr, uint32_t val, void *priv)
{
    sis_85c46x_cache_sram_writew(addr, val & 0xffff, priv);
    sis_85c46x_cache_sram_writew(addr + 2, val >> 16, priv);
}

static void
sis_85c46x_cache_sizing(sis_85c4xx_t *dev, int enable)
{
    if (!dev->force_cache_size)
        return;

    if (enable)
        mem_mapping_enable(&dev->cache_sram_mapping);
    else
        mem_mapping_disable(&dev->cache_sram_mapping);

    flushmmucache();
}

/* The 85C401 BIOS (AMI 05/05/91) sizes the cache with the cache on
   (register 61h bit 7) and register 67h bit 0 clear, writing a pattern at
   256, 320 and 384 KB and reading which one survives where they share a
   cache line; it then sets 67h bit 0. Model that window as the cache data
   SRAM, which wraps around at its size. */
static void
sis_85c401_cache_sizing_update(sis_85c4xx_t *dev)
{
    int on = (dev->regs[0x01] & 0x80) && !(dev->regs[0x07] & 0x01);

    if (dev->force_cache_size && (on != dev->cache_sizing)) {
        dev->cache_sizing = on;
        sis_85c46x_cache_sizing(dev, on);
    }
}

/* SiS 85C460 datasheet, Turbo Switch: "When the de-turbo function is turned
   on, system performs 1/3 or 2/3 speed". Register 53h bit 0 set means
   ignore the front panel turbo switch. Register 58h bit 4 picks 1/3 or 2/3
   on the real chip; here the LFTech BIOS's De-Turbo Speed setting picks
   the speed instead (lftech.c). Register 57h bit 1, the software de-turbo
   that the AMI BIOS sets on Ctrl+Alt+minus, is left alone: Turbo is only
   switched from the front panel. The hold is the processor's clock control
   throttle (STPCLK# modulated, cpu.h), which the CPU loop counts as time
   passing, so the processor runs slower against the timers and the clock.
   The timer polls so that it also picks up the switch, which the user
   interface changes from another thread. */
static void
sis_85c46x_deturbo_timer(void *priv)
{
    sis_85c4xx_t *dev    = (sis_85c4xx_t *) priv;
    int           active = !(dev->regs[0x03] & 0x01) && !machine_turbo_switch;
    int           period = 3;
    int           run    = 0;

    /* The LFTech BIOS's De-Turbo Speed setting picks the hold instead. */
    machine_speed_pct = 100;
    if (active)
        machine_speed_pct = lftech_deturbo_throttle(&period, &run);
    if (run >= period)
        run = 0;

    if ((cpu_throttle_run != run) || (run && (cpu_throttle_period != period))) {
        cpu_throttle_period = period;
        cpu_throttle_break  = 0;
        cpu_throttle_run    = run;
    }

    timer_advance_u64(&dev->deturbo_timer, (uint64_t) (1000.0 * TIMER_USEC));
}

static void
sis_85c4xx_out(uint16_t port, uint8_t val, void *priv)
{
    sis_85c4xx_t *dev       = (sis_85c4xx_t *) priv;
    uint8_t       rel_reg   = dev->cur_reg - dev->reg_base;
    uint8_t       valxor    = 0x00;
    uint32_t      host_base = 0x000e0000;
    uint32_t      ram_base  = 0x000a0000;

    switch (port) {
        case 0x22:
            dev->cur_reg = val;
            break;
        case 0x23:
            if ((dev->cur_reg >= dev->reg_base) && (dev->cur_reg <= dev->reg_last)) {
                if (dev->is_471 && dev->force_cache_size && (rel_reg == 0x01))
                    val = (val & 0x8f) | ((dev->cache_size_code & 0x07) << 4);

                valxor = val ^ dev->regs[rel_reg];

                if (!dev->is_471 && (rel_reg == 0x00))
                    dev->regs[rel_reg] = (dev->regs[rel_reg] & 0x1f) | (val & 0xe0);
                else
                    dev->regs[rel_reg] = val;

                switch (rel_reg) {
                    case 0x00:
                        break;

                    case 0x01:
                        cpu_cache_ext_enabled = ((val & 0x84) == 0x84);
                        cpu_update_waitstates();
                        if (dev->reg_base == 0x60)
                            sis_85c401_cache_sizing_update(dev);
                        break;

                    case 0x02:
                    case 0x03:
                    case 0x08:
                        if (valxor)
                            sis_85c4xx_recalcmapping(dev);
                        if ((rel_reg == 0x08) && dev->is_471)
                            flushmmucache();
                        break;

                    case 0x07:
                        if (dev->reg_base == 0x60)
                            sis_85c401_cache_sizing_update(dev);
                        else if (!dev->is_471 && (valxor & 0x01))
                            sis_85c46x_cache_sizing(dev, val & 0x01);
                        break;

                    case 0x09:
                        if (dev->is_471)
                            sis_85c471_banks_recalc(dev);
                        break;

                    case 0x0b:
                        sis_85c4xx_sw_smi_handler(dev);
                        if (valxor & 0x02)
                            sis_85c4xx_recalcremap(dev);
                        break;

                    case 0x10:
                        if (dev->reg_base == 0x50) {
                            double bus_clk;

                            switch (val & 0xe0) {
                                default:
                                case 0x00:
                                     bus_clk = 7159091.0;
                                     break;
                                case 0x02:
                                     bus_clk = cpu_busspeed / 10.0;
                                     break;
                                case 0x04:
                                     bus_clk = cpu_busspeed / 8.0;
                                     break;
                                case 0x06:
                                     bus_clk = cpu_busspeed / 6.0;
                                     break;
                                case 0x80:
                                     bus_clk = cpu_busspeed / 5.0;
                                     break;
                                case 0xa0:
                                     bus_clk = cpu_busspeed / 4.0;
                                     break;
                                case 0xc0:
                                     bus_clk = cpu_busspeed / 3.0;
                                     break;
                                case 0xe0:
                                     bus_clk = cpu_busspeed / 2.0;
                                     break;
                            }
                            cpu_set_isa_speed((int) round(bus_clk));
                        }
                        break;

                    case 0x13:
                        if (dev->is_471 && (valxor & 0xf0)) {
                            smram_disable(dev->smram);
                            host_base = (val & 0x80) ? 0x00060000 : 0x000e0000;
                            switch ((val >> 5) & 0x03) {
                                case 0x00:
                                    ram_base = 0x000a0000;
                                    break;
                                case 0x01:
                                    ram_base = 0x000b0000;
                                    break;
                                case 0x02:
                                    ram_base = (val & 0x80) ? 0x00000000 : 0x000e0000;
                                    break;
                                default:
                                    ram_base = 0x00000000;
                                    break;
                            }
                            dev->smram_enabled = (ram_base != 0x00000000);
                            if (ram_base != 0x00000000)
                                smram_enable(dev->smram, host_base, ram_base, 0x00010000, (val & 0x10), 1);
                            sis_85c4xx_recalcremap(dev);
                        }
                        break;

                    case 0x14:
                    case 0x15:
                    case 0x18:
                        sis_85c4xx_sw_smi_handler(dev);
                        break;

                    case 0x1c:
                        if (dev->is_471)
                            soft_reset_mask = 0;
                        break;

                    case 0x22:
                        if (dev->is_471 && (valxor & 0x01)) {
                            port_92_remove(dev->port_92);
                            if (val & 0x01)
                                port_92_add(dev->port_92);
                        }
                        break;
                    default:
                        break;
                }
            } else if ((dev->reg_base == 0x60) && (dev->cur_reg == 0x00))
                dev->reg_00 = val;
            dev->cur_reg = 0x00;
            break;

        case 0xe1:
        case 0xe2:
            dev->scratch[port - 0xe1] = val;
            return;
        default:
            break;
    }
}

static uint8_t
sis_85c4xx_in(uint16_t port, void *priv)
{
    sis_85c4xx_t *dev     = (sis_85c4xx_t *) priv;
    uint8_t       rel_reg = dev->cur_reg - dev->reg_base;
    uint8_t       ret     = 0xff;

    switch (port) {
        case 0x22:
            /* The 85C471 reads back its index register. The AMI 471 BIOS
               tests bit 0 of it to decide whether to run its Cyrix cache
               setup, which sets CR0.NW with CD clear on any other CPU; a
               real 486 takes a #GP there and POST hangs at code A1. */
            if (dev->is_471)
                ret = dev->cur_reg;
            break;

        case 0x23:
            if (dev->is_471 && (dev->cur_reg == 0x1c))
                ret = inb(0x70);
            /* On the SiS 40x, the shadow RAM read and write enable bits are write-only! */
            if ((dev->reg_base == 0x60) && (dev->cur_reg == 0x62))
                ret = dev->regs[rel_reg] & 0x3f;
            else if ((dev->cur_reg >= dev->reg_base) && (dev->cur_reg <= dev->reg_last))
                ret = dev->regs[rel_reg];
            else if ((dev->reg_base == 0x60) && (dev->cur_reg == 0x00))
                ret = dev->reg_00;
            if (dev->reg_base != 0x60)
                dev->cur_reg = 0x00;
            break;

        case 0xe1:
        case 0xe2:
            ret = dev->scratch[port - 0xe1];
            break;

        default:
            break;
    }

    return ret;
}

static void
sis_85c4xx_reset(void *priv)
{
    sis_85c4xx_t  *dev         = (sis_85c4xx_t *) priv;
    int            mem_size_mb = mem_size >> 10;

    memset(dev->regs, 0x00, sizeof(dev->regs));

    dev->romcs_e_active = dev->post_romcs_e;

    sis_85c46x_cache_sizing(dev, 0);
    dev->cache_sizing = 0;

    if ((cpu_s != NULL) && (cpu_s->rspeed < 25000000))
        dev->regs[0x08] = 0x80;

    if (dev->is_471) {
        dev->regs[0x09] = 0x40;

        if (machines[machine].init == machine_at_vli486sv2g_init) {
            if (mem_size_mb == 64)
                dev->regs[0x09] |= 0x1f;
            else
                dev->regs[0x09] |= ram_asus[mem_size_mb];
        } else if (mem_size_mb >= 64) {
            if ((mem_size_mb >= 64) && (mem_size_mb < 68))
                dev->regs[0x09] |= 0x33;
            else if ((mem_size_mb >= 68) && (mem_size_mb < 72))
                dev->regs[0x09] |= 0x2b;
            else if ((mem_size_mb >= 72) && (mem_size_mb < 80))
                dev->regs[0x09] |= 0x2d;
            else if ((mem_size_mb >= 80) && (mem_size_mb < 96))
                dev->regs[0x09] |= 0x2f;
            else if ((mem_size_mb >= 96) && (mem_size_mb < 128))
                dev->regs[0x09] |= 0x34;
            else
                dev->regs[0x09] |= 0x35;
        } else if (machines[machine].init == machine_at_tg486g_init)
            dev->regs[0x09] |= ram_tg486g[mem_size_mb];
        else
            dev->regs[0x09] |= ram_471[mem_size_mb];
        dev->ram_banks_val = dev->regs[0x09] & 0x3f;
        dev->regs[0x09] = 0x00;

        uint32_t *banks   = banks_471[dev->ram_banks_val];
        uint32_t  b_ex[8] = { 0x00000000 };
        uint32_t  size    = 0x00000000;

        sis_85c471_banks_split(b_ex, banks);

        for (uint8_t i = 0; i < 8; i++) {
            dev->ram_banks[i].phys_base = size;
            dev->ram_banks[i].phys_size = b_ex[i];

            size += b_ex[i];
        }

        dev->regs[0x11] = 0x09;
        dev->regs[0x12] = 0xff;
        dev->regs[0x1f] = 0x20; /* Video access enabled. */
        dev->regs[0x23] = 0xf0;
        dev->regs[0x26] = 0x01;

        smram_enable(dev->smram, 0x000e0000, 0x000a0000, 0x00010000, 0, 1);

        port_92_remove(dev->port_92);

        soft_reset_mask = 0;

        sis_85c471_banks_recalc(dev);

        kbc_at_set_fast_reset(1);
        cpu_cpurst_on_sr = 0;
    } else {
        /* Bits 6 and 7 must be clear on the SiS 40x. */
        if (dev->reg_base == 0x60)
            dev->reg_00 = 0x24;

        if (mem_size_mb == 64)
            dev->regs[0x00] = 0x1f;
        else if (mem_size_mb < 64)
            dev->regs[0x00] = ram_4xx[mem_size_mb];

        dev->regs[0x11] = 0x01;
    }

    dev->scratch[0] = dev->scratch[1] = 0xff;

    cpu_cache_ext_enabled = 0;
    cpu_update_waitstates();

    dev->force_flush = 1;
    sis_85c4xx_recalcmapping(dev);

    if (dev->reg_base == 0x50)
        cpu_set_isa_speed((int) round(7159091.0));
}

static void
sis_85c4xx_close(void *priv)
{
    sis_85c4xx_t *dev = (sis_85c4xx_t *) priv;

    if (dev->is_471)
        smram_del(dev->smram);

    if (dev->has_deturbo) {
        timer_disable(&dev->deturbo_timer);
        cpu_throttle_run = 0;
        machine_set_turbo_switch_present(0);
    }

    free(dev->cache_sram);
    free(dev);
}

static void *
sis_85c4xx_init(const device_t *info)
{
    sis_85c4xx_t *dev = (sis_85c4xx_t *) calloc(1, sizeof(sis_85c4xx_t));

    dev->is_471           = (info->local >> 8) & 0xff;
    dev->cache_size_code  = (info->local >> 16) & 0x07;
    dev->force_cache_size = (info->local >> 24) & 0x01;
    dev->post_romcs_e     = (info->local >> 25) & 0x01;

    dev->reg_base = info->local & 0xff;

    if (dev->is_471) {
        dev->reg_last = 0x76;

        dev->smram = smram_add();

        dev->port_92 = device_add(&port_92_device);

        for (uint8_t i = 0; i < 8; i++) {
            mem_mapping_add(&dev->ram_banks[i].mapping, 0x00000000, 0x00000000,
                            sis_85c471_read_ram, sis_85c471_read_ramw, sis_85c471_read_raml,
                            sis_85c471_write_ram, sis_85c471_write_ramw, sis_85c471_write_raml,
                            NULL, MEM_MAPPING_INTERNAL, &(dev->ram_banks[i]));
            mem_mapping_disable(&dev->ram_banks[i].mapping);
        }
    } else {
        dev->reg_last = dev->reg_base + 0x11;

        /* On the 85C460/461, the force cache size flag means that the board
           has cache SRAM; the code is the register 51h bits 5:4 size, from
           32 KB (0) to 256 KB (3). */
        if (dev->force_cache_size) {
            /* On the 85C401, code 0 is a board without cache SRAM. */
            if ((dev->reg_base != 0x60) || dev->cache_size_code) {
                uint32_t sram_size = 0x00008000 << (dev->cache_size_code & 0x03);

                dev->cache_sram      = (uint8_t *) calloc(1, sram_size);
                dev->cache_sram_mask = sram_size - 1;
            }

            mem_mapping_add(&dev->cache_sram_mapping, 0x00000000, 0x000a0000,
                            sis_85c46x_cache_sram_read, sis_85c46x_cache_sram_readw,
                            sis_85c46x_cache_sram_readl,
                            sis_85c46x_cache_sram_write, sis_85c46x_cache_sram_writew,
                            sis_85c46x_cache_sram_writel,
                            NULL, MEM_MAPPING_INTERNAL, dev);
            mem_mapping_disable(&dev->cache_sram_mapping);
        }
    }

    /* De-turbo and the Turbo switch only on the LFTech boards. */
    dev->has_deturbo = !dev->is_471 && (dev->reg_base == 0x50) && machine_lftech;
    if (dev->has_deturbo) {
        timer_add(&dev->deturbo_timer, sis_85c46x_deturbo_timer, dev, 0);
        timer_set_delay_u64(&dev->deturbo_timer, (uint64_t) (1000.0 * TIMER_USEC));
        machine_set_turbo_switch_present(1);
    }

    io_sethandler(0x0022, 0x0002,
                  sis_85c4xx_in, NULL, NULL, sis_85c4xx_out, NULL, NULL, dev);

    io_sethandler(0x00e1, 0x0002,
                  sis_85c4xx_in, NULL, NULL, sis_85c4xx_out, NULL, NULL, dev);

    if (dev->post_romcs_e) {
        io_sethandler(0x0080, 0x0001,
                      NULL, NULL, NULL, sis_85c4xx_post_out, NULL, NULL, dev);
        device_add_ex(&sis_85c4xx_cpu_reset_device, dev);
    }

    sis_85c4xx_reset(dev);

    return dev;
}

const device_t sis_85c401_device = {
    .name          = "SiS 85c401/85c402",
    .internal_name = "sis_85c401",
    .flags         = 0,
    .local         = 0x060,
    .init          = sis_85c4xx_init,
    .close         = sis_85c4xx_close,
    .reset         = sis_85c4xx_reset,
    .available     = NULL,
    .speed_changed = NULL,
    .force_redraw  = NULL,
    .config        = NULL
};

const device_t sis_85c460_device = {
    .name          = "SiS 85c460",
    .internal_name = "sis_85c460",
    .flags         = 0,
    .local         = 0x050,
    .init          = sis_85c4xx_init,
    .close         = sis_85c4xx_close,
    .reset         = sis_85c4xx_reset,
    .available     = NULL,
    .speed_changed = NULL,
    .force_redraw  = NULL,
    .config        = NULL
};

/* TODO: Log to make sure the registers are correct. */
const device_t sis_85c461_device = {
    .name          = "SiS 85c461",
    .internal_name = "sis_85c461",
    .flags         = 0,
    .local         = 0x050,
    .init          = sis_85c4xx_init,
    .close         = sis_85c4xx_close,
    .reset         = sis_85c4xx_reset,
    .available     = NULL,
    .speed_changed = NULL,
    .force_redraw  = NULL,
    .config        = NULL
};

const device_t sis_85c471_device = {
    .name          = "SiS 85c407/85c471",
    .internal_name = "sis_85c471",
    .flags         = 0,
    .local         = 0x150,
    .init          = sis_85c4xx_init,
    .close         = sis_85c4xx_close,
    .reset         = sis_85c4xx_reset,
    .available     = NULL,
    .speed_changed = NULL,
    .force_redraw  = NULL,
    .config        = NULL
};

/*
 * Software License Agreement (BSD License)
 *
 * Copyright (c) 2017, DUKELEC, Inc.
 * All rights reserved.
 *
 * Author: Duke Fong <duke@dukelec.com>
 */

#include "common.h"
#include "modbus_crc.h"
#include "cdctl_bx.h"


#define REG_VERSION         0x00
#define REG_SETTING         0x01
#define REG_SILENCE_LEN     0x02
#define REG_TX_DELAY        0x03
#define REG_FILTER          0x04
#define REG_PERIOD_LS_L     0x05
#define REG_PERIOD_LS_H     0x06
#define REG_PERIOD_HS_L     0x07
#define REG_PERIOD_HS_H     0x08
#define REG_INT_FLAG        0x09
#define REG_INT_MASK        0x0a
#define REG_RX              0x0b
#define REG_TX              0x0c
#define REG_RX_CTRL         0x0d
#define REG_TX_CTRL         0x0e
#define REG_RX_ADDR         0x0f
#define REG_RX_PAGE_FLAG    0x10


#define BIT_SETTING_TX_PUSH_PULL    (1 << 0)
#define BIT_SETTING_TX_INVERT       (1 << 1)
#define BIT_SETTING_USER_CRC        (1 << 2)
#define BIT_SETTING_NO_DROP         (1 << 3)
#define POS_SETTING_TX_EN_ADVANCE         4
#define BIT_SETTING_NO_ARBITRATE    (1 << 6)

#define BIT_FLAG_BUS_IDLE           (1 << 0)
#define BIT_FLAG_RX_PENDING         (1 << 1)
#define BIT_FLAG_RX_LOST            (1 << 2)
#define BIT_FLAG_RX_ERROR           (1 << 3)
#define BIT_FLAG_TX_BUF_CLEAN       (1 << 4)
#define BIT_FLAG_TX_CD              (1 << 5)
#define BIT_FLAG_TX_ERROR           (1 << 6)

#define BIT_RX_RST_POINTER          (1 << 0)
#define BIT_RX_CLR_PENDING          (1 << 1)
#define BIT_RX_CLR_LOST             (1 << 2)
#define BIT_RX_CLR_ERROR            (1 << 3)
#define BIT_RX_RST                  (1 << 4)

#define BIT_TX_RST_POINTER          (1 << 0)
#define BIT_TX_START                (1 << 1)
#define BIT_TX_CLR_CD               (1 << 2)
#define BIT_TX_CLR_ERROR            (1 << 3)


static uint8_t cdctl_read_reg(cdctl_intf_t *intf, uint8_t reg)
{
    uint8_t dat = 0xff;
#ifdef CDCTL_I2C
    i2c_mem_read(intf->i2c, reg, &dat, 1);
#else
    spi_mem_read(intf->spi, reg, &dat, 1);
#endif
    return dat;
}

static void cdctl_write_reg(cdctl_intf_t *intf, uint8_t reg, uint8_t val)
{
#ifdef CDCTL_I2C
    i2c_mem_write(intf->i2c, reg, &val, 1);
#else
    spi_mem_write(intf->spi, reg | 0x80, &val, 1);
#endif
}

static void cdctl_read_frame(cdctl_intf_t *intf, cd_frame_t *frame)
{
#ifdef CDCTL_I2C
    i2c_mem_read(intf->i2c, REG_RX, frame->dat, 3);
    i2c_mem_read(intf->i2c, REG_RX, frame->dat + 3, frame->dat[2]);
#else
    spi_mem_read(intf->spi, REG_RX, frame->dat, 3);
    spi_mem_read(intf->spi, REG_RX, frame->dat + 3, frame->dat[2]);
#endif
}

static void cdctl_write_frame(cdctl_intf_t *intf, const cd_frame_t *frame)
{
#ifdef CDCTL_I2C
    i2c_mem_write(intf->i2c, REG_TX, frame->dat, frame->dat[2] + 3);
#else
    spi_mem_write(intf->spi, REG_TX | 0x80, frame->dat, frame->dat[2] + 3);
#endif
}

// member functions

static list_node_t *cdctl_get_free_node(cd_intf_t *cd_intf)
{
    cdctl_intf_t *cdctl_intf = container_of(cd_intf, cdctl_intf_t, cd_intf);
    return list_get(cdctl_intf->free_head);
}

static list_node_t *cdctl_get_rx_node(cd_intf_t *cd_intf)
{
    cdctl_intf_t *cdctl_intf = container_of(cd_intf, cdctl_intf_t, cd_intf);
    return list_get(&cdctl_intf->rx_head);
}

static void cdctl_put_free_node(cd_intf_t *cd_intf, list_node_t *node)
{
    cdctl_intf_t *cdctl_intf = container_of(cd_intf, cdctl_intf_t, cd_intf);
    list_put(cdctl_intf->free_head, node);
}

static void cdctl_put_tx_node(cd_intf_t *cd_intf, list_node_t *node)
{
    cdctl_intf_t *cdctl_intf = container_of(cd_intf, cdctl_intf_t, cd_intf);
    list_put(&cdctl_intf->tx_head, node);
}

static void cdctl_set_mac_filter(cd_intf_t *cd_intf, uint8_t filter)
{
    cdctl_intf_t *cdctl_intf = container_of(cd_intf, cdctl_intf_t, cd_intf);
    cdctl_write_reg(cdctl_intf, REG_FILTER, filter);
    cdctl_write_reg(cdctl_intf, REG_TX_DELAY,
                    filter == 255 ? 255 : filter + 1);
}

static uint8_t cdctl_get_mac_filter(cd_intf_t *cd_intf)
{
    cdctl_intf_t *cdctl_intf = container_of(cd_intf, cdctl_intf_t, cd_intf);
    return cdctl_read_reg(cdctl_intf, REG_FILTER);
}

static void cdctl_set_bond_rate(cd_intf_t *cd_intf,
                                uint16_t low, uint16_t high)
{
    cdctl_intf_t *cdctl_intf = container_of(cd_intf, cdctl_intf_t, cd_intf);
    cdctl_write_reg(cdctl_intf, REG_PERIOD_LS_L, low & 0xff);
    cdctl_write_reg(cdctl_intf, REG_PERIOD_LS_H, low >> 8);
    cdctl_write_reg(cdctl_intf, REG_PERIOD_HS_L, high & 0xff);
    cdctl_write_reg(cdctl_intf, REG_PERIOD_HS_H, high >> 8);
}

static void cdctl_get_bond_rate(cd_intf_t *cd_intf,
                                uint16_t *low, uint16_t *high)
{
    cdctl_intf_t *cdctl_intf = container_of(cd_intf, cdctl_intf_t, cd_intf);
    *low = cdctl_read_reg(cdctl_intf, REG_PERIOD_LS_L) |
            cdctl_read_reg(cdctl_intf, REG_PERIOD_LS_H) << 8;
    *high = cdctl_read_reg(cdctl_intf, REG_PERIOD_HS_L) |
            cdctl_read_reg(cdctl_intf, REG_PERIOD_HS_H) << 8;
}

static void cdctl_flush(cd_intf_t *cd_intf)
{
    cdctl_intf_t *cdctl_intf = container_of(cd_intf, cdctl_intf_t, cd_intf);
    cdctl_write_reg(cdctl_intf, REG_RX_CTRL, BIT_RX_RST);
}

void cdctl_intf_init(cdctl_intf_t *intf, list_head_t *free_head,
#ifdef CDCTL_I2C
        i2c_t *i2c)
#else
        spi_t *spi)
#endif
{
    intf->free_head = free_head;
    intf->cd_intf.get_free_node = cdctl_get_free_node;
    intf->cd_intf.get_rx_node = cdctl_get_rx_node;
    intf->cd_intf.put_free_node = cdctl_put_free_node;
    intf->cd_intf.put_tx_node = cdctl_put_tx_node;
    intf->cd_intf.set_mac_filter = cdctl_set_mac_filter;
    intf->cd_intf.get_mac_filter = cdctl_get_mac_filter;
    intf->cd_intf.set_bond_rate = cdctl_set_bond_rate;
    intf->cd_intf.get_bond_rate = cdctl_get_bond_rate;
    intf->cd_intf.flush = cdctl_flush;

#ifdef USE_DYNAMIC_INIT
    list_head_init(&intf->rx_head);
    list_head_init(&intf->tx_head);
    intf->is_pending = false;
    intf->t_wait = 0;
#endif

#ifdef CDCTL_I2C
    intf->i2c = i2c;
#else
    intf->spi = spi;
#endif

    while (true) {
        uint8_t ver = cdctl_read_reg(intf, REG_VERSION);
        if (ver != 0xff && ver != 0x00 &&
                ver == cdctl_read_reg(intf, REG_VERSION)) {
            d_info("cdctl %p: version: %02x\n", intf, ver);
            break;
        }
        d_info("cdctl %p: not ready, ver: %02x\n", intf, ver);
        debug_flush();
    }

    cdctl_write_reg(intf, REG_SETTING, BIT_SETTING_TX_PUSH_PULL);
    cdctl_set_mac_filter(&intf->cd_intf, 255);
    cdctl_flush(&intf->cd_intf);

    d_debug("cdctl %p: flags: %02x\n", intf,
        cdctl_read_reg(intf, REG_INT_FLAG));
}

// handlers


void cdctl_task(cdctl_intf_t *intf)
{
    uint8_t flags = cdctl_read_reg(intf, REG_INT_FLAG);

    if (flags & BIT_FLAG_RX_PENDING) {
        // if get free list: copy to rx list
        list_node_t *node = list_get(intf->free_head);
        if (node) {
            d_verbose("cdctl %p: get_rx\n", intf);
            cd_frame_t *frame = container_of(node, cd_frame_t, node);
            cdctl_read_frame(intf, frame);
            cdctl_write_reg(intf, REG_RX_CTRL, BIT_RX_CLR_PENDING);
            list_put(&intf->rx_head, node);
        } else {
            d_debug("cdctl %p: get_rx, no free node\n", intf);
        }
    }

    if (!intf->is_pending && intf->tx_head.first) {
        list_node_t *node = list_get(&intf->tx_head);
        cd_frame_t *frame = container_of(node, cd_frame_t, node);
        d_verbose("cdctl %p: write frame\n", intf);
        cdctl_write_frame(intf, frame);
        if (frame->dat[0] == 255) {
            // wait random time if from_id = 255
            intf->t_wait = 1 + rand() / (RAND_MAX / CDCTL_TX_WAIT_RANGE);
            intf->t_last = get_systick();
            intf->is_pending = true;
        } else {
            intf->t_wait = 0;
            flags = cdctl_read_reg(intf, REG_INT_FLAG);
            if (flags & BIT_FLAG_TX_BUF_CLEAN) {
                cdctl_write_reg(intf, REG_TX_CTRL, BIT_TX_START);
            } else {
                intf->t_wait = 0;
                intf->is_pending = true;
            }
        }
        list_put(intf->free_head, node);
    }

    if (intf->is_pending) {
        flags = cdctl_read_reg(intf, REG_INT_FLAG);
        if (intf->t_wait) {
            if (flags & BIT_FLAG_BUS_IDLE) {
                if (get_systick() - intf->t_last <= intf->t_wait)
                    return;
                intf->t_wait = 0;
                d_verbose("cdctl %p: wait end\n", intf);
            } else {
                intf->t_wait = 1 + rand() / (RAND_MAX / CDCTL_TX_WAIT_RANGE);
                intf->t_last = get_systick();
                d_verbose("cdctl %p: re-wait\n", intf);
                return;
            }
        }
        if (flags & BIT_FLAG_TX_BUF_CLEAN) {
            d_verbose("cdctl %p: trigger tx\n", intf);
            cdctl_write_reg(intf, REG_TX_CTRL, BIT_TX_START);
            intf->is_pending = false;
        }
    }
}


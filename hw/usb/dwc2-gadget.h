/*
 * DWC2 device mode (buffer DMA) with a built-in USB host
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_USB_DWC2_GADGET_H
#define HW_USB_DWC2_GADGET_H

#include "chardev/char-fe.h"
#include "hw/usb/usbmidi-codec.h"

#define DWC2_DREG_SIZE      0x600       /* 0x800 - 0xdff */
#define DWC2_GADGET_EPS     16
#define DWC2_GADGET_CTRL    4096        /* control transfer data */
#define DWC2_GADGET_EVQ     (64 * 1024) /* event packets for the MIDI OUT EP */

typedef struct DWC2State DWC2State;

typedef struct DWC2Gadget {
    CharFrontend chr;
    QEMUTimer *timer;
    uint32_t dreg[DWC2_DREG_SIZE / 4];

    /* the built-in host */
    int link;               /* DWC2_LINK_* */
    int64_t wait_until;
    int step;               /* enumeration step */
    int phase;              /* control transfer phase */
    uint8_t setup[8];
    uint8_t ctrl[DWC2_GADGET_CTRL];
    unsigned ctrl_len, ctrl_want;
    bool ctrl_failed;
    uint16_t cfg_total;
    uint8_t cfg_value;

    /* the MIDI streaming interface found in the configuration */
    int midi_iface, midi_alt;
    int midi_out, midi_in;          /* EP numbers, -1: none */

    /* raw bytes from the chardev -> event packets for the OUT EP */
    USBMIDIEncoder enc;
    uint8_t *evq;
    unsigned evq_head, evq_count;

    /* pacing of the OUT EP: event bytes a second (0: as fast as armed) */
    uint32_t out_rate;
    int64_t out_credit;

    /* raw bytes for the chardev */
    GByteArray *tx;

    uint64_t setups, stalls, midi_rx, midi_tx;
} DWC2Gadget;

bool dwc2_gadget_enabled(DWC2State *s);
void dwc2_gadget_realize(DWC2State *s, Error **errp);
void dwc2_gadget_reset(DWC2State *s);
uint32_t dwc2_gadget_read(DWC2State *s, hwaddr off);
void dwc2_gadget_write(DWC2State *s, hwaddr off, uint32_t val);

/* in hcd-dwc2.c */
void dwc2_irq_update(DWC2State *s);

#endif

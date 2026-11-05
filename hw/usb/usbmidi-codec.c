/*
 * USB MIDI 1.0 event packets <-> raw MIDI bytes (cable 0)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/usb/usbmidi-codec.h"

static void put3(USBMIDIPut *put, void *opaque, uint8_t cin,
                 uint8_t b0, uint8_t b1, uint8_t b2)
{
    uint8_t ev[4] = { cin & 0x0f, b0, b1, b2 };
    put(opaque, ev);
}

void usbmidi_encoder_reset(USBMIDIEncoder *e)
{
    memset(e, 0, sizeof(*e));
}

void usbmidi_encode(USBMIDIEncoder *e, uint8_t b, USBMIDIPut *put,
                    void *opaque)
{
    if (b >= 0xf8) {                         /* real time, anywhere */
        put3(put, opaque, 0xf, b, 0, 0);
        return;
    }
    if (b == 0xf0) {
        e->sysex = true;
        e->status = 0;
        e->msg[0] = b;
        e->n = 1;
        return;
    }
    if (b == 0xf7) {
        if (e->sysex) {                      /* CIN 5, 6 or 7 by its length */
            uint8_t m[3] = { 0, 0, 0 };
            memcpy(m, e->msg, e->n);
            m[e->n] = 0xf7;
            put3(put, opaque, 0x4 + e->n + 1, m[0], m[1], m[2]);
        }
        e->sysex = false;
        e->n = 0;
        return;
    }
    if (e->sysex) {
        if (b & 0x80) {                  /* a status ends it, unterminated */
            e->sysex = false;
            e->n = 0;
        } else {
            e->msg[e->n++] = b;
            if (e->n == 3) {
                put3(put, opaque, 0x4, e->msg[0], e->msg[1], e->msg[2]);
                e->n = 0;
            }
            return;
        }
    }
    if (b & 0x80) {
        e->status = b;
        e->n = 0;
        switch (b & 0xf0) {
        case 0xc0: case 0xd0:
            e->need = 1;
            break;
        case 0xf0:
            e->need = (b == 0xf2) ? 2 : (b == 0xf1 || b == 0xf3) ? 1 : 0;
            if (!e->need) {                  /* F6 tune request; F4/F5 */
                if (b == 0xf6) {
                    put3(put, opaque, 0x5, b, 0, 0);
                }
                e->status = 0;
            }
            break;
        default:
            e->need = 2;
        }
        return;
    }
    if (!e->status) {
        return;                              /* data without a status */
    }
    e->msg[1 + e->n++] = b;
    if (e->n == e->need) {
        uint8_t cin = e->status < 0xf0 ? e->status >> 4
                    : e->need == 1 ? 0x2 : 0x3;
        put3(put, opaque, cin, e->status, e->msg[1],
             e->need > 1 ? e->msg[2] : 0);
        e->n = 0;
        if (e->status >= 0xf0) {
            e->status = 0;                   /* no running status */
        }
    }
}

static const uint8_t cin_len[16] = {
    0, 0, 2, 3, 3, 1, 2, 3, 3, 3, 3, 3, 2, 2, 3, 1
};

unsigned usbmidi_decode(const uint8_t *ev, unsigned n, uint8_t *raw)
{
    unsigned i, o = 0;

    for (i = 0; i + 4 <= n; i += 4) {
        unsigned len = cin_len[ev[i] & 0x0f];
        memcpy(raw + o, ev + i + 1, len);
        o += len;
    }
    return o;
}

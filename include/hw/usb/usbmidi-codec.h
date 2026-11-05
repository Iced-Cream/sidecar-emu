/*
 * USB MIDI 1.0 event packets <-> raw MIDI bytes (cable 0)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_USB_USBMIDI_CODEC_H
#define HW_USB_USBMIDI_CODEC_H

/* raw bytes -> event packets: the running state of one stream */
typedef struct USBMIDIEncoder {
    uint8_t status, need, n;
    uint8_t msg[3];
    bool sysex;
} USBMIDIEncoder;

/* called with each finished 4-byte event packet */
typedef void USBMIDIPut(void *opaque, const uint8_t ev[4]);

void usbmidi_encoder_reset(USBMIDIEncoder *e);
void usbmidi_encode(USBMIDIEncoder *e, uint8_t b, USBMIDIPut *put,
                    void *opaque);

/*
 * event packets -> raw bytes; raw must hold 3 bytes per 4 in.
 * Returns the number of raw bytes.
 */
unsigned usbmidi_decode(const uint8_t *ev, unsigned n, uint8_t *raw);

#endif

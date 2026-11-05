/*
 * Elektron Analog Rytm MKII stand-in, in OVERBRIDGE mode (1935:0b48)
 *
 * A USB device with the Rytm's interface layout, whose other end is
 * rytm-emu's Overbridge link (rytm-emu src/oblink.rs) on a chardev, so a
 * guest that drives a real Rytm over USB (the Sidecar Pi engine) talks to an
 * emulated one:
 *
 *   interface 1 alt 3, interrupt IN 0x83, 368 B: Overbridge blocks to the
 *     host, 7 frames x 12 channels, paced at 48000 / 7 blocks a second of
 *     virtual time (the Rytm is the clock); the audio is the link's R frames
 *     when it sends them, silence otherwise
 *   interface 2 alt 3, interrupt OUT 0x03, 368 B: blocks from the host; their
 *     frames answer the link's N requests as A messages
 *   interface 5, USB MIDI bulk 0x01 / 0x81: event packets to and from the
 *     link's M byte stream (the Rytm's own USB MIDI port)
 *   interface 3 (bulk OUT 0x02, interrupt IN 0x82): present, idle
 *   vendor requests C0 00/01/02/04/05/06 and 40 01 (unit ID, name, OS
 *     version, protocol versions, error counters, set name): answered from
 *     properties, not from the emulated firmware
 *
 * The device is plugged while the chardev is connected and unplugged when it
 * closes, like a Rytm that reboots. The descriptors are built from the
 * documented layout (interfaces, alternate settings, endpoints, the MIDI
 * jacks); nothing here comes from Elektron's firmware.
 *
 * Local experiment for the Sidecar project's emulator gates; not intended
 * for upstream QEMU.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/cutils.h"
#include "qemu/timer.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/usb/usb.h"
#include "migration/vmstate.h"
#include "desc.h"
#include "chardev/char-fe.h"
#include "qom/object.h"

#define TYPE_USB_RYTM "usb-rytm"
OBJECT_DECLARE_SIMPLE_TYPE(USBRytmState, USB_RYTM)

#define OB_CHANNELS         12
#define OB_FRAMES           7               /* frames per block */
#define OB_BLOCK            368             /* 32-byte header + 7 x 12 x 4 */
#define OB_HDR              32
#define OB_MAGIC_IN         0x0700          /* Rytm -> host */
#define OB_ALT              3
#define OB_IFACE_IN         1
#define OB_IFACE_OUT        2
#define OB_EP_IN            3               /* 0x83 */
#define OB_EP_OUT           3               /* 0x03 */
#define MIDI_EP             1               /* 0x01 / 0x81 */
#define MIDI_MPS            512

#define IN_BLOCKS_MAX       8               /* blocks waiting for the host */
#define RFRAMES_MAX         (48000 / 4)     /* link R frames kept (250 ms) */
#define OFRAMES_MAX         (48000 / 2)     /* host frames kept for the link */
#define MIDI_IN_MAX         (64 * 1024)     /* event bytes waiting for 0x81 */
#define LINK_MSG_MAX        (65536 + 3)

/* the link's hello flags (oblink.rs) */
#define HELLO_MIDI          1

#define BLOCK_NS            (NANOSECONDS_PER_SECOND * OB_FRAMES / 48000)

struct USBRytmState {
    USBDevice dev;
    CharFrontend cs;

    /* properties */
    uint64_t uid;
    char *name;
    char *os_version;

    USBEndpoint *ep_in;                     /* 0x83 */
    USBEndpoint *ep_midi_in;                /* 0x81 */
    QEMUTimer *clock;
    int64_t next_block;

    /* Overbridge IN: whole blocks ready for 0x83 */
    uint8_t in_blocks[IN_BLOCKS_MAX][OB_BLOCK];
    unsigned in_head, in_count;
    uint16_t in_counter;

    /* the link's R frames (Rytm -> host audio), a ring */
    int32_t (*rframes)[OB_CHANNELS];
    unsigned r_head, r_count;

    /* frames from the host's OUT blocks, for the link's N */
    int32_t (*oframes)[OB_CHANNELS];
    unsigned o_head, o_count;
    uint64_t wanted;

    /* USB MIDI event packets for 0x81 */
    uint8_t *midi_in;
    unsigned mi_head, mi_count;

    /* raw MIDI -> event packets */
    uint8_t enc_status, enc_need, enc_n;
    uint8_t enc_msg[3];
    bool enc_sysex;

    /* the link's parser */
    uint8_t kind;
    uint8_t head[4];
    unsigned need, have;
    uint64_t body;
    uint8_t rframe_buf[OB_CHANNELS * 4];
    unsigned rframe_have;
    bool hello_sent, granted_midi, connected;

    /* counters (the monitor's "info qtree" shows none; trace by log) */
    uint64_t blocks_in, blocks_out, starved, out_dropped, midi_to_link,
             midi_from_link, link_bad;
};

/* ---------------------------------------------------------------------------
 * Descriptors
 */

enum {
    STR_MANUFACTURER = 1,
    STR_PRODUCT,
    STR_SERIALNUMBER,
};

static const USBDescStrings desc_strings = {
    [STR_MANUFACTURER] = "Elektron Music Machines",
    [STR_PRODUCT]      = "Analog Rytm MKII",
    [STR_SERIALNUMBER] = "000000000001",
};

/* interrupt endpoints of interfaces 1 and 2: alt 1..3 = 32, 88, 368 bytes */
static USBDescEndpoint ob_in_eps[3] = {
    { .bEndpointAddress = USB_DIR_IN | OB_EP_IN, .bmAttributes = USB_ENDPOINT_XFER_INT,
      .wMaxPacketSize = 32, .bInterval = 1 },
    { .bEndpointAddress = USB_DIR_IN | OB_EP_IN, .bmAttributes = USB_ENDPOINT_XFER_INT,
      .wMaxPacketSize = 88, .bInterval = 1 },
    { .bEndpointAddress = USB_DIR_IN | OB_EP_IN, .bmAttributes = USB_ENDPOINT_XFER_INT,
      .wMaxPacketSize = OB_BLOCK, .bInterval = 1 },
};

static USBDescEndpoint ob_out_eps[3] = {
    { .bEndpointAddress = USB_DIR_OUT | OB_EP_OUT, .bmAttributes = USB_ENDPOINT_XFER_INT,
      .wMaxPacketSize = 32, .bInterval = 1 },
    { .bEndpointAddress = USB_DIR_OUT | OB_EP_OUT, .bmAttributes = USB_ENDPOINT_XFER_INT,
      .wMaxPacketSize = 88, .bInterval = 1 },
    { .bEndpointAddress = USB_DIR_OUT | OB_EP_OUT, .bmAttributes = USB_ENDPOINT_XFER_INT,
      .wMaxPacketSize = OB_BLOCK, .bInterval = 1 },
};

static USBDescEndpoint pipe_eps[2] = {
    { .bEndpointAddress = USB_DIR_OUT | 2, .bmAttributes = USB_ENDPOINT_XFER_BULK,
      .wMaxPacketSize = 512 },
    { .bEndpointAddress = USB_DIR_IN | 2, .bmAttributes = USB_ENDPOINT_XFER_INT,
      .wMaxPacketSize = 64, .bInterval = 1 },
};

#define VENDOR_IF(n, alt, neps, ep) {                                       \
        .bInterfaceNumber = (n), .bAlternateSetting = (alt),                \
        .bNumEndpoints = (neps), .bInterfaceClass = USB_CLASS_VENDOR_SPEC,  \
        .eps = (ep) }

static const USBDescIface ob_ifaces[] = {
    VENDOR_IF(0, 0, 0, NULL),
    VENDOR_IF(1, 0, 0, NULL),
    VENDOR_IF(1, 1, 1, &ob_in_eps[0]),
    VENDOR_IF(1, 2, 1, &ob_in_eps[1]),
    VENDOR_IF(1, 3, 1, &ob_in_eps[2]),
    VENDOR_IF(2, 0, 0, NULL),
    VENDOR_IF(2, 1, 1, &ob_out_eps[0]),
    VENDOR_IF(2, 2, 1, &ob_out_eps[1]),
    VENDOR_IF(2, 3, 1, &ob_out_eps[2]),
    VENDOR_IF(3, 0, 2, pipe_eps),
};

/* USB MIDI 1.0: audio control header naming streaming interface 5 */
static const USBDescOther ac_descs[] = {
    { .data = (uint8_t[]) { 0x09, 0x24, 0x01, 0x00, 0x01, 0x09, 0x00, 0x01, 0x05 } },
};

/* MS header, jacks 1 (embedded IN), 2 (external IN), 3 (embedded OUT <- 2),
 * 4 (external OUT <- 1) */
static const USBDescOther ms_descs[] = {
    { .data = (uint8_t[]) { 0x07, 0x24, 0x01, 0x00, 0x01, 0x41, 0x00 } },
    { .data = (uint8_t[]) { 0x06, 0x24, 0x02, 0x01, 0x01, 0x00 } },
    { .data = (uint8_t[]) { 0x06, 0x24, 0x02, 0x02, 0x02, 0x00 } },
    { .data = (uint8_t[]) { 0x09, 0x24, 0x03, 0x01, 0x03, 0x01, 0x02, 0x01, 0x00 } },
    { .data = (uint8_t[]) { 0x09, 0x24, 0x03, 0x02, 0x04, 0x01, 0x01, 0x01, 0x00 } },
};

static USBDescEndpoint midi_eps[2] = {
    { .bEndpointAddress = USB_DIR_OUT | MIDI_EP, .bmAttributes = USB_ENDPOINT_XFER_BULK,
      .wMaxPacketSize = MIDI_MPS, .is_audio = 1,
      .extra = (uint8_t[]) { 0x05, 0x25, 0x01, 0x01, 0x01 } },
    { .bEndpointAddress = USB_DIR_IN | MIDI_EP, .bmAttributes = USB_ENDPOINT_XFER_BULK,
      .wMaxPacketSize = MIDI_MPS, .is_audio = 1,
      .extra = (uint8_t[]) { 0x05, 0x25, 0x01, 0x01, 0x03 } },
};

static const USBDescIface midi_ifaces[] = {
    {
        .bInterfaceNumber = 4, .bNumEndpoints = 0,
        .bInterfaceClass = USB_CLASS_AUDIO, .bInterfaceSubClass = 0x01,
        .ndesc = ARRAY_SIZE(ac_descs), .descs = (USBDescOther *)ac_descs,
    }, {
        .bInterfaceNumber = 5, .bNumEndpoints = 2,
        .bInterfaceClass = USB_CLASS_AUDIO, .bInterfaceSubClass = 0x03,
        .ndesc = ARRAY_SIZE(ms_descs), .descs = (USBDescOther *)ms_descs,
        .eps = midi_eps,
    },
};

static const USBDescIfaceAssoc rytm_groups[] = {
    {
        .bFirstInterface = 0, .bInterfaceCount = 4,
        .bFunctionClass = USB_CLASS_VENDOR_SPEC,
        .nif = ARRAY_SIZE(ob_ifaces), .ifs = ob_ifaces,
    }, {
        .bFirstInterface = 4, .bInterfaceCount = 2,
        .bFunctionClass = USB_CLASS_AUDIO, .bFunctionSubClass = 0x01,
        .nif = ARRAY_SIZE(midi_ifaces), .ifs = midi_ifaces,
    },
};

static const USBDescDevice desc_device_rytm = {
    .bcdUSB             = 0x0200,
    .bDeviceClass       = 0xef,             /* miscellaneous: IADs */
    .bDeviceSubClass    = 0x02,
    .bDeviceProtocol    = 0x01,
    .bMaxPacketSize0    = 64,
    .bNumConfigurations = 1,
    .confs = (USBDescConfig[]) {
        {
            .bNumInterfaces      = 6,
            .bConfigurationValue = 1,
            .bmAttributes        = USB_CFG_ATT_ONE | USB_CFG_ATT_SELFPOWER,
            .bMaxPower           = 0,
            .nif_groups          = ARRAY_SIZE(rytm_groups),
            .if_groups           = rytm_groups,
        },
    },
};

static const USBDesc desc_rytm = {
    .id = {
        .idVendor          = 0x1935,
        .idProduct         = 0x0b48,
        .bcdDevice         = 0x0100,
        .iManufacturer     = STR_MANUFACTURER,
        .iProduct          = STR_PRODUCT,
        .iSerialNumber     = STR_SERIALNUMBER,
    },
    .high = &desc_device_rytm,
    .str  = desc_strings,
};

/* ---------------------------------------------------------------------------
 * The link to rytm-emu
 */

static void rytm_link_write(USBRytmState *s, const uint8_t *d, size_t n)
{
    if (s->connected) {
        qemu_chr_fe_write_all(&s->cs, d, n);
    }
}

static bool rytm_streaming(USBRytmState *s)
{
    return s->dev.configuration && s->dev.altsetting[OB_IFACE_IN] == OB_ALT;
}

/* host frames -> the link, as much as it asked for */
static void rytm_link_send_frames(USBRytmState *s)
{
    uint8_t *m;
    unsigned n, i, c, o = 0;

    n = MIN(s->wanted, s->o_count);
    if (!n || !s->connected) {
        return;
    }
    m = g_malloc(5 + n * OB_CHANNELS * 4);
    m[o++] = 'A';
    stl_le_p(m + o, n);
    o += 4;
    for (i = 0; i < n; i++) {
        int32_t *f = s->oframes[s->o_head];
        for (c = 0; c < OB_CHANNELS; c++) {
            stl_le_p(m + o, f[c]);
            o += 4;
        }
        s->o_head = (s->o_head + 1) % OFRAMES_MAX;
    }
    s->o_count -= n;
    s->wanted -= n;
    rytm_link_write(s, m, o);
    g_free(m);
}

static void rytm_link_send_midi(USBRytmState *s, const uint8_t *d, unsigned n)
{
    uint8_t h[3];

    if (!s->granted_midi || !n) {
        return;
    }
    h[0] = 'M';
    stw_le_p(h + 1, n);
    rytm_link_write(s, h, 3);
    rytm_link_write(s, d, n);
    s->midi_to_link += n;
}

/* ---------------------------------------------------------------------------
 * USB MIDI event packets
 */

static void rytm_midi_in_put(USBRytmState *s, uint8_t cin, const uint8_t *b)
{
    uint8_t ev[4] = { cin & 0x0f, b[0], b[1], b[2] };   /* cable 0 */
    unsigned i;

    if (s->mi_count + 4 > MIDI_IN_MAX) {
        return;     /* the host stopped reading: drop */
    }
    for (i = 0; i < 4; i++) {
        s->midi_in[(s->mi_head + s->mi_count + i) % MIDI_IN_MAX] = ev[i];
    }
    s->mi_count += 4;
}

/* the link's raw MIDI bytes -> event packets for 0x81 */
static void rytm_midi_encode(USBRytmState *s, uint8_t b)
{
    if (b >= 0xf8) {                         /* real time, anywhere */
        uint8_t m[3] = { b, 0, 0 };
        rytm_midi_in_put(s, 0xf, m);
        return;
    }
    if (b == 0xf0) {
        s->enc_sysex = true;
        s->enc_status = 0;
        s->enc_msg[0] = b;
        s->enc_n = 1;
        return;
    }
    if (b == 0xf7) {                         /* an F7 outside a SysEx */
        s->enc_sysex = false;
        s->enc_n = 0;
        return;
    }
    if (s->enc_sysex) {
        if (b & 0x80) {                      /* a status ends it, unterminated */
            s->enc_sysex = false;
            s->enc_n = 0;
        } else {
            s->enc_msg[s->enc_n++] = b;
            if (s->enc_n == 3) {
                rytm_midi_in_put(s, 0x4, s->enc_msg);
                s->enc_n = 0;
            }
            return;
        }
    }
    if (b & 0x80) {
        s->enc_status = b;
        s->enc_n = 0;
        switch (b & 0xf0) {
        case 0xc0: case 0xd0:
            s->enc_need = 1;
            break;
        case 0xf0:
            s->enc_need = (b == 0xf2) ? 2 : (b == 0xf1 || b == 0xf3) ? 1 : 0;
            if (!s->enc_need) {              /* F6 tune request; F4/F5 */
                uint8_t m[3] = { b, 0, 0 };
                if (b == 0xf6) {
                    rytm_midi_in_put(s, 0x5, m);
                }
                s->enc_status = 0;
            }
            break;
        default:
            s->enc_need = 2;
        }
        return;
    }
    if (!s->enc_status) {
        return;                              /* data without a status */
    }
    s->enc_msg[1 + s->enc_n++] = b;
    if (s->enc_n == s->enc_need) {
        uint8_t m[3] = { s->enc_status, s->enc_msg[1],
                         s->enc_need > 1 ? s->enc_msg[2] : 0 };
        uint8_t cin = s->enc_status < 0xf0 ? s->enc_status >> 4
                    : s->enc_need == 1 ? 0x2 : 0x3;
        rytm_midi_in_put(s, cin, m);
        s->enc_n = 0;
        if (s->enc_status >= 0xf0) {
            s->enc_status = 0;               /* no running status */
        }
    }
}

/* the SysEx end: CIN 5, 6 or 7 by the bytes in its last packet */
static void rytm_midi_encode_byte(USBRytmState *s, uint8_t b)
{
    if (b == 0xf7 && s->enc_sysex) {
        unsigned n = s->enc_n + 1;           /* with the F7 */
        uint8_t m[3] = { 0, 0, 0 };
        memcpy(m, s->enc_msg, s->enc_n);
        m[s->enc_n] = 0xf7;
        rytm_midi_in_put(s, 0x4 + n, m);     /* 5, 6 or 7 */
        s->enc_sysex = false;
        s->enc_n = 0;
        return;
    }
    rytm_midi_encode(s, b);
}

static const uint8_t cin_len[16] = {
    0, 0, 2, 3, 3, 1, 2, 3, 3, 3, 3, 3, 2, 2, 3, 1
};

/* event packets from the host (0x01) -> raw MIDI for the link */
static void rytm_midi_decode(USBRytmState *s, const uint8_t *d, unsigned n)
{
    g_autofree uint8_t *raw = g_malloc(n);
    unsigned i, o = 0;

    for (i = 0; i + 4 <= n; i += 4) {
        unsigned len = cin_len[d[i] & 0x0f];
        memcpy(raw + o, d + i + 1, len);
        o += len;
    }
    rytm_link_send_midi(s, raw, o);
}

/* ---------------------------------------------------------------------------
 * Overbridge blocks
 */

static void rytm_make_block(USBRytmState *s)
{
    uint8_t *blk;
    unsigned f, c;

    if (s->in_count == IN_BLOCKS_MAX) {
        return;     /* the host is not reading: it falls behind, as a Rytm */
    }
    blk = s->in_blocks[(s->in_head + s->in_count) % IN_BLOCKS_MAX];
    memset(blk, 0, OB_HDR);
    stw_be_p(blk, OB_MAGIC_IN);
    stw_be_p(blk + 2, s->in_counter);
    s->in_counter += OB_FRAMES;
    for (f = 0; f < OB_FRAMES; f++) {
        int32_t *fr = NULL;
        if (s->r_count) {
            fr = s->rframes[s->r_head];
            s->r_head = (s->r_head + 1) % RFRAMES_MAX;
            s->r_count--;
        } else {
            s->starved++;
        }
        for (c = 0; c < OB_CHANNELS; c++) {
            stl_be_p(blk + OB_HDR + (f * OB_CHANNELS + c) * 4, fr ? fr[c] : 0);
        }
    }
    s->in_count++;
}

static void rytm_clock(void *opaque)
{
    USBRytmState *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    unsigned made = 0;

    if (!rytm_streaming(s)) {
        s->next_block = 0;
        return;     /* restarted by SET_INTERFACE alt 3 */
    }
    if (!s->next_block) {
        s->next_block = now;
    }
    /* catch up after a stall, but not by more than the queue */
    while (s->next_block <= now && made < IN_BLOCKS_MAX) {
        rytm_make_block(s);
        s->next_block += BLOCK_NS;
        made++;
    }
    if (s->next_block <= now) {
        s->next_block = now + BLOCK_NS;
    }
    if (made) {
        usb_wakeup(s->ep_in, 0);
    }
    timer_mod(s->clock, s->next_block);
}

static void rytm_take_out_block(USBRytmState *s, const uint8_t *blk)
{
    unsigned f, c;

    s->blocks_out++;
    for (f = 0; f < OB_FRAMES; f++) {
        int32_t *fr;
        if (s->o_count == OFRAMES_MAX) {
            s->out_dropped++;
            continue;
        }
        fr = s->oframes[(s->o_head + s->o_count) % OFRAMES_MAX];
        for (c = 0; c < OB_CHANNELS; c++) {
            fr[c] = ldl_be_p(blk + OB_HDR + (f * OB_CHANNELS + c) * 4);
        }
        s->o_count++;
    }
    rytm_link_send_frames(s);
}

/* ---------------------------------------------------------------------------
 * The link's messages (emulator -> host)
 */

static void rytm_link_reset(USBRytmState *s)
{
    s->kind = 0;
    s->need = s->have = 0;
    s->body = 0;
    s->rframe_have = 0;
    s->wanted = 0;
    s->hello_sent = s->granted_midi = false;
    s->r_count = 0;
}

static void rytm_link_hello(USBRytmState *s)
{
    uint8_t h[3] = { 'H', 1, HELLO_MIDI };

    rytm_link_write(s, h, sizeof(h));
    s->hello_sent = true;
}

static void rytm_link_body(USBRytmState *s, const uint8_t *d, unsigned n)
{
    unsigned i;

    switch (s->kind) {
    case 'M':
        s->midi_from_link += n;
        for (i = 0; i < n; i++) {
            rytm_midi_encode_byte(s, d[i]);
        }
        if (s->mi_count) {
            usb_wakeup(s->ep_midi_in, 0);
        }
        break;
    case 'R':
        for (i = 0; i < n; i++) {
            s->rframe_buf[s->rframe_have++] = d[i];
            if (s->rframe_have == sizeof(s->rframe_buf)) {
                int32_t *fr;
                unsigned c;
                if (s->r_count == RFRAMES_MAX) {    /* keep the newest */
                    s->r_head = (s->r_head + 1) % RFRAMES_MAX;
                    s->r_count--;
                }
                fr = s->rframes[(s->r_head + s->r_count) % RFRAMES_MAX];
                for (c = 0; c < OB_CHANNELS; c++) {
                    fr[c] = ldl_le_p(s->rframe_buf + c * 4);
                }
                s->r_count++;
                s->rframe_have = 0;
            }
        }
        break;
    }
}

static void rytm_link_head_done(USBRytmState *s)
{
    switch (s->kind) {
    case 'N':
        s->wanted += ldl_le_p(s->head);
        if (s->wanted > 48000) {
            s->wanted = 48000;
        }
        rytm_link_send_frames(s);
        s->kind = 0;
        break;
    case 'H':
        s->granted_midi = s->head[1] & HELLO_MIDI;
        s->kind = 0;
        break;
    case 'T': case 'P':
        s->kind = 0;    /* the EXT machine stand-ins: not used */
        break;
    case 'R':
        s->body = (uint64_t)ldl_le_p(s->head) * OB_CHANNELS * 4;
        s->rframe_have = 0;
        break;
    case 'M':
        s->body = lduw_le_p(s->head);
        break;
    }
    if (s->kind && !s->body) {
        s->kind = 0;
    }
}

static void rytm_link_feed(USBRytmState *s, const uint8_t *d, unsigned n)
{
    while (n) {
        if (!s->kind) {
            s->kind = *d++;
            n--;
            s->have = 0;
            switch (s->kind) {
            case 'N': case 'R': s->need = 4; break;
            case 'H': s->need = 2; break;
            case 'T': s->need = 3; break;
            case 'P': s->need = 9; break;
            case 'M': s->need = 2; break;
            default:
                s->link_bad++;
                qemu_log_mask(LOG_GUEST_ERROR,
                              "usb-rytm: link: unknown message 0x%02x\n",
                              s->kind);
                s->kind = 0;    /* resync on the next byte */
                continue;
            }
            continue;
        }
        if (s->have < s->need) {
            unsigned k = MIN(n, s->need - s->have);
            if (s->have + k <= sizeof(s->head)) {
                memcpy(s->head + s->have, d, k);
            }
            s->have += k;
            d += k;
            n -= k;
            if (s->have == s->need) {
                rytm_link_head_done(s);
            }
            continue;
        }
        {
            unsigned k = MIN((uint64_t)n, s->body);
            rytm_link_body(s, d, k);
            s->body -= k;
            d += k;
            n -= k;
            if (!s->body) {
                s->kind = 0;
            }
        }
    }
}

static int rytm_can_read(void *opaque)
{
    return 4096;
}

static void rytm_read(void *opaque, const uint8_t *buf, int size)
{
    rytm_link_feed(opaque, buf, size);
}

static void rytm_event(void *opaque, QEMUChrEvent event)
{
    USBRytmState *s = opaque;

    switch (event) {
    case CHR_EVENT_OPENED:
        rytm_link_reset(s);
        s->connected = true;
        rytm_link_hello(s);
        if (!s->dev.attached) {
            usb_device_attach(&s->dev, &error_abort);
        }
        break;
    case CHR_EVENT_CLOSED:
        s->connected = false;
        rytm_link_reset(s);
        if (s->dev.attached) {
            usb_device_detach(&s->dev);  /* the Rytm went away (rebooting) */
        }
        break;
    default:
        break;
    }
}

/* ---------------------------------------------------------------------------
 * USB
 */

static void rytm_handle_reset(USBDevice *dev)
{
    USBRytmState *s = USB_RYTM(dev);

    s->in_head = s->in_count = 0;
    s->o_head = s->o_count = 0;
    s->mi_head = s->mi_count = 0;
    s->enc_sysex = false;
    s->enc_status = s->enc_n = 0;
    s->in_counter = 0;
    s->next_block = 0;
    timer_del(s->clock);
}

static void rytm_set_interface(USBDevice *dev, int iface, int old, int value)
{
    USBRytmState *s = USB_RYTM(dev);

    if (iface == OB_IFACE_IN) {
        s->in_head = s->in_count = 0;
        s->next_block = 0;
        if (value == OB_ALT) {
            rytm_clock(s);
        } else {
            timer_del(s->clock);
        }
    }
    if (iface == OB_IFACE_OUT && value != OB_ALT) {
        s->o_head = s->o_count = 0;
    }
}

/* the vendor requests Overbridge makes (device recipient, wValue 0) */
static int rytm_vendor_in(USBRytmState *s, int request, uint8_t *data,
                          int length)
{
    uint8_t buf[32] = { 0 };
    int n;

    switch (request & 0xff) {
    case 0x00:                              /* unit ID */
        stq_be_p(buf, s->uid);
        n = 8;
        break;
    case 0x01:                              /* device name */
        strpadcpy((char *)buf, 32, s->name, 0);
        n = 32;
        break;
    case 0x02:                              /* OS version */
        strpadcpy((char *)buf, 16, s->os_version, 0);
        n = 16;
        break;
    case 0x04:                              /* protocol versions */
        stw_be_p(buf, 1);
        stw_be_p(buf + 2, 1);
        n = 4;
        break;
    case 0x05:                              /* streaming errors */
        n = 4;
        break;
    case 0x06:                              /* USB error counters */
        n = 24;
        break;
    default:
        return -1;
    }
    n = MIN(n, length);
    memcpy(data, buf, n);
    return n;
}

static void rytm_handle_control(USBDevice *dev, USBPacket *p, int request,
                                int value, int index, int length,
                                uint8_t *data)
{
    USBRytmState *s = USB_RYTM(dev);
    int ret;

    ret = usb_desc_handle_control(dev, p, request, value, index, length, data);
    if (ret >= 0) {
        return;
    }
    switch (request >> 8) {
    case USB_DIR_IN | USB_TYPE_VENDOR | USB_RECIP_DEVICE:
        ret = rytm_vendor_in(s, request, data, length);
        if (ret >= 0) {
            p->actual_length = ret;
            return;
        }
        break;
    case USB_DIR_OUT | USB_TYPE_VENDOR | USB_RECIP_DEVICE:
        if ((request & 0xff) == 0x01) {     /* set name */
            g_free(s->name);
            s->name = g_strndup((char *)data, MIN(length, 32));
            return;
        }
        break;
    }
    p->status = USB_RET_STALL;
}

static void rytm_handle_data(USBDevice *dev, USBPacket *p)
{
    USBRytmState *s = USB_RYTM(dev);
    uint8_t buf[MIDI_MPS * 8];
    size_t n;

    switch (p->pid) {
    case USB_TOKEN_IN:
        if (p->ep->nr == OB_EP_IN && p->ep == s->ep_in) {
            if (!s->in_count || p->iov.size < OB_BLOCK) {
                p->status = USB_RET_NAK;
                return;
            }
            usb_packet_copy(p, s->in_blocks[s->in_head], OB_BLOCK);
            s->in_head = (s->in_head + 1) % IN_BLOCKS_MAX;
            s->in_count--;
            s->blocks_in++;
            return;
        }
        if (p->ep->nr == MIDI_EP) {
            n = MIN(MIN(p->iov.size, (size_t)s->mi_count), sizeof(buf)) & ~3u;
            if (!n) {
                p->status = USB_RET_NAK;
                return;
            }
            for (size_t i = 0; i < n; i++) {
                buf[i] = s->midi_in[(s->mi_head + i) % MIDI_IN_MAX];
            }
            s->mi_head = (s->mi_head + n) % MIDI_IN_MAX;
            s->mi_count -= n;
            usb_packet_copy(p, buf, n);
            return;
        }
        p->status = USB_RET_NAK;            /* 0x82: idle */
        return;
    case USB_TOKEN_OUT:
        if (p->ep->nr == OB_EP_OUT) {
            uint8_t blk[OB_BLOCK];
            if (p->iov.size >= OB_BLOCK) {
                usb_packet_copy(p, blk, OB_BLOCK);
                rytm_take_out_block(s, blk);
            } else {
                usb_packet_skip(p, p->iov.size);
            }
            return;
        }
        if (p->ep->nr == MIDI_EP) {
            while (p->actual_length < p->iov.size) {
                n = MIN(p->iov.size - p->actual_length, sizeof(buf));
                usb_packet_copy(p, buf, n);
                rytm_midi_decode(s, buf, n);
            }
            return;
        }
        usb_packet_skip(p, p->iov.size);    /* 0x02: accepted, unused */
        return;
    default:
        p->status = USB_RET_STALL;
    }
}

static void rytm_realize(USBDevice *dev, Error **errp)
{
    USBRytmState *s = USB_RYTM(dev);
    Error *local_err = NULL;

    usb_desc_init(dev);
    dev->auto_attach = 0;

    if (!qemu_chr_fe_backend_connected(&s->cs)) {
        error_setg(errp, "usb-rytm: property chardev is required "
                   "(rytm-emu's Overbridge link, e.g. a socket to port 7301)");
        return;
    }
    usb_check_attach(dev, &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
        return;
    }

    s->rframes = g_malloc0(sizeof(*s->rframes) * RFRAMES_MAX);
    s->oframes = g_malloc0(sizeof(*s->oframes) * OFRAMES_MAX);
    s->midi_in = g_malloc0(MIDI_IN_MAX);
    s->clock = timer_new_ns(QEMU_CLOCK_VIRTUAL, rytm_clock, s);
    s->ep_in = usb_ep_get(dev, USB_TOKEN_IN, OB_EP_IN);
    s->ep_midi_in = usb_ep_get(dev, USB_TOKEN_IN, MIDI_EP);

    qemu_chr_fe_set_handlers(&s->cs, rytm_can_read, rytm_read, rytm_event,
                             NULL, s, NULL, true);
    if (qemu_chr_fe_backend_open(&s->cs) && !dev->attached) {
        usb_device_attach(dev, &error_abort);
    }
}

static void rytm_unrealize(USBDevice *dev)
{
    USBRytmState *s = USB_RYTM(dev);

    qemu_chr_fe_deinit(&s->cs, false);
    timer_free(s->clock);
    g_free(s->rframes);
    g_free(s->oframes);
    g_free(s->midi_in);
}

static const VMStateDescription vmstate_usb_rytm = {
    .name = TYPE_USB_RYTM,
    .unmigratable = 1,
};

static const Property rytm_properties[] = {
    DEFINE_PROP_CHR("chardev", USBRytmState, cs),
    DEFINE_PROP_UINT64("uid", USBRytmState, uid, 0x00000000c0ffee01ULL),
    DEFINE_PROP_STRING("name", USBRytmState, name),
    DEFINE_PROP_STRING("os-version", USBRytmState, os_version),
};

static void rytm_instance_init(Object *obj)
{
    USBRytmState *s = USB_RYTM(obj);

    s->name = g_strdup("Analog Rytm MKII (QEMU)");
    s->os_version = g_strdup("1.74");
}

static void rytm_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    USBDeviceClass *uc = USB_DEVICE_CLASS(klass);

    uc->product_desc   = "Analog Rytm MKII stand-in";
    uc->usb_desc       = &desc_rytm;
    uc->realize        = rytm_realize;
    uc->unrealize      = rytm_unrealize;
    uc->handle_attach  = usb_desc_attach;
    uc->handle_reset   = rytm_handle_reset;
    uc->handle_control = rytm_handle_control;
    uc->handle_data    = rytm_handle_data;
    uc->set_interface  = rytm_set_interface;
    dc->vmsd = &vmstate_usb_rytm;
    device_class_set_props(dc, rytm_properties);
    set_bit(DEVICE_CATEGORY_SOUND, dc->categories);
}

static const TypeInfo rytm_info = {
    .name          = TYPE_USB_RYTM,
    .parent        = TYPE_USB_DEVICE,
    .instance_size = sizeof(USBRytmState),
    .instance_init = rytm_instance_init,
    .class_init    = rytm_class_init,
};

static void rytm_register_types(void)
{
    type_register_static(&rytm_info);
}

type_init(rytm_register_types)

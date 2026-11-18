/*
 * Teensy 4.1 mirror relay stand-in (1209:0002) and its HalfKay bootloader
 * (16C0:0478), for the Sidecar Pi engine's relay and flash paths
 *
 * Not an emulation of the i.MX RT or of the relay firmware: a USB device that
 * speaks the link protocol of sidecar-firmware pi/engine/relay_proto.h
 * (version 1.02) the way teensy/mirror_relay/relay.cpp does, so the Pi's
 * CTeensyTarget and teensy_flash.cpp run against it:
 *
 *   link (1209:0002, bcdDevice 0x0102, high speed, one vendor interface
 *   0xFF/0x53/0x01, all bulk 512 B):
 *     0x81 IN    events (16 B each, one per packet)
 *     0x82 IN    the computer's audio OUT blocks for the Pi
 *     0x02 OUT   audio IN blocks for the computer
 *     0x83..0x86 IN / 0x03..0x06 OUT   channels 0..3 (pipes, MIDI)
 *   EP0 vendor requests 0x01..0x0D: descriptor upload, connect, the
 *   forwarded control requests (kReqCtlAnswer / kReqCtlData), credits,
 *   stats, log, halt, bootloader
 *
 *   HalfKay (16C0:0478, high speed, one HID interface): SET_REPORT
 *   (0x21 09 0200) reports of 1088 bytes, 24-bit block offset + 60 zero
 *   bytes + 1 KB of program; offset 0xFFFFFF starts the program. Block 0
 *   erases the chip: reports are refused (STALL) for erase-ms after it.
 *   After the start the relay comes back with the build flash-build (or
 *   "stub-<crc32 of the program>") in its start line.
 *
 * The computer side of the relay (the Teensy's USB2) is an optional chardev
 * whose other end plays the computer (sidecar/tools/teensystub.py). Frames
 * both ways are [kind u8][length u16 LE][payload]:
 *   computer -> stub
 *     'S' setup (8 B) + OUT data   a control request on the box's EP0;
 *                                  standard ones are answered here as the
 *                                  relay does, class/vendor ones go to the
 *                                  Pi as kEvSetup
 *     'R'                          bus reset (kEvBus reset)
 *     'U'                          suspend (kEvBus suspend)
 *     'O' ep + data                a packet the computer sent to the box's
 *                                  OUT endpoint ep (0x03: audio; others:
 *                                  their channel, held until the Pi grants
 *                                  credits)
 *     'P' n                        0 unplug the Teensy, 1 plug it in (power
 *                                  up as the relay, with the program last
 *                                  written), 2 the program button (HalfKay
 *                                  now)
 *     'B' text                     the build the next flash comes back with
 *   stub -> computer
 *     'V' n                        1: the relay shows the box (the computer
 *                                  should reset and enumerate it), 0: gone
 *     'C' status + data            the answer to the last 'S' (status 0 ok,
 *                                  1 STALL)
 *     'I' ep + data                a packet from the box's IN endpoint ep
 *     'H' ep + on                  kReqHalt
 *     'L' text                     the stub's own lines (mode changes,
 *                                  blocks written)
 *
 * Fault options model the relay and flash flows that failed on the unit:
 * bl-ignore (the bootloader request is taken but nothing happens),
 * bl-stay (HalfKay keeps running after the start report), drop-block=N (block
 * N's report is never answered), leave-quiet-ms (before leaving, every
 * transfer ends in a transaction error that long, as the Teensy did ~50 ms
 * after kReqBootloader on the unit), start-halfkay (power up in the
 * bootloader: the recovery path).
 *
 * Local experiment for the Sidecar project's emulator gates; not intended
 * for upstream QEMU. Nothing here comes from PJRC's or Elektron's firmware;
 * the HalfKay descriptor values are the stub's own (bcdDevice is a property).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qemu/cutils.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/usb/usb.h"
#include "migration/vmstate.h"
#include "desc.h"
#include "chardev/char-fe.h"
#include "qom/object.h"

#define TYPE_USB_TEENSY "usb-teensy"
OBJECT_DECLARE_SIMPLE_TYPE(USBTeensyState, USB_TEENSY)

/* ---- relay_proto.h 1.02 -------------------------------------------------- */

#define RL_VID              0x1209
#define RL_PID              0x0002
#define RL_VERSION          0x0102
#define RL_EV_EP            1               /* 0x81 */
#define RL_AUDIO_TO_PI_EP   2               /* 0x82 */
#define RL_AUDIO_FROM_PI_EP 2               /* 0x02 */
#define RL_CHANNELS         4
#define RL_CHAN_FIRST_EP    3
#define RL_LINK_MPS         512
#define RL_CHAN_DEPTH       4
#define RL_MAX_CREDITS      RL_CHAN_DEPTH

enum {
    REQ_SET_DEVICE = 0x01, REQ_SET_CONFIG, REQ_SET_STRING, REQ_SET_EXTRA,
    REQ_CONNECT, REQ_CTL_ANSWER, REQ_CTL_DATA, REQ_CREDIT, REQ_STATS,
    REQ_STATS_RESET, REQ_LOG, REQ_HALT, REQ_BOOTLOADER,
};
#define ANS_STALL           1

enum {
    EV_HELLO = 1, EV_BUS, EV_CONFIG, EV_ALT, EV_SETUP, EV_ABORT, EV_IN_TAKEN,
    EV_DROP,
};
enum { BUS_RESET = 1, BUS_SUSPEND, BUS_RESUME, BUS_OFF };
enum { DROP_AUDIO_TO_PC = 1, DROP_AUDIO_TO_PI, DROP_NO_CREDIT, DROP_BAD_LEN };

#define STATS_MAGIC         0x594C5252      /* "RRLY" */
#define HIST_BINS           16

/* the Stats layout of relay_proto.h, field for field (the Pi reads its size) */
typedef struct RelayStats {
    uint32_t magic, version;
    uint32_t uptime_ms;
    uint32_t link_hs, link_configured, link_resets;
    uint32_t pc_hs, pc_configured, pc_resets, pc_suspends;
    uint32_t shown;
    uint32_t setups, ctl_forwarded, ctl_answered, ctl_stalled, ctl_aborted,
             ctl_late;
    uint32_t audio_to_pc, audio_to_pc_drop, audio_to_pc_empty;
    uint32_t audio_to_pi, audio_to_pi_drop;
    uint32_t chan_to_pc[RL_CHANNELS], chan_to_pi[RL_CHANNELS],
             chan_nocredit[RL_CHANNELS];
    uint32_t events, events_drop;
    uint32_t q_to_pc_max, q_to_pi_max;
    uint32_t hist_to_pc[HIST_BINS];
    uint32_t hist_to_pi[HIST_BINS];
    uint32_t worst_to_pc_us, worst_to_pi_us;
    uint32_t isr1, isr2;
    uint32_t isr1_max_us, isr2_max_us;
    uint32_t link_gap_max_us;
    uint32_t link_gap_hist[HIST_BINS];
    uint32_t link_dry;
    uint32_t link_nak_polls;
    uint32_t pc_audio_err;
    uint32_t pc_in_gap[8], pc_out_gap[8], pc_in_gap_max_us, pc_out_gap_max_us;
    uint32_t pc_idle_enter, pc_idle_leave;
    uint32_t pace_primed, pace_held, pace_empty;
} RelayStats;

/* the box's audio endpoints (ob::kInEp / ob::kOutEp), as the relay has them */
#define BOX_AUDIO_IN        0x83
#define BOX_AUDIO_OUT       0x03

/* mirror_desc.h limits */
#define MIR_MAX_CONFIG      512
#define MIR_MAX_EPS         14
#define MIR_MAX_IFACES      16
#define MIR_MAX_STRINGS     24

/* ---- HalfKay (Teensy 4.x) ------------------------------------------------ */

#define HK_VID              0x16c0
#define HK_PID              0x0478
#define HK_REPORT           1088            /* 64 B header + 1 KB block */
#define HK_BLOCK            1024
#define HK_CODE_SIZE        0x7c0000        /* teensy_hex.h kCodeSize */
#define HK_START            0xffffff
#define HK_DT_HID           0x21
#define HK_DT_REPORT        0x22

/* ---- the stub ------------------------------------------------------------ */

enum { MODE_GONE, MODE_RELAY, MODE_HALFKAY };

/* a leave: the device still answers (PRE), fails every transfer (QUIET),
 * is unplugged (GONE), then comes back as next_mode */
enum { PH_NONE, PH_PRE, PH_QUIET, PH_GONE };

#define EV_RING             64
#define AQ                  48              /* audio blocks queued each way */
#define PEND_MAX            64              /* computer packets held without credit */
#define LOG_RING            2048
#define PC_MSG_MAX          (65535 + 3)

typedef struct Packet {
    uint16_t len;
    uint8_t d[RL_LINK_MPS];
} Packet;

typedef struct MirEp {
    uint8_t addr, attr, iface, role;
    uint16_t mps;
} MirEp;

enum { ROLE_NONE, ROLE_AUDIO_IN, ROLE_AUDIO_OUT, ROLE_PIPE_IN, ROLE_PIPE_OUT,
       ROLE_MIDI_IN, ROLE_MIDI_OUT };

struct USBTeensyState {
    USBDevice dev;
    CharFrontend cs;                        /* the computer (optional) */

    /* properties */
    char *build;
    char *flash_build;
    uint32_t enter_ms, halfkay_ms, erase_ms, boot_ms, connect_ms,
             leave_quiet_ms;
    uint16_t halfkay_bcd;
    int32_t drop_block;
    bool bl_ignore, bl_stay, start_halfkay, verbose;

    int mode;
    bool quiet;                             /* leaving: transfers fail */
    QEMUTimer *t_mode;                      /* the next phase of a leave */
    int next_mode;
    int phase;                              /* PH_* */
    QEMUTimer *t_connect;
    char cur_build[32];

    /* link state */
    bool link_configured;
    uint8_t ev_ring[EV_RING][16];
    unsigned ev_head, ev_count;
    char log_ring[LOG_RING];
    unsigned log_head, log_count;
    RelayStats st;
    int64_t boot_ns;

    /* the box as uploaded */
    uint8_t dev_desc[18];
    bool have_dev;
    uint8_t cfg_desc[MIR_MAX_CONFIG];
    unsigned cfg_len;
    struct { uint8_t index, len; uint8_t data[254]; } strs[MIR_MAX_STRINGS];
    unsigned n_strs;
    MirEp eps[MIR_MAX_EPS];
    unsigned n_eps;
    int8_t ep_of_in[RL_CHANNELS], ep_of_out[RL_CHANNELS];
    bool shown;
    uint8_t pc_config;
    uint8_t pc_alt[MIR_MAX_IFACES];

    /* the computer's control request in flight */
    uint8_t ctl_seq;
    bool ctl_pending;
    uint8_t ctl_setup[8];
    uint8_t ctl_data[512];
    uint16_t ctl_data_len;

    /* audio */
    Packet aq_pi[AQ];                       /* computer -> Pi (link 0x82) */
    unsigned aq_pi_head, aq_pi_count;

    /* channels: computer -> Pi */
    Packet ch_to_pi[RL_CHANNELS][RL_CHAN_DEPTH];
    unsigned ch_head[RL_CHANNELS], ch_count[RL_CHANNELS];
    Packet pend[RL_CHANNELS][PEND_MAX];     /* waiting for credits */
    unsigned pend_head[RL_CHANNELS], pend_count[RL_CHANNELS];
    uint8_t credits[RL_CHANNELS];

    /* HalfKay */
    USBDesc hk_desc;                        /* bcdDevice is the property's */
    uint8_t *image;
    int64_t busy_until;
    unsigned blocks_written, refusals;
    bool dropped_once;
    USBPacket *held;                        /* drop-block's report */

    /* the computer's chardev */
    bool pc_connected;
    uint8_t *pc_msg;
    unsigned pc_have;

    USBEndpoint *ep_ev, *ep_audio, *ep_chan[RL_CHANNELS];
};

/* ---------------------------------------------------------------------------
 * Descriptors
 */

enum { STR_MANUFACTURER = 1, STR_PRODUCT, STR_SERIALNUMBER };

static const USBDescStrings link_strings = {
    [STR_MANUFACTURER] = "Sidecar",
    [STR_PRODUCT]      = "Sidecar mirror relay",
    [STR_SERIALNUMBER] = "SMR1",
};

#define LINK_EP(a) { .bEndpointAddress = (a), \
        .bmAttributes = USB_ENDPOINT_XFER_BULK, .wMaxPacketSize = RL_LINK_MPS }

static USBDescEndpoint link_eps[] = {
    LINK_EP(USB_DIR_IN | RL_EV_EP),
    LINK_EP(USB_DIR_IN | RL_AUDIO_TO_PI_EP),
    LINK_EP(USB_DIR_OUT | RL_AUDIO_FROM_PI_EP),
    LINK_EP(USB_DIR_IN | 3), LINK_EP(USB_DIR_OUT | 3),
    LINK_EP(USB_DIR_IN | 4), LINK_EP(USB_DIR_OUT | 4),
    LINK_EP(USB_DIR_IN | 5), LINK_EP(USB_DIR_OUT | 5),
    LINK_EP(USB_DIR_IN | 6), LINK_EP(USB_DIR_OUT | 6),
};

static const USBDescIface link_iface = {
    .bInterfaceNumber   = 0,
    .bNumEndpoints      = ARRAY_SIZE(link_eps),
    .bInterfaceClass    = 0xff,
    .bInterfaceSubClass = 0x53,
    .bInterfaceProtocol = 0x01,
    .eps                = link_eps,
};

static const USBDescDevice link_device = {
    .bcdUSB             = 0x0210,
    .bMaxPacketSize0    = 64,
    .bNumConfigurations = 1,
    .confs = (USBDescConfig[]) {
        {
            .bNumInterfaces      = 1,
            .bConfigurationValue = 1,
            .bmAttributes        = USB_CFG_ATT_ONE,
            .bMaxPower           = 50,
            .nif                 = 1,
            .ifs                 = &link_iface,
        },
    },
};

static const USBDesc desc_link = {
    .id = {
        .idVendor      = RL_VID,
        .idProduct     = RL_PID,
        .bcdDevice     = RL_VERSION,
        .iManufacturer = STR_MANUFACTURER,
        .iProduct      = STR_PRODUCT,
        .iSerialNumber = STR_SERIALNUMBER,
    },
    .high = &link_device,
    .str  = link_strings,
};

/* HalfKay: one HID interface whose output report is the 1088-byte block */
static const uint8_t hk_report_desc[] = {
    0x06, 0x9c, 0xff,           /* usage page 0xFF9C (vendor) */
    0x09, 0x24,                 /* usage 0x24 */
    0xa1, 0x01,                 /* collection application */
    0x75, 0x08,                 /*   report size 8 */
    0x15, 0x00,                 /*   logical minimum 0 */
    0x26, 0xff, 0x00,           /*   logical maximum 255 */
    0x96, HK_REPORT & 0xff, HK_REPORT >> 8,     /* report count 1088 */
    0x09, 0x01,                 /*   usage 1 */
    0x91, 0x02,                 /*   output (data, variable, absolute) */
    0xc0,                       /* end collection */
};

static const USBDescStrings hk_strings = { };

static USBDescEndpoint hk_eps[] = {
    { .bEndpointAddress = USB_DIR_IN | 1, .bmAttributes = USB_ENDPOINT_XFER_INT,
      .wMaxPacketSize = 64, .bInterval = 4 },
};

static const USBDescIface hk_iface = {
    .bInterfaceNumber = 0,
    .bNumEndpoints    = 1,
    .bInterfaceClass  = USB_CLASS_HID,
    .ndesc            = 1,
    .descs = (USBDescOther[]) {
        {
            .data = (uint8_t[]) {
                0x09, HK_DT_HID, 0x11, 0x01, 0x00, 0x01, HK_DT_REPORT,
                sizeof(hk_report_desc), 0x00,
            },
        },
    },
    .eps = hk_eps,
};

static const USBDescDevice hk_device = {
    .bcdUSB             = 0x0200,
    .bMaxPacketSize0    = 64,
    .bNumConfigurations = 1,
    .confs = (USBDescConfig[]) {
        {
            .bNumInterfaces      = 1,
            .bConfigurationValue = 1,
            .bmAttributes        = USB_CFG_ATT_ONE,
            .bMaxPower           = 50,
            .nif                 = 1,
            .ifs                 = &hk_iface,
        },
    },
};


/* ---------------------------------------------------------------------------
 * Small helpers
 */

static uint32_t crc32_le(const uint8_t *d, size_t n)
{
    uint32_t c = 0xffffffff;

    while (n--) {
        c ^= *d++;
        for (int k = 0; k < 8; k++) {
            c = (c >> 1) ^ (0xedb88320 & -(c & 1));
        }
    }
    return ~c;
}

static int64_t now_ns(void)
{
    return qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
}

static void pc_send(USBTeensyState *s, uint8_t kind, const void *a,
                    unsigned na, const void *b, unsigned nb)
{
    uint8_t h[3];

    if (!s->pc_connected) {
        return;
    }
    h[0] = kind;
    stw_le_p(h + 1, na + nb);
    qemu_chr_fe_write_all(&s->cs, h, 3);
    if (na) {
        qemu_chr_fe_write_all(&s->cs, a, na);
    }
    if (nb) {
        qemu_chr_fe_write_all(&s->cs, b, nb);
    }
}

static void G_GNUC_PRINTF(2, 3) note(USBTeensyState *s, const char *fmt, ...)
{
    va_list ap;
    g_autofree char *t = NULL;

    va_start(ap, fmt);
    t = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    if (s->verbose) {
        info_report("usb-teensy: %s", t);
    }
    pc_send(s, 'L', t, strlen(t), NULL, 0);
}

/* the relay's own log (kReqLog reads it destructively) */
static void G_GNUC_PRINTF(2, 3) rlog(USBTeensyState *s, const char *fmt, ...)
{
    va_list ap;
    g_autofree char *t = NULL;
    size_t n;

    va_start(ap, fmt);
    t = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    n = strlen(t);
    for (size_t i = 0; i <= n; i++) {
        char ch = i < n ? t[i] : '\n';
        if (s->log_count == LOG_RING) {
            s->log_head = (s->log_head + 1) % LOG_RING;
            s->log_count--;
        }
        s->log_ring[(s->log_head + s->log_count) % LOG_RING] = ch;
        s->log_count++;
    }
}

/* ---------------------------------------------------------------------------
 * The box's layout (mirror_desc.h parse + relay_proto.h make_map)
 */

static bool mir_parse(USBTeensyState *s)
{
    const uint8_t *cfg = s->cfg_desc;
    unsigned len = s->cfg_len, o;
    int iface = -1;
    uint8_t cls = 0, sub = 0;
    unsigned n_in = 0, n_out = 0;

    s->n_eps = 0;
    for (unsigned c = 0; c < RL_CHANNELS; c++) {
        s->ep_of_in[c] = s->ep_of_out[c] = -1;
    }
    if (len < 9 || len > MIR_MAX_CONFIG || cfg[1] != 2 ||
        lduw_le_p(cfg + 2) != len) {
        return false;
    }
    for (o = 0; o < len;) {
        unsigned dl = cfg[o];
        const uint8_t *d = cfg + o;
        if (dl < 2 || o + dl > len) {
            return false;
        }
        if (d[1] == 4 && dl >= 9) {
            iface = d[2];
            if (iface >= MIR_MAX_IFACES) {
                return false;
            }
            cls = d[5];
            sub = d[6];
        } else if (d[1] == 5 && dl >= 7) {
            uint8_t a = d[2];
            uint16_t m = lduw_le_p(d + 4) & 0x7ff;
            unsigned i = 0;
            if (iface < 0) {
                return false;
            }
            while (i < s->n_eps && s->eps[i].addr != a) {
                i++;
            }
            if (i == s->n_eps) {
                MirEp *e;
                if (s->n_eps == MIR_MAX_EPS || (a & 0x0f) == 0 ||
                    (a & 0x0f) > 7) {
                    return false;
                }
                e = &s->eps[s->n_eps++];
                e->addr = a;
                e->attr = d[3] & 3;
                e->iface = iface;
                e->mps = 0;
                if (cls == 0xff) {
                    e->role = a == BOX_AUDIO_IN ? ROLE_AUDIO_IN :
                              a == BOX_AUDIO_OUT ? ROLE_AUDIO_OUT :
                              (a & 0x80) ? ROLE_PIPE_IN : ROLE_PIPE_OUT;
                } else if (cls == 0x01 && sub == 0x03) {
                    e->role = (a & 0x80) ? ROLE_MIDI_IN : ROLE_MIDI_OUT;
                } else {
                    e->role = ROLE_NONE;
                }
            } else if (s->eps[i].iface != iface) {
                return false;
            }
            if (m > s->eps[i].mps) {
                s->eps[i].mps = m;
            }
        }
        o += dl;
    }
    if (!s->n_eps) {
        return false;
    }
    for (unsigned i = 0; i < s->n_eps; i++) {
        MirEp *e = &s->eps[i];
        if (e->mps > RL_LINK_MPS) {
            return false;
        }
        if (e->role == ROLE_AUDIO_IN || e->role == ROLE_AUDIO_OUT) {
            continue;
        }
        if (e->addr & 0x80) {
            if (n_in == RL_CHANNELS) {
                return false;
            }
            s->ep_of_in[n_in++] = i;
        } else {
            if (n_out == RL_CHANNELS) {
                return false;
            }
            s->ep_of_out[n_out++] = i;
        }
    }
    return true;
}

static int chan_of_box_out(USBTeensyState *s, uint8_t addr)
{
    for (unsigned c = 0; c < RL_CHANNELS; c++) {
        if (s->ep_of_out[c] >= 0 && s->eps[s->ep_of_out[c]].addr == addr) {
            return c;
        }
    }
    return -1;
}

static bool cfg_has_alt(USBTeensyState *s, uint8_t iface, uint8_t alt)
{
    const uint8_t *cfg = s->cfg_desc;
    unsigned len = s->cfg_len;

    for (unsigned o = 0; o + 2 <= len && cfg[o] >= 2 && o + cfg[o] <= len;
         o += cfg[o]) {
        if (cfg[o + 1] == 4 && cfg[o] >= 9 && cfg[o + 2] == iface &&
            cfg[o + 3] == alt) {
            return true;
        }
    }
    return false;
}

/* ---------------------------------------------------------------------------
 * Events
 */

static void ev_post(USBTeensyState *s, uint8_t type, uint8_t a, uint8_t b,
                    const uint8_t *data)
{
    uint8_t *e;

    if (s->mode != MODE_RELAY || !s->link_configured) {
        return;
    }
    if (s->ev_count == EV_RING) {
        s->st.events_drop++;
        return;
    }
    e = s->ev_ring[(s->ev_head + s->ev_count) % EV_RING];
    memset(e, 0, 16);
    e[0] = type;
    e[1] = a;
    e[2] = b;
    stl_le_p(e + 4, (uint32_t)((now_ns() - s->boot_ns) / 1000));
    if (data) {
        memcpy(e + 8, data, 8);
    }
    s->ev_count++;
    s->st.events++;
    usb_wakeup(s->ep_ev, 0);
}

/* ---------------------------------------------------------------------------
 * The computer's side
 */

static bool ctl_abort(USBTeensyState *s)
{
    bool p = s->ctl_pending;

    s->ctl_pending = false;
    if (p) {
        s->st.ctl_aborted++;
        ev_post(s, EV_ABORT, s->ctl_seq, 0, NULL);
    }
    return p;
}

static void pc_eps_close(USBTeensyState *s)
{
    for (unsigned c = 0; c < RL_CHANNELS; c++) {
        s->credits[c] = 0;
        s->pend_head[c] = s->pend_count[c] = 0;
    }
}

static void pc_show(USBTeensyState *s, bool on)
{
    if (!on) {
        timer_del(s->t_connect);
        if (s->shown) {
            pc_eps_close(s);
            s->pc_config = 0;
            s->st.pc_configured = 0;
            ctl_abort(s);
            s->shown = false;
            s->st.shown = 0;
            pc_send(s, 'V', (uint8_t[]) { 0 }, 1, NULL, 0);
        }
        return;
    }
    s->shown = true;
    s->st.shown = 1;
    s->st.pc_hs = 1;
    pc_send(s, 'V', (uint8_t[]) { 1 }, 1, NULL, 0);
}

static void forget_box(USBTeensyState *s)
{
    pc_show(s, false);
    s->have_dev = false;
    s->cfg_len = 0;
    s->n_strs = 0;
    s->n_eps = 0;
}

static void pc_answer(USBTeensyState *s, bool stall, const uint8_t *d,
                      unsigned n, uint16_t wlength)
{
    uint8_t st = stall ? 1 : 0;

    pc_send(s, 'C', &st, 1, d, stall ? 0 : MIN(n, wlength));
}

/* move held computer packets onto the link as credits allow */
static void chan_prime(USBTeensyState *s, unsigned c)
{
    bool moved = false;

    while (s->credits[c] && s->pend_count[c] &&
           s->ch_count[c] < RL_CHAN_DEPTH) {
        Packet *src = &s->pend[c][s->pend_head[c]];
        Packet *dst = &s->ch_to_pi[c][(s->ch_head[c] + s->ch_count[c]) %
                                      RL_CHAN_DEPTH];
        *dst = *src;
        s->pend_head[c] = (s->pend_head[c] + 1) % PEND_MAX;
        s->pend_count[c]--;
        s->ch_count[c]++;
        s->credits[c]--;
        moved = true;
    }
    if (moved) {
        usb_wakeup(s->ep_chan[c], 0);
    }
}

static bool pc_desc(USBTeensyState *s, uint16_t wvalue, const uint8_t **p,
                    unsigned *n)
{
    static const uint8_t lang[4] = { 4, 3, 0x09, 0x04 };
    uint8_t i = wvalue & 0xff;

    switch (wvalue >> 8) {
    case 1:
        if (i) {
            return false;
        }
        *p = s->dev_desc;
        *n = 18;
        return true;
    case 2:
        if (i) {
            return false;
        }
        *p = s->cfg_desc;
        *n = s->cfg_len;
        return true;
    case 3:
        for (unsigned k = 0; k < s->n_strs; k++) {
            if (s->strs[k].index == i) {
                *p = s->strs[k].data;
                *n = s->strs[k].len;
                return true;
            }
        }
        if (i == 0) {
            *p = lang;
            *n = 4;
            return true;
        }
        return false;
    }
    return false;
}

static void pc_setup(USBTeensyState *s, const uint8_t *su, const uint8_t *out,
                     unsigned nout)
{
    uint8_t type = su[0], req = su[1];
    uint16_t wvalue = lduw_le_p(su + 2), windex = lduw_le_p(su + 4),
             wlength = lduw_le_p(su + 6);
    const uint8_t *p;
    unsigned n;
    uint8_t b[2] = { 0, 0 };

    if (!s->shown) {
        pc_answer(s, true, NULL, 0, 0);
        return;
    }
    s->st.setups++;
    ctl_abort(s);                           /* the computer moved on */
    if (type & 0x60) {                      /* class or vendor: to the Pi */
        if (wlength > sizeof(s->ctl_data)) {
            pc_answer(s, true, NULL, 0, 0);
            return;
        }
        s->ctl_seq++;
        s->ctl_pending = true;
        memcpy(s->ctl_setup, su, 8);
        s->ctl_data_len = 0;
        if (!(type & 0x80) && wlength) {
            s->ctl_data_len = MIN(nout, wlength);
            memcpy(s->ctl_data, out, s->ctl_data_len);
        }
        s->st.ctl_forwarded++;
        ev_post(s, EV_SETUP, s->ctl_seq, 0, su);
        return;
    }
    switch ((uint16_t)(req << 8 | type)) {
    case 0x0500:                            /* SET_ADDRESS */
    case 0x0100: case 0x0300:               /* device features */
        pc_answer(s, false, NULL, 0, 0);
        return;
    case 0x0680: case 0x0681:
        if (!pc_desc(s, wvalue, &p, &n)) {
            break;
        }
        pc_answer(s, false, p, n, wlength);
        return;
    case 0x0900:
        pc_eps_close(s);
        s->pc_config = wvalue;
        memset(s->pc_alt, 0, sizeof(s->pc_alt));
        s->st.pc_configured = s->pc_config ? 1 : 0;
        pc_answer(s, false, NULL, 0, 0);
        ev_post(s, EV_CONFIG, s->pc_config, 0, NULL);
        return;
    case 0x0880:
        b[0] = s->pc_config;
        pc_answer(s, false, b, 1, wlength);
        return;
    case 0x0080:
        b[0] = (s->cfg_len > 7 && (s->cfg_desc[7] & 0x40)) ? 1 : 0;
        pc_answer(s, false, b, 2, wlength);
        return;
    case 0x0081: case 0x0082:
        pc_answer(s, false, b, 2, wlength);
        return;
    case 0x0102: case 0x0302:
        pc_answer(s, false, NULL, 0, 0);
        return;
    case 0x0b01:
        if (windex >= MIR_MAX_IFACES || !cfg_has_alt(s, windex, wvalue)) {
            break;
        }
        s->pc_alt[windex] = wvalue;
        pc_answer(s, false, NULL, 0, 0);
        ev_post(s, EV_ALT, windex, wvalue, NULL);
        return;
    case 0x0a81:
        if (windex >= MIR_MAX_IFACES) {
            break;
        }
        b[0] = s->pc_alt[windex];
        pc_answer(s, false, b, 1, wlength);
        return;
    }
    pc_answer(s, true, NULL, 0, 0);
}

static void pc_bus_reset(USBTeensyState *s)
{
    if (!s->shown) {
        return;
    }
    pc_eps_close(s);
    s->pc_config = 0;
    s->st.pc_configured = 0;
    s->st.pc_resets++;
    ctl_abort(s);
    ev_post(s, EV_BUS, BUS_RESET, 1, NULL);
}

static void pc_packet(USBTeensyState *s, uint8_t ep, const uint8_t *d,
                      unsigned n)
{
    int c;
    Packet *pk;

    if (!s->shown || !s->pc_config || n > RL_LINK_MPS) {
        return;
    }
    if (ep == BOX_AUDIO_OUT) {
        if (s->aq_pi_count == AQ) {
            s->aq_pi_head = (s->aq_pi_head + 1) % AQ;
            s->aq_pi_count--;
            s->st.audio_to_pi_drop++;
        }
        pk = &s->aq_pi[(s->aq_pi_head + s->aq_pi_count) % AQ];
        pk->len = n;
        memcpy(pk->d, d, n);
        s->aq_pi_count++;
        if (s->aq_pi_count > s->st.q_to_pi_max) {
            s->st.q_to_pi_max = s->aq_pi_count;
        }
        usb_wakeup(s->ep_audio, 0);
        return;
    }
    c = chan_of_box_out(s, ep);
    if (c < 0) {
        return;
    }
    if (!s->credits[c]) {
        s->st.chan_nocredit[c]++;           /* the computer is NAKed */
    }
    if (s->pend_count[c] == PEND_MAX) {
        return;                             /* the computer would wait */
    }
    pk = &s->pend[c][(s->pend_head[c] + s->pend_count[c]) % PEND_MAX];
    pk->len = n;
    memcpy(pk->d, d, n);
    s->pend_count[c]++;
    chan_prime(s, c);
}

/* ---------------------------------------------------------------------------
 * Modes
 */

static const USBDesc *hk_desc(USBTeensyState *s);

static void relay_boot_state(USBTeensyState *s)
{
    s->link_configured = false;
    s->ev_head = s->ev_count = 0;
    s->log_head = s->log_count = 0;
    memset(&s->st, 0, sizeof(s->st));
    s->boot_ns = now_ns();
    forget_box(s);
    s->ctl_pending = false;
    s->aq_pi_head = s->aq_pi_count = 0;
    for (unsigned c = 0; c < RL_CHANNELS; c++) {
        s->ch_head[c] = s->ch_count[c] = 0;
    }
    pc_eps_close(s);
    rlog(s, "mirror relay up, protocol %u.%02u, build %s", RL_VERSION >> 8,
         RL_VERSION & 0xff, s->cur_build);
}

static void teensy_become(USBTeensyState *s, int mode)
{
    static const char *const name[] = { "gone", "relay", "HalfKay" };
    Error *err = NULL;

    timer_del(s->t_mode);
    timer_del(s->t_connect);
    s->quiet = false;
    s->phase = PH_NONE;
    if (s->dev.attached) {
        usb_device_detach(&s->dev);
    }
    if (s->mode == MODE_RELAY) {
        forget_box(s);
    }
    s->mode = mode;
    s->held = NULL;
    switch (mode) {
    case MODE_RELAY:
        s->dev.usb_desc = &desc_link;
        relay_boot_state(s);
        note(s, "relay up (1209:0002, protocol 1.02, build %s)", s->cur_build);
        break;
    case MODE_HALFKAY:
        s->dev.usb_desc = hk_desc(s);
        if (!s->image) {
            s->image = g_malloc(HK_CODE_SIZE);
            memset(s->image, 0xff, HK_CODE_SIZE);
        }
        s->busy_until = 0;
        s->blocks_written = s->refusals = 0;
        s->dropped_once = false;
        note(s, "HalfKay up (16C0:0478)");
        break;
    default:
        note(s, "%s", name[mode]);
        return;
    }
    usb_desc_init(&s->dev);
    usb_device_attach(&s->dev, &err);
    if (err) {
        error_report_err(err);          /* stays unplugged */
    }
}

/* MODE_GONE now, then `mode` after ms */
static void teensy_leave_then(USBTeensyState *s, int mode, uint32_t ms)
{
    teensy_become(s, MODE_GONE);
    s->next_mode = mode;
    s->phase = PH_GONE;
    timer_mod(s->t_mode, now_ns() + (int64_t)ms * SCALE_MS);
}

static void teensy_mode_timer(void *opaque)
{
    USBTeensyState *s = opaque;
    uint32_t back = s->next_mode == MODE_HALFKAY ? s->halfkay_ms : s->boot_ms;

    switch (s->phase) {
    case PH_PRE:
        if (s->leave_quiet_ms) {
            s->phase = PH_QUIET;
            s->quiet = true;
            /* the host's queued IN transfers are retried and fail now */
            usb_wakeup(s->ep_ev, 0);
            usb_wakeup(s->ep_audio, 0);
            for (unsigned c = 0; c < RL_CHANNELS; c++) {
                usb_wakeup(s->ep_chan[c], 0);
            }
            note(s, "leaving: every transfer fails for %u ms (leave-quiet-ms)",
                 s->leave_quiet_ms);
            timer_mod(s->t_mode, now_ns() +
                      (int64_t)s->leave_quiet_ms * SCALE_MS);
            return;
        }
        /* fall through */
    case PH_QUIET:
        teensy_leave_then(s, s->next_mode, back);
        return;
    case PH_GONE:
        s->phase = PH_NONE;
        teensy_become(s, s->next_mode);
        return;
    }
}

/* leave toward `mode` after_ms from now (the device answers until then) */
static void teensy_leave(USBTeensyState *s, int mode, uint32_t after_ms)
{
    s->next_mode = mode;
    s->phase = PH_PRE;
    timer_mod(s->t_mode, now_ns() + (int64_t)after_ms * SCALE_MS);
}

static void connect_timer(void *opaque)
{
    USBTeensyState *s = opaque;

    if (s->mode == MODE_RELAY && s->have_dev && s->cfg_len) {
        pc_show(s, true);
        ev_post(s, EV_HELLO, RL_VERSION >> 8, RL_VERSION & 0xff, NULL);
    }
}

/* ---------------------------------------------------------------------------
 * Link EP0
 */

static int relay_vendor_out(USBTeensyState *s, int req, int value, int index,
                            int length, const uint8_t *data)
{
    unsigned k;

    switch (req) {
    case REQ_SET_DEVICE:
        if (length != 18 || data[0] != 18 || data[1] != 1 || data[7] != 64) {
            return -1;
        }
        pc_show(s, false);
        memcpy(s->dev_desc, data, 18);
        s->have_dev = true;
        return 0;
    case REQ_SET_CONFIG:
        if (length < 9 || length > MIR_MAX_CONFIG) {
            return -1;
        }
        pc_show(s, false);
        memcpy(s->cfg_desc, data, length);
        s->cfg_len = length;
        s->n_strs = 0;
        return 0;
    case REQ_SET_STRING:
        if (length < 2 || length > 254 || data[1] != 3) {
            return -1;
        }
        for (k = 0; k < s->n_strs && s->strs[k].index != (uint8_t)value; k++) {
        }
        if (k == s->n_strs) {
            if (s->n_strs == MIR_MAX_STRINGS) {
                return -1;
            }
            s->n_strs++;
        }
        s->strs[k].index = value;
        s->strs[k].len = length;
        memcpy(s->strs[k].data, data, length);
        return 0;
    case REQ_SET_EXTRA:
        return (value == 15 || value == 0xee) ? 0 : -1;   /* kept by nobody */
    case REQ_CTL_ANSWER:
        if (!s->ctl_pending || (uint8_t)value != s->ctl_seq) {
            s->st.ctl_late++;               /* for nobody */
            return 0;
        }
        s->ctl_pending = false;
        s->st.ctl_answered++;
        if (index & ANS_STALL) {
            s->st.ctl_stalled++;
            pc_answer(s, true, NULL, 0, 0);
        } else if (s->ctl_setup[0] & 0x80) {
            pc_answer(s, false, data, length, lduw_le_p(s->ctl_setup + 6));
        } else {
            pc_answer(s, false, NULL, 0, 0);
        }
        return 0;
    case REQ_CONNECT:
        if (value == 0) {
            pc_show(s, false);
            return 0;
        }
        if (!s->have_dev || !s->cfg_len || !mir_parse(s)) {
            rlog(s, "connect refused: dev %d cfg %u map no", s->have_dev,
                 s->cfg_len);
            note(s, "connect refused (dev %d, cfg %u B)", s->have_dev,
                 s->cfg_len);
            return -1;
        }
        pc_show(s, false);
        rlog(s, "connect: %04x:%04x, %u B cfg, %u eps", lduw_le_p(s->dev_desc + 8),
             lduw_le_p(s->dev_desc + 10), s->cfg_len, s->n_eps);
        note(s, "connect: box %04x:%04x, %u B cfg, %u eps",
             lduw_le_p(s->dev_desc + 8), lduw_le_p(s->dev_desc + 10),
             s->cfg_len, s->n_eps);
        timer_mod(s->t_connect, now_ns() + (int64_t)s->connect_ms * SCALE_MS);
        return 0;
    case REQ_CREDIT:
        for (unsigned c = 0; c < RL_CHANNELS; c++) {
            if (s->ep_of_out[c] >= 0 &&
                s->eps[s->ep_of_out[c]].addr == (uint8_t)value) {
                unsigned v = s->credits[c] + index;
                s->credits[c] = MIN(v, RL_MAX_CREDITS);
                chan_prime(s, c);
                return 0;
            }
        }
        return -1;
    case REQ_STATS_RESET: {
        RelayStats keep = s->st;
        memset(&s->st, 0, sizeof(s->st));
        s->st.link_hs = keep.link_hs;
        s->st.link_configured = keep.link_configured;
        s->st.link_resets = keep.link_resets;
        s->st.pc_hs = keep.pc_hs;
        s->st.pc_configured = keep.pc_configured;
        s->st.pc_resets = keep.pc_resets;
        s->st.pc_suspends = keep.pc_suspends;
        s->st.shown = keep.shown;
        return 0;
    }
    case REQ_BOOTLOADER:
        if (value != 0x4c42) {
            return -1;
        }
        if (s->bl_ignore) {
            note(s, "bootloader request taken and ignored (bl-ignore)");
            return 0;
        }
        note(s, "bootloader request: leaving in %u ms", s->enter_ms);
        teensy_leave(s, MODE_HALFKAY, s->enter_ms);
        return 0;
    case REQ_HALT:
        if ((value & 0x0f) == 0) {
            return -1;
        }
        pc_send(s, 'H', (uint8_t[]) { value, index != 0 }, 2, NULL, 0);
        return 0;
    }
    return -1;
}

static int relay_vendor_in(USBTeensyState *s, int req, int value, int index,
                           int length, uint8_t *data)
{
    unsigned n = 0;

    switch (req) {
    case REQ_CTL_DATA:
        if ((uint8_t)value != s->ctl_seq || !s->ctl_pending) {
            return -1;
        }
        n = MIN(s->ctl_data_len, (unsigned)length);
        memcpy(data, s->ctl_data, n);
        return n;
    case REQ_STATS: {
        RelayStats st = s->st;
        st.magic = STATS_MAGIC;
        st.version = RL_VERSION;
        st.uptime_ms = (now_ns() - s->boot_ns) / SCALE_MS;
        n = MIN(sizeof(st), (size_t)length);
        memcpy(data, &st, n);
        return n;
    }
    case REQ_LOG:
        while (s->log_count && n < (unsigned)length && n < 1024) {
            data[n++] = s->log_ring[s->log_head];
            s->log_head = (s->log_head + 1) % LOG_RING;
            s->log_count--;
        }
        return n;
    }
    return -1;
}

static void relay_handle_control(USBTeensyState *s, USBPacket *p, int request,
                                 int value, int index, int length,
                                 uint8_t *data)
{
    int ret;

    ret = usb_desc_handle_control(&s->dev, p, request, value, index, length,
                                  data);
    if (ret >= 0) {
        if (request == (DeviceOutRequest | USB_REQ_SET_CONFIGURATION)) {
            s->link_configured = value == 1;
            s->st.link_configured = s->link_configured;
            s->st.link_hs = 1;
            if (s->link_configured) {
                ev_post(s, EV_HELLO, RL_VERSION >> 8, RL_VERSION & 0xff, NULL);
            } else {
                forget_box(s);
            }
        }
        return;
    }
    switch (request >> 8) {
    case USB_DIR_OUT | USB_TYPE_VENDOR | USB_RECIP_DEVICE:
        if (relay_vendor_out(s, request & 0xff, value, index, length,
                             data) == 0) {
            return;
        }
        break;
    case USB_DIR_IN | USB_TYPE_VENDOR | USB_RECIP_DEVICE:
        ret = relay_vendor_in(s, request & 0xff, value, index, length, data);
        if (ret >= 0) {
            p->actual_length = ret;
            return;
        }
        break;
    }
    p->status = USB_RET_STALL;
}

/* ---------------------------------------------------------------------------
 * HalfKay EP0
 */

static void hk_handle_control(USBTeensyState *s, USBPacket *p, int request,
                              int value, int index, int length, uint8_t *data)
{
    int ret;
    uint32_t off;
    int64_t now = now_ns();

    ret = usb_desc_handle_control(&s->dev, p, request, value, index, length,
                                  data);
    if (ret >= 0) {
        return;
    }
    if (request == (InterfaceRequest | USB_REQ_GET_DESCRIPTOR) &&
        (value >> 8) == HK_DT_REPORT) {
        ret = MIN(sizeof(hk_report_desc), (size_t)length);
        memcpy(data, hk_report_desc, ret);
        p->actual_length = ret;
        return;
    }
    if (request != (ClassInterfaceOutRequest | 0x09) || value != 0x0200) {
        p->status = USB_RET_STALL;
        return;
    }
    if (length != HK_REPORT) {
        note(s, "HalfKay: report of %d bytes refused", length);
        p->status = USB_RET_STALL;
        return;
    }
    off = data[0] | data[1] << 8 | data[2] << 16;
    if (off == HK_START) {
        note(s, "HalfKay: start after %u blocks (%u refusals)",
             s->blocks_written, s->refusals);
        if (s->bl_stay) {
            note(s, "HalfKay stays (bl-stay)");
            return;
        }
        if (s->flash_build && *s->flash_build) {
            pstrcpy(s->cur_build, sizeof(s->cur_build), s->flash_build);
        } else {
            snprintf(s->cur_build, sizeof(s->cur_build), "stub-%08x",
                     crc32_le(s->image, HK_CODE_SIZE));
        }
        teensy_leave(s, MODE_RELAY, 5);
        return;
    }
    if (off % HK_BLOCK || off >= HK_CODE_SIZE) {
        note(s, "HalfKay: bad block offset 0x%06x refused", off);
        p->status = USB_RET_STALL;
        return;
    }
    if (now < s->busy_until) {              /* erasing */
        s->refusals++;
        p->status = USB_RET_STALL;
        return;
    }
    if ((int32_t)(off / HK_BLOCK) == s->drop_block && !s->dropped_once) {
        s->dropped_once = true;
        note(s, "HalfKay: block %u never answered (drop-block)",
             off / HK_BLOCK);
        s->held = p;
        p->status = USB_RET_ASYNC;
        return;
    }
    if (off == 0) {
        memset(s->image, 0xff, HK_CODE_SIZE);
        s->busy_until = now + (int64_t)s->erase_ms * SCALE_MS;
    }
    memcpy(s->image + off, data + 64, HK_BLOCK);
    s->blocks_written++;
}

/* ---------------------------------------------------------------------------
 * USB
 */

static void teensy_handle_reset(USBDevice *dev)
{
    USBTeensyState *s = USB_TEENSY(dev);

    if (s->mode == MODE_RELAY) {             /* the Pi went away or restarts */
        s->st.link_resets++;
        s->link_configured = false;
        s->st.link_configured = 0;
        forget_box(s);
        s->ev_head = s->ev_count = 0;
    }
}

static void teensy_handle_control(USBDevice *dev, USBPacket *p, int request,
                                  int value, int index, int length,
                                  uint8_t *data)
{
    USBTeensyState *s = USB_TEENSY(dev);

    if (s->quiet) {
        p->status = USB_RET_IOERROR;
        return;
    }
    if (s->mode == MODE_HALFKAY) {
        hk_handle_control(s, p, request, value, index, length, data);
    } else {
        relay_handle_control(s, p, request, value, index, length, data);
    }
}

static void relay_handle_data(USBTeensyState *s, USBPacket *p)
{
    unsigned nr = p->ep->nr;
    uint8_t buf[RL_LINK_MPS];
    size_t n;

    if (p->pid == USB_TOKEN_IN) {
        if (nr == RL_EV_EP) {
            if (!s->ev_count || p->iov.size < 16) {
                p->status = USB_RET_NAK;
                return;
            }
            usb_packet_copy(p, s->ev_ring[s->ev_head], 16);
            s->ev_head = (s->ev_head + 1) % EV_RING;
            s->ev_count--;
            return;
        }
        if (nr == RL_AUDIO_TO_PI_EP) {
            Packet *pk;
            if (!s->aq_pi_count) {
                p->status = USB_RET_NAK;
                return;
            }
            pk = &s->aq_pi[s->aq_pi_head];
            usb_packet_copy(p, pk->d, MIN(pk->len, p->iov.size));
            s->aq_pi_head = (s->aq_pi_head + 1) % AQ;
            s->aq_pi_count--;
            s->st.audio_to_pi++;
            return;
        }
        if (nr >= RL_CHAN_FIRST_EP && nr < RL_CHAN_FIRST_EP + RL_CHANNELS) {
            unsigned c = nr - RL_CHAN_FIRST_EP;
            Packet *pk;
            if (!s->ch_count[c]) {
                p->status = USB_RET_NAK;
                return;
            }
            pk = &s->ch_to_pi[c][s->ch_head[c]];
            usb_packet_copy(p, pk->d, MIN(pk->len, p->iov.size));
            s->ch_head[c] = (s->ch_head[c] + 1) % RL_CHAN_DEPTH;
            s->ch_count[c]--;
            s->st.chan_to_pi[c]++;
            chan_prime(s, c);
            return;
        }
        p->status = USB_RET_STALL;
        return;
    }
    if (p->pid != USB_TOKEN_OUT) {
        p->status = USB_RET_STALL;
        return;
    }
    n = MIN(p->iov.size, sizeof(buf));
    usb_packet_copy(p, buf, n);
    if (p->iov.size > n) {
        usb_packet_skip(p, p->iov.size - n);
    }
    if (nr == RL_AUDIO_FROM_PI_EP) {
        if (s->shown && s->pc_config && s->pc_connected) {
            uint8_t ep = BOX_AUDIO_IN;
            pc_send(s, 'I', &ep, 1, buf, n);
            s->st.audio_to_pc++;
        } else {
            s->st.audio_to_pc_drop++;
        }
        return;
    }
    if (nr >= RL_CHAN_FIRST_EP && nr < RL_CHAN_FIRST_EP + RL_CHANNELS) {
        unsigned c = nr - RL_CHAN_FIRST_EP;
        int i = s->ep_of_in[c];
        uint8_t ep = i >= 0 ? s->eps[i].addr : 0;
        if (!ep) {
            return;                         /* no box endpoint on it */
        }
        if (s->shown && s->pc_config && s->pc_connected) {
            pc_send(s, 'I', &ep, 1, buf, n);
            s->st.chan_to_pc[c]++;
        }
        /* taken by the computer, or drained with nobody there */
        ev_post(s, EV_IN_TAKEN, ep, 1, NULL);
        return;
    }
    p->status = USB_RET_STALL;
}

static void teensy_handle_data(USBDevice *dev, USBPacket *p)
{
    USBTeensyState *s = USB_TEENSY(dev);

    if (s->quiet) {
        p->status = USB_RET_IOERROR;
        return;
    }
    if (s->mode == MODE_RELAY) {
        relay_handle_data(s, p);
        return;
    }
    p->status = USB_RET_NAK;                /* HalfKay's HID IN: nothing */
}

static void teensy_cancel_packet(USBDevice *dev, USBPacket *p)
{
    USBTeensyState *s = USB_TEENSY(dev);

    if (s->held == p) {
        s->held = NULL;
    }
}

/* ---------------------------------------------------------------------------
 * The computer's chardev
 */

static void pc_frame(USBTeensyState *s, uint8_t kind, const uint8_t *d,
                     unsigned n)
{
    switch (kind) {
    case 'S':
        if (n >= 8) {
            pc_setup(s, d, d + 8, n - 8);
        }
        break;
    case 'R':
        pc_bus_reset(s);
        break;
    case 'U':
        if (s->shown) {
            s->st.pc_suspends++;
            ev_post(s, EV_BUS, BUS_SUSPEND, 1, NULL);
        }
        break;
    case 'O':
        if (n >= 1) {
            pc_packet(s, d[0], d + 1, n - 1);
        }
        break;
    case 'B':
        g_free(s->flash_build);
        s->flash_build = g_strndup((const char *)d, MIN(n, 23));
        note(s, "next flash comes back as build %s", s->flash_build);
        break;
    case 'P':
        if (n < 1) {
            break;
        }
        if (d[0] == 0) {
            teensy_become(s, MODE_GONE);
        } else if (d[0] == 1) {             /* power: the program last written */
            teensy_become(s, MODE_RELAY);
        } else if (d[0] == 2) {
            teensy_leave_then(s, MODE_HALFKAY, s->halfkay_ms);
        }
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "usb-teensy: computer: frame 0x%02x\n",
                      kind);
    }
}

static int pc_can_read(void *opaque)
{
    return 4096;
}

static void pc_read(void *opaque, const uint8_t *buf, int size)
{
    USBTeensyState *s = opaque;

    while (size > 0) {
        unsigned need = s->pc_have < 3 ? 3 : 3 + lduw_le_p(s->pc_msg + 1);
        unsigned k = MIN((unsigned)size, need - s->pc_have);
        memcpy(s->pc_msg + s->pc_have, buf, k);
        s->pc_have += k;
        buf += k;
        size -= k;
        if (s->pc_have < 3) {
            continue;
        }
        need = 3 + lduw_le_p(s->pc_msg + 1);
        if (s->pc_have == need) {
            pc_frame(s, s->pc_msg[0], s->pc_msg + 3, need - 3);
            s->pc_have = 0;
        }
    }
}

static void pc_event(void *opaque, QEMUChrEvent event)
{
    USBTeensyState *s = opaque;

    switch (event) {
    case CHR_EVENT_OPENED:
        s->pc_connected = true;
        s->pc_have = 0;
        if (s->shown) {                     /* tell a late computer */
            pc_send(s, 'V', (uint8_t[]) { 1 }, 1, NULL, 0);
        }
        break;
    case CHR_EVENT_CLOSED:
        s->pc_connected = false;
        if (s->shown && s->pc_config) {     /* the computer unplugged */
            pc_bus_reset(s);
        }
        break;
    default:
        break;
    }
}

/* ---------------------------------------------------------------------------
 * Device
 */

static const USBDesc *hk_desc(USBTeensyState *s)
{
    s->hk_desc.id.idVendor = HK_VID;
    s->hk_desc.id.idProduct = HK_PID;
    s->hk_desc.id.bcdDevice = s->halfkay_bcd;
    s->hk_desc.high = &hk_device;
    s->hk_desc.str = hk_strings;
    return &s->hk_desc;
}

static void teensy_realize(USBDevice *dev, Error **errp)
{
    USBTeensyState *s = USB_TEENSY(dev);
    Error *local_err = NULL;

    dev->usb_desc = &desc_link;
    usb_desc_init(dev);
    dev->auto_attach = 0;
    usb_check_attach(dev, &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
        return;
    }
    s->t_mode = timer_new_ns(QEMU_CLOCK_VIRTUAL, teensy_mode_timer, s);
    s->t_connect = timer_new_ns(QEMU_CLOCK_VIRTUAL, connect_timer, s);
    s->pc_msg = g_malloc(PC_MSG_MAX);
    s->ep_ev = usb_ep_get(dev, USB_TOKEN_IN, RL_EV_EP);
    s->ep_audio = usb_ep_get(dev, USB_TOKEN_IN, RL_AUDIO_TO_PI_EP);
    for (unsigned c = 0; c < RL_CHANNELS; c++) {
        s->ep_chan[c] = usb_ep_get(dev, USB_TOKEN_IN, RL_CHAN_FIRST_EP + c);
    }
    pstrcpy(s->cur_build, sizeof(s->cur_build), s->build);
    s->mode = MODE_GONE;
    if (qemu_chr_fe_backend_connected(&s->cs)) {
        qemu_chr_fe_set_handlers(&s->cs, pc_can_read, pc_read, pc_event,
                                 NULL, s, NULL, true);
    }
    teensy_become(s, s->start_halfkay ? MODE_HALFKAY : MODE_RELAY);
}

static void teensy_unrealize(USBDevice *dev)
{
    USBTeensyState *s = USB_TEENSY(dev);

    qemu_chr_fe_deinit(&s->cs, false);
    timer_free(s->t_mode);
    timer_free(s->t_connect);
    g_free(s->pc_msg);
    g_free(s->image);
}

static const VMStateDescription vmstate_usb_teensy = {
    .name = TYPE_USB_TEENSY,
    .unmigratable = 1,
};

static const Property teensy_properties[] = {
    DEFINE_PROP_CHR("chardev", USBTeensyState, cs),
    DEFINE_PROP_STRING("build", USBTeensyState, build),
    DEFINE_PROP_STRING("flash-build", USBTeensyState, flash_build),
    DEFINE_PROP_UINT32("enter-ms", USBTeensyState, enter_ms, 50),
    DEFINE_PROP_UINT32("halfkay-ms", USBTeensyState, halfkay_ms, 300),
    DEFINE_PROP_UINT32("erase-ms", USBTeensyState, erase_ms, 400),
    DEFINE_PROP_UINT32("boot-ms", USBTeensyState, boot_ms, 500),
    DEFINE_PROP_UINT32("connect-ms", USBTeensyState, connect_ms, 300),
    DEFINE_PROP_UINT32("leave-quiet-ms", USBTeensyState, leave_quiet_ms, 0),
    DEFINE_PROP_UINT16("halfkay-bcd", USBTeensyState, halfkay_bcd, 0x0280),
    DEFINE_PROP_INT32("drop-block", USBTeensyState, drop_block, -1),
    DEFINE_PROP_BOOL("bl-ignore", USBTeensyState, bl_ignore, false),
    DEFINE_PROP_BOOL("bl-stay", USBTeensyState, bl_stay, false),
    DEFINE_PROP_BOOL("start-halfkay", USBTeensyState, start_halfkay, false),
    DEFINE_PROP_BOOL("verbose", USBTeensyState, verbose, true),
};

static void teensy_instance_init(Object *obj)
{
    USBTeensyState *s = USB_TEENSY(obj);

    s->build = g_strdup("stub0001");
}

static void teensy_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    USBDeviceClass *uc = USB_DEVICE_CLASS(klass);

    uc->product_desc   = "Teensy mirror relay stand-in";
    uc->usb_desc       = &desc_link;
    uc->realize        = teensy_realize;
    uc->unrealize      = teensy_unrealize;
    uc->handle_attach  = usb_desc_attach;
    uc->handle_reset   = teensy_handle_reset;
    uc->handle_control = teensy_handle_control;
    uc->handle_data    = teensy_handle_data;
    uc->cancel_packet  = teensy_cancel_packet;
    dc->vmsd = &vmstate_usb_teensy;
    device_class_set_props(dc, teensy_properties);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo teensy_info = {
    .name          = TYPE_USB_TEENSY,
    .parent        = TYPE_USB_DEVICE,
    .instance_size = sizeof(USBTeensyState),
    .instance_init = teensy_instance_init,
    .class_init    = teensy_class_init,
};

static void teensy_register_types(void)
{
    type_register_static(&teensy_info);
}

type_init(teensy_register_types)

/*
 * DWC2 device mode (buffer DMA) with a built-in USB host
 *
 * When the dwc2-usb device has a gadget-chardev, the guest can force the
 * core into device mode and run a USB gadget on it, as Circle's
 * CDWUSBGadget does on a Raspberry Pi 4. There is no real USB host on the
 * other side. A small host lives here instead:
 *
 *   - it resets the bus and reports a high-speed enumeration,
 *   - it enumerates the gadget on EP0 (device and configuration
 *     descriptors, SET_ADDRESS, SET_CONFIGURATION, SET_INTERFACE),
 *   - it finds the first USB MIDI streaming interface with bulk endpoints
 *     and carries it to the chardev as a raw MIDI byte stream: bytes read
 *     from the chardev go to the gadget's MIDI OUT endpoint as USB MIDI
 *     event packets, and the gadget's MIDI IN packets come back as bytes.
 *
 * Every other endpoint (audio, interrupt) is enumerated but never polled:
 * the host sends no tokens to it. Only buffer DMA mode is modelled, which
 * is what Circle uses; there is no slave (FIFO) mode and no descriptor DMA.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "hw/usb/dwc2-regs.h"
#include "hw/usb/hcd-dwc2.h"
#include "hw/usb/dwc2-gadget.h"
#include "system/dma.h"

/* device registers, offsets from 0x800 */
#define D_DCFG          0x000
#define D_DCTL          0x004
#define D_DSTS          0x008
#define D_DIEPMSK       0x010
#define D_DOEPMSK       0x014
#define D_DAINT         0x018
#define D_DAINTMSK      0x01c
#define D_DIEPCTL(n)    (0x100 + (n) * 0x20)
#define D_DIEPINT(n)    (0x108 + (n) * 0x20)
#define D_DIEPTSIZ(n)   (0x110 + (n) * 0x20)
#define D_DIEPDMA(n)    (0x114 + (n) * 0x20)
#define D_DTXFSTS(n)    (0x118 + (n) * 0x20)
#define D_DOEPCTL(n)    (0x300 + (n) * 0x20)
#define D_DOEPINT(n)    (0x308 + (n) * 0x20)
#define D_DOEPTSIZ(n)   (0x310 + (n) * 0x20)
#define D_DOEPDMA(n)    (0x314 + (n) * 0x20)


#define EPCTL_EPENA     (1u << 31)
#define EPCTL_EPDIS     (1u << 30)
#define EPCTL_SD1PID    (1u << 29)
#define EPCTL_SD0PID    (1u << 28)
#define EPCTL_SNAK      (1u << 27)
#define EPCTL_CNAK      (1u << 26)
#define EPCTL_STALL     (1u << 21)
#define EPCTL_NAKSTS    (1u << 17)
#define EPCTL_ACTIONS   (EPCTL_EPDIS | EPCTL_SD1PID | EPCTL_SD0PID | \
                         EPCTL_SNAK | EPCTL_CNAK)

#define EPINT_XFERCOMPL (1u << 0)
#define EPINT_EPDISBLD  (1u << 1)
#define EPINT_SETUP     (1u << 3)

#define TSIZ_XFER_MASK  0x7ffff
#define TSIZ_PKT_SHIFT  19
#define TSIZ_PKT_MASK   (0x3ffu << TSIZ_PKT_SHIFT)
#define TSIZ_SUP_MASK   (3u << 29)

#define POLL_NS         (125 * SCALE_US)    /* one microframe */

enum {
    DWC2_LINK_OFF,          /* not in device mode, or soft-disconnected */
    DWC2_LINK_WAIT,         /* waiting for the driver's interrupts */
    DWC2_LINK_RESET,        /* USB reset raised, waiting for the ack */
    DWC2_LINK_ENUM,         /* enumeration done raised, waiting for the ack */
    DWC2_LINK_UP,           /* the host talks to EP0 */
};

enum {
    PH_IDLE,
    PH_SETUP,
    PH_DATA_IN,
    PH_DATA_OUT,
    PH_STATUS_IN,
    PH_STATUS_OUT,
    PH_DONE,
};

enum {
    ST_DEV64,
    ST_ADDRESS,
    ST_DEV18,
    ST_CFG9,
    ST_CFG,
    ST_SETCFG,
    ST_SETIF,
    ST_RUN,
};

#define R(off)  (g->dreg[(off) >> 2])

static int64_t now_ns(void)
{
    return qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
}

bool dwc2_gadget_enabled(DWC2State *s)
{
    return qemu_chr_fe_backend_connected(&s->gadget.chr);
}

/* ---------------------------------------------------------------------------
 * interrupts
 */

static void gadget_update_irq(DWC2State *s)
{
    DWC2Gadget *g = &s->gadget;
    uint32_t daint = 0;
    int n;

    for (n = 0; n < DWC2_GADGET_EPS; n++) {
        if (R(D_DIEPINT(n)) & R(D_DIEPMSK)) {
            daint |= 1u << n;
        }
        if (R(D_DOEPINT(n)) & R(D_DOEPMSK)) {
            daint |= 1u << (n + 16);
        }
    }
    R(D_DAINT) = daint;
    daint &= R(D_DAINTMSK);

    s->gintsts &= ~(GINTSTS_IEPINT | GINTSTS_OEPINT);
    if (daint & 0xffff) {
        s->gintsts |= GINTSTS_IEPINT;
    }
    if (daint >> 16) {
        s->gintsts |= GINTSTS_OEPINT;
    }
    dwc2_irq_update(s);
}

/* ---------------------------------------------------------------------------
 * DMA
 */

static bool gadget_dma_read(DWC2State *s, uint32_t addr, void *buf,
                            unsigned len)
{
    if (!len) {
        return true;
    }
    if (dma_memory_read(&s->dma_as, addr, buf, len,
                        MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR, "dwc2-gadget: DMA read 0x%x failed\n",
                      addr);
        return false;
    }
    return true;
}

static bool gadget_dma_write(DWC2State *s, uint32_t addr, const void *buf,
                             unsigned len)
{
    if (!len) {
        return true;
    }
    if (dma_memory_write(&s->dma_as, addr, buf, len,
                         MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR, "dwc2-gadget: DMA write 0x%x failed\n",
                      addr);
        return false;
    }
    return true;
}

static unsigned xfer_mask(int ep)
{
    return ep ? TSIZ_XFER_MASK : 0x7f;
}

static unsigned ep0_mps(DWC2Gadget *g)
{
    static const unsigned mps[4] = { 64, 32, 16, 8 };
    return mps[R(D_DIEPCTL(0)) & 3];
}

/*
 * The host sends OUT data (or a SETUP) to an armed OUT EP: up to the
 * armed size, in one go. Returns the number of bytes taken.
 */
static unsigned gadget_out(DWC2State *s, int ep, const uint8_t *d,
                           unsigned n, bool setup)
{
    DWC2Gadget *g = &s->gadget;
    uint32_t tsiz = R(D_DOEPTSIZ(ep));
    unsigned room = tsiz & xfer_mask(ep);
    uint32_t dma = R(D_DOEPDMA(ep));

    if (n > room) {
        n = room;
    }
    if (!gadget_dma_write(s, dma, d, n)) {
        return 0;
    }
    tsiz = (tsiz & ~xfer_mask(ep)) | (room - n);
    tsiz &= ~TSIZ_PKT_MASK;             /* the transfer ends here */
    if (setup) {
        tsiz &= ~TSIZ_SUP_MASK;
    }
    R(D_DOEPTSIZ(ep)) = tsiz;
    R(D_DOEPDMA(ep)) = dma + n;
    R(D_DOEPCTL(ep)) &= ~EPCTL_EPENA;
    R(D_DOEPINT(ep)) |= setup ? EPINT_SETUP : EPINT_XFERCOMPL;
    if (setup) {
        /* a SETUP clears the EP0 stall both ways */
        R(D_DIEPCTL(0)) &= ~EPCTL_STALL;
        R(D_DOEPCTL(0)) &= ~EPCTL_STALL;
    }
    gadget_update_irq(s);
    return n;
}

/*
 * The host takes the IN data of an armed IN EP: all of it. Returns the
 * number of bytes, -1 when the DMA failed.
 */
static int gadget_in(DWC2State *s, int ep, uint8_t *d, unsigned max)
{
    DWC2Gadget *g = &s->gadget;
    uint32_t tsiz = R(D_DIEPTSIZ(ep));
    unsigned n = tsiz & xfer_mask(ep);
    uint32_t dma = R(D_DIEPDMA(ep));

    if (n > max) {
        n = max;
    }
    if (!gadget_dma_read(s, dma, d, n)) {
        return -1;
    }
    R(D_DIEPTSIZ(ep)) = tsiz & ~(xfer_mask(ep) | TSIZ_PKT_MASK);
    R(D_DIEPDMA(ep)) = dma + n;
    R(D_DIEPCTL(ep)) &= ~EPCTL_EPENA;
    R(D_DIEPINT(ep)) |= EPINT_XFERCOMPL;
    gadget_update_irq(s);
    return n;
}

/* ---------------------------------------------------------------------------
 * the built-in host: control transfers on EP0
 */

static void ctrl_start(DWC2Gadget *g, uint8_t type, uint8_t req,
                       uint16_t value, uint16_t index, uint16_t len)
{
    g->setup[0] = type;
    g->setup[1] = req;
    stw_le_p(g->setup + 2, value);
    stw_le_p(g->setup + 4, index);
    stw_le_p(g->setup + 6, len);
    g->ctrl_want = len;
    g->ctrl_len = 0;
    g->ctrl_failed = false;
    g->phase = PH_SETUP;
}

/* one step of the control transfer in flight; true when it moved */
static bool ctrl_run(DWC2State *s)
{
    DWC2Gadget *g = &s->gadget;
    bool in = g->setup[0] & 0x80;
    int n;

    switch (g->phase) {
    case PH_SETUP:
        if (!(R(D_DOEPCTL(0)) & EPCTL_EPENA)) {
            return false;
        }
        if (gadget_out(s, 0, g->setup, 8, true) != 8) {
            return false;
        }
        g->setups++;
        g->phase = !g->ctrl_want ? PH_STATUS_IN : in ? PH_DATA_IN
                                                     : PH_DATA_OUT;
        return true;

    case PH_DATA_IN:
        if (R(D_DIEPCTL(0)) & EPCTL_STALL) {
            g->stalls++;
            g->ctrl_failed = true;
            g->phase = PH_DONE;
            return true;
        }
        if (!(R(D_DIEPCTL(0)) & EPCTL_EPENA)) {
            return false;
        }
        n = gadget_in(s, 0, g->ctrl + g->ctrl_len,
                      MIN(g->ctrl_want - g->ctrl_len, ep0_mps(g)));
        if (n < 0) {
            g->ctrl_failed = true;
            g->phase = PH_DONE;
            return true;
        }
        g->ctrl_len += n;
        if (g->ctrl_len >= g->ctrl_want || n < ep0_mps(g)) {
            g->phase = PH_STATUS_OUT;
        }
        return true;

    case PH_DATA_OUT:
        if (R(D_DOEPCTL(0)) & EPCTL_STALL) {
            g->stalls++;
            g->ctrl_failed = true;
            g->phase = PH_DONE;
            return true;
        }
        if (!(R(D_DOEPCTL(0)) & EPCTL_EPENA)) {
            return false;
        }
        g->ctrl_len += gadget_out(s, 0, g->ctrl + g->ctrl_len,
                                  MIN(g->ctrl_want - g->ctrl_len,
                                      ep0_mps(g)), false);
        if (g->ctrl_len >= g->ctrl_want) {
            g->phase = PH_STATUS_IN;
        }
        return true;

    case PH_STATUS_IN:
        if (R(D_DIEPCTL(0)) & EPCTL_STALL) {
            g->stalls++;
            g->ctrl_failed = true;
            g->phase = PH_DONE;
            return true;
        }
        if (!(R(D_DIEPCTL(0)) & EPCTL_EPENA)) {
            return false;
        }
        gadget_in(s, 0, g->ctrl, 0);
        g->phase = PH_DONE;
        return true;

    case PH_STATUS_OUT:
        if (!(R(D_DOEPCTL(0)) & EPCTL_EPENA)) {
            return false;
        }
        gadget_out(s, 0, NULL, 0, false);
        g->phase = PH_DONE;
        return true;

    default:
        return false;
    }
}

/* the configuration: the first MIDI streaming interface with bulk EPs */
static void parse_config(DWC2Gadget *g)
{
    const uint8_t *d = g->ctrl;
    unsigned len = MIN(g->ctrl_len, g->cfg_total), i = 0;
    int cls = -1, sub = -1, ifn = -1, alt = -1;

    g->midi_out = g->midi_in = -1;
    g->midi_iface = g->midi_alt = -1;
    g->cfg_value = len >= 6 ? d[5] : 1;

    while (i + 2 <= len && d[i] >= 2 && i + d[i] <= len) {
        const uint8_t *p = d + i;

        if (p[1] == 4 && p[0] >= 9) {               /* interface */
            ifn = p[2];
            alt = p[3];
            cls = p[5];
            sub = p[6];
        } else if (p[1] == 5 && p[0] >= 7 && cls == 1 && sub == 3 &&
                   (p[3] & 3) == 2) {               /* bulk EP of MIDI */
            if (g->midi_iface < 0 ||
                (g->midi_iface == ifn && g->midi_alt == alt)) {
                g->midi_iface = ifn;
                g->midi_alt = alt;
                if (p[2] & 0x80) {
                    g->midi_in = p[2] & 0xf;
                } else {
                    g->midi_out = p[2] & 0xf;
                }
            }
        }
        i += d[i];
    }
    if (g->midi_iface < 0) {
        qemu_log_mask(LOG_UNIMP,
                      "dwc2-gadget: no USB MIDI streaming interface\n");
    }
}

/* the enumeration, one control transfer per step */
static void host_enumerate(DWC2State *s)
{
    DWC2Gadget *g = &s->gadget;

    while (ctrl_run(s)) {
        /* as far as the guest has armed */
    }
    if (g->phase != PH_IDLE && g->phase != PH_DONE) {
        return;
    }
    if (g->phase == PH_DONE) {
        bool ok = !g->ctrl_failed;

        g->phase = PH_IDLE;
        switch (g->step) {
        case ST_DEV64:
        case ST_ADDRESS:
        case ST_DEV18:
            g->step++;
            break;
        case ST_CFG9:
            g->cfg_total = ok && g->ctrl_len >= 4 ? lduw_le_p(g->ctrl + 2) : 0;
            if (g->cfg_total < 9 || g->cfg_total > DWC2_GADGET_CTRL) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "dwc2-gadget: bad configuration length %u\n",
                              g->cfg_total);
                g->step = ST_RUN;
                return;
            }
            g->step = ST_CFG;
            break;
        case ST_CFG:
            parse_config(g);
            g->step = ST_SETCFG;
            break;
        case ST_SETCFG:
            g->step = g->midi_alt > 0 ? ST_SETIF : ST_RUN;
            break;
        case ST_SETIF:
            g->step = ST_RUN;
            break;
        }
        if (!ok) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "dwc2-gadget: request %02x %02x stalled\n",
                          g->setup[0], g->setup[1]);
        }
    }

    switch (g->step) {
    case ST_DEV64:
        ctrl_start(g, 0x80, 6, 0x0100, 0, 64);
        break;
    case ST_ADDRESS:
        ctrl_start(g, 0x00, 5, 1, 0, 0);
        break;
    case ST_DEV18:
        ctrl_start(g, 0x80, 6, 0x0100, 0, 18);
        break;
    case ST_CFG9:
        ctrl_start(g, 0x80, 6, 0x0200, 0, 9);
        break;
    case ST_CFG:
        ctrl_start(g, 0x80, 6, 0x0200, 0, g->cfg_total);
        break;
    case ST_SETCFG:
        ctrl_start(g, 0x00, 9, g->cfg_value, 0, 0);
        break;
    case ST_SETIF:
        ctrl_start(g, 0x01, 11, g->midi_alt, g->midi_iface, 0);
        break;
    default:
        return;
    }
    while (ctrl_run(s)) {
    }
}

/* ---------------------------------------------------------------------------
 * the MIDI endpoints
 */

static void evq_put(void *opaque, const uint8_t ev[4])
{
    DWC2Gadget *g = opaque;
    unsigned i;

    if (g->evq_count + 4 > DWC2_GADGET_EVQ) {
        return;
    }
    for (i = 0; i < 4; i++) {
        g->evq[(g->evq_head + g->evq_count + i) % DWC2_GADGET_EVQ] = ev[i];
    }
    g->evq_count += 4;
}

static void gadget_flush_tx(DWC2State *s)
{
    DWC2Gadget *g = &s->gadget;
    int n;

    if (!g->tx->len) {
        return;
    }
    n = qemu_chr_fe_write(&g->chr, g->tx->data, g->tx->len);
    if (n > 0) {
        g_byte_array_remove_range(g->tx, 0, n);
    }
}

static void host_midi(DWC2State *s)
{
    DWC2Gadget *g = &s->gadget;
    uint8_t buf[2048], raw[2048];
    int ep, n;

    ep = g->midi_out;
    if (g->out_rate) {
        /* a token bucket of up to 10 ms: the gadget has no flow control */
        int64_t max = g->out_rate / 100 + 64;
        g->out_credit = MIN(g->out_credit + g->out_rate /
                            (NANOSECONDS_PER_SECOND / POLL_NS), max);
    }
    if (ep > 0 && g->evq_count && (R(D_DOEPCTL(ep)) & EPCTL_EPENA)) {
        unsigned room = R(D_DOEPTSIZ(ep)) & TSIZ_XFER_MASK;
        unsigned i;

        n = MIN(MIN(g->evq_count, room), sizeof(buf));
        if (g->out_rate) {
            n = MIN(n, MAX(g->out_credit, 0));
        }
        n &= ~3u;
        if (n) {
            for (i = 0; i < n; i++) {
                buf[i] = g->evq[(g->evq_head + i) % DWC2_GADGET_EVQ];
            }
            n = gadget_out(s, ep, buf, n, false);
            g->evq_head = (g->evq_head + n) % DWC2_GADGET_EVQ;
            g->evq_count -= n;
            g->out_credit -= n;
            g->midi_rx += n;
            qemu_chr_fe_accept_input(&g->chr);
        }
    }

    ep = g->midi_in;
    if (ep > 0 && g->tx->len < 64 * 1024 &&
        (R(D_DIEPCTL(ep)) & EPCTL_EPENA)) {
        n = gadget_in(s, ep, buf, sizeof(buf));
        if (n > 0) {
            unsigned r = usbmidi_decode(buf, n, raw);
            g_byte_array_append(g->tx, raw, r);
            g->midi_tx += r;
        }
    }
    gadget_flush_tx(s);
}

/* ---------------------------------------------------------------------------
 * the link state and the poll
 */

static void link_down(DWC2Gadget *g)
{
    g->link = DWC2_LINK_OFF;
    g->phase = PH_IDLE;
    g->step = ST_DEV64;
    g->midi_out = g->midi_in = -1;
}

static void gadget_poll(void *opaque)
{
    DWC2State *s = opaque;
    DWC2Gadget *g = &s->gadget;
    bool device = (s->gusbcfg & GUSBCFG_FORCEDEVMODE) &&
                  !(R(D_DCTL) & DCTL_SFTDISCON);

    if (!device) {
        if (g->link != DWC2_LINK_OFF) {
            link_down(g);
        }
    } else {
        switch (g->link) {
        case DWC2_LINK_OFF:
            g->link = DWC2_LINK_WAIT;
            break;
        case DWC2_LINK_WAIT:
            /* the driver is ready for the reset: its interrupts are on */
            if ((s->gintmsk & GINTSTS_USBRST) &&
                (s->gahbcfg & GAHBCFG_GLBL_INTR_EN)) {
                s->gintsts |= GINTSTS_USBRST;
                dwc2_irq_update(s);
                g->link = DWC2_LINK_RESET;
            }
            break;
        case DWC2_LINK_RESET:
            if (!(s->gintsts & GINTSTS_USBRST)) {
                R(D_DSTS) &= ~(3u << 1);        /* high speed */
                s->gintsts |= GINTSTS_ENUMDONE;
                dwc2_irq_update(s);
                g->link = DWC2_LINK_ENUM;
            }
            break;
        case DWC2_LINK_ENUM:
            if (!(s->gintsts & GINTSTS_ENUMDONE)) {
                g->link = DWC2_LINK_UP;
                g->step = ST_DEV64;
                g->phase = PH_IDLE;
                g->wait_until = now_ns() + 10 * SCALE_MS;
            }
            break;
        case DWC2_LINK_UP:
            if (now_ns() < g->wait_until) {
                break;
            }
            if (g->step != ST_RUN) {
                host_enumerate(s);
            } else {
                host_midi(s);
            }
            break;
        }
    }
    timer_mod(g->timer, now_ns() + POLL_NS);
}

/* ---------------------------------------------------------------------------
 * the chardev
 */

static int gadget_can_read(void *opaque)
{
    DWC2State *s = opaque;
    DWC2Gadget *g = &s->gadget;

    /* each byte makes 4 bytes of event packets at most */
    return (DWC2_GADGET_EVQ - g->evq_count) / 4;
}

static void gadget_read(void *opaque, const uint8_t *buf, int size)
{
    DWC2State *s = opaque;
    DWC2Gadget *g = &s->gadget;
    int i;

    for (i = 0; i < size; i++) {
        usbmidi_encode(&g->enc, buf[i], evq_put, g);
    }
}

static void gadget_event(void *opaque, QEMUChrEvent event)
{
    DWC2State *s = opaque;
    DWC2Gadget *g = &s->gadget;

    if (event == CHR_EVENT_OPENED || event == CHR_EVENT_CLOSED) {
        usbmidi_encoder_reset(&g->enc);
        g->evq_head = g->evq_count = 0;
        g_byte_array_set_size(g->tx, 0);
    }
}

/* ---------------------------------------------------------------------------
 * registers
 */

uint32_t dwc2_gadget_read(DWC2State *s, hwaddr off)
{
    DWC2Gadget *g = &s->gadget;

    if (!g->timer || off >= DWC2_DREG_SIZE) {
        return 0;                               /* host only, as before */
    }
    switch (off) {
    case D_DSTS:
        /* SOFFN: the (micro)frame number from the clock */
        return (R(D_DSTS) & ~(0x3fffu << 8)) |
               (((now_ns() / POLL_NS) & 0x3fff) << 8);
    default:
        if (off >= 0x118 && off < 0x300 && (off & 0x1f) == 0x18) {
            return 0x400;                       /* DTXFSTS: room */
        }
        return R(off);
    }
}

static void write_epctl(DWC2State *s, hwaddr off, uint32_t val,
                        bool in, int ep)
{
    DWC2Gadget *g = &s->gadget;
    uint32_t old = R(off);
    uint32_t v = (val & ~(EPCTL_ACTIONS | EPCTL_NAKSTS | EPCTL_EPENA)) |
                 (old & (EPCTL_NAKSTS | EPCTL_EPENA));

    if (val & EPCTL_SNAK) {
        v |= EPCTL_NAKSTS;
    }
    if (val & EPCTL_CNAK) {
        v &= ~EPCTL_NAKSTS;
    }
    if (val & EPCTL_EPENA) {
        v |= EPCTL_EPENA;
    }
    if (val & EPCTL_EPDIS) {
        /*
         * A disable of an enabled EP that keeps EPENA set in the write
         * (read-modify-write, as Circle's DisableEndpoint) is reported with
         * EPDISBLD; one that clears EPENA in the same write (Circle's
         * CancelTransfer) just stops the EP.
         */
        if ((old & EPCTL_EPENA) && (val & EPCTL_EPENA)) {
            if (in) {
                R(D_DIEPINT(ep)) |= EPINT_EPDISBLD;
            } else {
                R(D_DOEPINT(ep)) |= EPINT_EPDISBLD;
            }
        }
        v &= ~EPCTL_EPENA;
    }
    R(off) = v;
    gadget_update_irq(s);
}

void dwc2_gadget_write(DWC2State *s, hwaddr off, uint32_t val)
{
    DWC2Gadget *g = &s->gadget;

    if (!g->timer || off >= DWC2_DREG_SIZE) {
        return;
    }
    switch (off) {
    case D_DCTL:
        if (val & DCTL_SGOUTNAK) {
            s->gintsts |= GINTSTS_GOUTNAKEFF;
        }
        if (val & DCTL_CGOUTNAK) {
            s->gintsts &= ~GINTSTS_GOUTNAKEFF;
        }
        R(off) = val & ~(DCTL_SGOUTNAK | DCTL_CGOUTNAK | (3u << 7));
        dwc2_irq_update(s);
        return;
    case D_DSTS:
    case D_DAINT:
        return;                                 /* read-only */
    case D_DIEPMSK:
    case D_DOEPMSK:
    case D_DAINTMSK:
        R(off) = val;
        gadget_update_irq(s);
        return;
    default:
        break;
    }
    if (off >= 0x100 && off < 0x500) {
        bool in = off < 0x300;
        int ep = ((off - (in ? 0x100 : 0x300)) >> 5) & 0xf;

        switch (off & 0x1f) {
        case 0x00:
            write_epctl(s, off, val, in, ep);
            return;
        case 0x08:                              /* DxEPINT: W1C */
            R(off) &= ~val;
            gadget_update_irq(s);
            return;
        case 0x18:
            if (in) {
                return;                         /* DTXFSTS: read-only */
            }
            break;
        default:
            break;
        }
    }
    R(off) = val;
}

/* ---------------------------------------------------------------------------
 * life cycle
 */

void dwc2_gadget_reset(DWC2State *s)
{
    DWC2Gadget *g = &s->gadget;

    if (!g->timer) {
        return;
    }
    memset(g->dreg, 0, sizeof(g->dreg));
    link_down(g);
    s->gintsts &= ~(GINTSTS_IEPINT | GINTSTS_OEPINT | GINTSTS_GOUTNAKEFF);
}

void dwc2_gadget_realize(DWC2State *s, Error **errp)
{
    DWC2Gadget *g = &s->gadget;

    if (!dwc2_gadget_enabled(s)) {
        return;
    }
    g->evq = g_malloc0(DWC2_GADGET_EVQ);
    g->tx = g_byte_array_new();
    g->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, gadget_poll, s);
    link_down(g);
    qemu_chr_fe_set_handlers(&g->chr, gadget_can_read, gadget_read,
                             gadget_event, NULL, s, NULL, true);
    timer_mod(g->timer, now_ns() + POLL_NS);
}

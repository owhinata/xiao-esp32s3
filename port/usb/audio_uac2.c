/*
 * USB Audio Class 2.0 capture for the USB audio input (port/usb/audio.c),
 * on esp-usb's USB Host Library (lib/esp-usb, host/usb): esp-usb's
 * usb_host_uac takes UAC 1.0 devices only.  Kept to what a microphone
 * needs: one capture interface, 16-bit PCM, the clock source of its
 * terminal, the feature unit on its path.
 *
 * A client of the library of its own, with its task "uac2" (FreeRTOS, at
 * the UAC 1.0 driver's priority, shell/port/priorities_os.h): it runs the
 * client's events, which are the devices that come (each one opened for a
 * moment to look for a UAC 2.0 AudioControl interface, bInterfaceProtocol
 * 0x20, and an AudioStreaming one with an isochronous IN endpoint) and go
 * (the open one), the completion of the control transfers and of the
 * isochronous IN transfers.  An isochronous transfer's completion copies
 * the packets that arrived, at their actual lengths, into a byte ring
 * buffer (ESP-IDF's API, port/esp_compat/freertos/ringbuf.c) and resubmits
 * the transfer at once; CONFIG_UAC_NUM_ISOC_URBS transfers of
 * CONFIG_UAC_NUM_PACKETS_PER_URB packets are in flight, as in the UAC 1.0
 * driver.
 *
 * Everything else runs in audio.c's task "uac": parsing the descriptors,
 * the class requests to the clock and the feature unit (each one a control
 * transfer that it waits for), claiming the interface, starting and
 * stopping, reading the ring buffer.  The device stays open by this client
 * from uac2_open() to uac2_close(), so the client gets DEV_GONE when it
 * goes.  Only the AudioStreaming interface is claimed (the control
 * requests to the AudioControl interface need no claim); the UAC 1.0
 * driver never opens a UAC 2.0 device for long (audio.c gives such a
 * device to this file), and this file opens a UAC 1.0 device only for the
 * look at its descriptors.
 *
 * The parsing follows the descriptors, not one device: the AudioStreaming
 * interface's terminal link names an output terminal, whose clock source
 * is the clock to program (through a clock selector only when it has one
 * input; a clock multiplier or a selector with more is refused); the
 * output terminal's source chain up to the input terminal gives the first
 * feature unit, whose volume and mute controls are used where they are
 * programmable.
 */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "tx_api.h"

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "usb/usb_host.h"

#include "app_log.h"
#include "priorities.h"
#include "usb/audio_uac2.h"

LOG_MODULE_REGISTER(uac2, LOG_LEVEL_INF);

/* UAC 2.0 codes (USB Audio Device Class 2.0, appendix A). */
#define UAC2_PROTOCOL           0x20u   /* IP_VERSION_02_00 */
#define AUDIO_SUBCLASS_CONTROL  0x01u
#define AUDIO_SUBCLASS_STREAMING 0x02u
#define CS_INTERFACE            0x24u
#define AC_OUTPUT_TERMINAL      0x03u
#define AC_FEATURE_UNIT         0x06u
#define AC_CLOCK_SOURCE         0x0au
#define AC_CLOCK_SELECTOR       0x0bu
#define AS_GENERAL              0x01u
#define AS_FORMAT_TYPE          0x02u
#define FORMAT_TYPE_I           0x01u
#define REQUEST_CUR             0x01u
#define REQUEST_RANGE           0x02u
#define CS_SAM_FREQ_CONTROL     0x01u
#define FU_MUTE_CONTROL         0x01u
#define FU_VOLUME_CONTROL       0x02u
#define REQ_CLASS_IF_IN         0xa1u   /* device to host, class, interface */
#define REQ_CLASS_IF_OUT        0x21u

#define ENTITIES_MAX            24
#define CTRL_DATA_MAX           256u
#define CTRL_TIMEOUT_MS         1000u   /* the library has no timeout of its own */
#define GONE_WAIT_MS            2000u
#define FREQ_RANGES_READ        8u      /* subranges asked for */
#define STOP_WAIT_MS            200u

struct uac2_alt {
    uint8_t alt;
    uint8_t channels;
    uint8_t subslot;
    uint8_t bits;
    uint8_t ep;
    uint8_t sync;               /* the endpoint's synchronization type, USBAUDIO_SYNC_* */
    uint16_t mps;
};

/* What the descriptors say about the capture interface. */
struct uac2_desc {
    uint8_t ac_iface;
    uint8_t as_iface;
    uint8_t clock_id;
    bool clock_settable;
    uint8_t fu_id;
    uint16_t vol_map;           /* channels (0: master) with a programmable control */
    uint16_t mute_map;
    uint8_t alt_count;
    struct uac2_alt alts[USBAUDIO_ALTS_MAX];
};

static struct {
    /* Client and its task. */
    usb_host_client_handle_t client;
    const struct uac2_callbacks *cb;
    bool installed;
    volatile bool quit;
    TX_SEMAPHORE done;          /* the task's answers */
    bool done_created;
    esp_err_t result;
    TX_MUTEX open_lock;         /* the client's opens: the task's look and uac2_open() */
    SemaphoreHandle_t ctrl_done;
    usb_transfer_t *ctrl;
    volatile bool ctrl_busy;    /* the control transfer is in flight */

    /* The open device ("uac", but gone, which the client task sets). */
    usb_device_handle_t dev;
    volatile bool gone;
    bool failed;                /* a request got no answer: nothing more is sent to it */
    bool claimed;               /* the capture interface is claimed */
    struct uac2_desc d;
    bool vol_range_valid;
    int16_t vol_min;
    int16_t vol_max;
    int16_t vol_res;
    uint8_t range_count;
    struct usbaudio_rate_range ranges[USBAUDIO_RATES_MAX];

    /* The stream. */
    const struct uac2_alt *cur;
    usb_transfer_t *xfers[CONFIG_UAC_NUM_ISOC_URBS];
    RingbufHandle_t ring;
    uint32_t threshold;
    volatile bool running;
    volatile int inflight;      /* changed only in inflight_add(): "uac" and the callback both do */
    volatile uint32_t overruns;
    volatile uint32_t packet_errors;
    volatile uint32_t transfer_errors;
    volatile uint32_t wakeups;
} u2;

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t) (p[0] | (p[1] << 8));
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

/* ---- Descriptors ---------------------------------------------------------- */

/*
 * Every walk below stops at a descriptor whose bLength is below 2 or runs
 * past wTotalLength (which the library checked against the bytes the
 * device sent), and reads a field only when bLength covers it.
 */
#define FOR_EACH_DESC(p, total, off) \
    for (size_t off = 0; off + 2u <= (total) && (p)[off] >= 2u && off + (p)[off] <= (total); off += (p)[off])

/* A data endpoint (not explicit or implicit feedback) for isochronous IN. */
static bool is_data_in_iso(const uint8_t *d)
{
    const usb_ep_desc_t *e = (const usb_ep_desc_t *) d;

    return d[0] >= USB_EP_DESC_SIZE && USB_EP_DESC_GET_EP_DIR(e)
           && USB_EP_DESC_GET_XFERTYPE(e) == USB_TRANSFER_TYPE_ISOCHRONOUS && USB_EP_DESC_GET_USAGETYPE(e) == 0u;
}

static bool is_audio_if(const uint8_t *d, uint8_t subclass)
{
    return d[1] == USB_B_DESCRIPTOR_TYPE_INTERFACE && d[0] >= USB_INTF_DESC_SIZE && d[5] == USB_CLASS_AUDIO
           && d[6] == subclass && d[7] == UAC2_PROTOCOL;
}

/*
 * Whether a configuration has a UAC 2.0 AudioControl interface and an
 * AudioStreaming one with an isochronous IN data endpoint; that
 * interface's number in *as (the first one; uac2_open() looks at all).
 */
static bool is_uac2_capture(const usb_config_desc_t *cd, uint8_t *as)
{
    const uint8_t *p = (const uint8_t *) cd;
    size_t total = cd->wTotalLength;
    bool uac2 = false;
    bool streaming = false;
    bool found = false;
    uint8_t iface = 0;

    FOR_EACH_DESC(p, total, off) {
        const uint8_t *d = &p[off];

        if (d[1] == USB_B_DESCRIPTOR_TYPE_INTERFACE && d[0] >= USB_INTF_DESC_SIZE) {
            streaming = is_audio_if(d, AUDIO_SUBCLASS_STREAMING);
            iface = d[2];
            uac2 = uac2 || is_audio_if(d, AUDIO_SUBCLASS_CONTROL);
        } else if (d[1] == USB_B_DESCRIPTOR_TYPE_ENDPOINT && streaming && !found && is_data_in_iso(d)) {
            *as = iface;
            found = true;
        }
    }
    return uac2 && found;
}

/* The AudioControl interface's entities, by ID. */
struct entities {
    uint8_t count;
    const uint8_t *desc[ENTITIES_MAX];
};

static const uint8_t *entity(const struct entities *en, uint8_t id)
{
    for (uint8_t i = 0; i < en->count; i++) {
        if (en->desc[i][3] == id) {
            return en->desc[i];
        }
    }
    return NULL;
}

/*
 * One AudioStreaming interface as a capture interface: its alternates with
 * PCM and an isochronous IN data endpoint, its terminal link to an output
 * terminal of type USB streaming (0x0101: toward the host), that
 * terminal's clock and the first feature unit on its source chain.  0, or
 * -EIO (logged: why this interface does not qualify).
 */
static int parse(const usb_config_desc_t *cd, uint8_t as_iface, struct uac2_desc *out)
{
    const uint8_t *p = (const uint8_t *) cd;
    size_t total = cd->wTotalLength;
    struct entities en = {0};
    struct uac2_alt alt = {0};
    bool in_ac = false;
    bool in_as = false;
    uint8_t link = 0;
    const uint8_t *e;

    memset(out, 0, sizeof(*out));
    out->as_iface = as_iface;
    FOR_EACH_DESC(p, total, off) {
        const uint8_t *d = &p[off];

        if (d[1] == USB_B_DESCRIPTOR_TYPE_INTERFACE && d[0] >= USB_INTF_DESC_SIZE) {
            in_ac = is_audio_if(d, AUDIO_SUBCLASS_CONTROL) && en.count == 0u;
            if (in_ac) {
                out->ac_iface = d[2];
            }
            in_as = d[2] == as_iface && d[3] != 0u && is_audio_if(d, AUDIO_SUBCLASS_STREAMING);
            memset(&alt, 0, sizeof(alt));
            alt.alt = d[3];
        } else if (d[1] == CS_INTERFACE && d[0] >= 4u && in_ac) {
            if (d[2] != 0x01u && en.count < ENTITIES_MAX) {      /* not the header */
                en.desc[en.count++] = d;
            }
        } else if (d[1] == CS_INTERFACE && d[0] >= 3u && in_as) {
            if (d[2] == AS_GENERAL && d[0] >= 16u) {
                if ((d[6] & 0x01u) == 0u || d[5] != FORMAT_TYPE_I || (link != 0u && d[3] != link)) {
                    in_as = false;                              /* not PCM, or another terminal: skipped */
                    continue;
                }
                link = d[3];
                alt.channels = d[10];
            } else if (d[2] == AS_FORMAT_TYPE && d[0] >= 6u && d[3] == FORMAT_TYPE_I) {
                alt.subslot = d[4];
                alt.bits = d[5];
            }
        } else if (d[1] == USB_B_DESCRIPTOR_TYPE_ENDPOINT && in_as && is_data_in_iso(d)) {
            const usb_ep_desc_t *ep = (const usb_ep_desc_t *) d;

            if (alt.channels != 0u && alt.subslot != 0u && out->alt_count < USBAUDIO_ALTS_MAX) {
                alt.ep = ep->bEndpointAddress;
                alt.mps = (uint16_t) USB_EP_DESC_GET_MPS(ep);
                alt.sync = (uint8_t) USB_EP_DESC_GET_SYNCTYPE(ep);
                out->alts[out->alt_count++] = alt;
            }
            in_as = false;
        }
    }
    if (out->alt_count == 0u || link == 0u) {
        LOG_ERR("interface %u: no PCM alternate setting with an isochronous IN data endpoint", as_iface);
        return -EIO;
    }

    e = entity(&en, link);
    if (e == NULL || e[2] != AC_OUTPUT_TERMINAL || e[0] < 12u || le16(&e[4]) != 0x0101u) {
        LOG_ERR("interface %u: terminal link %u is not a USB streaming output terminal", as_iface, link);
        return -EIO;
    }
    out->clock_id = e[8];
    for (uint8_t id = e[7], hops = 0; id != 0u && hops < 8u; hops++) {
        const uint8_t *s = entity(&en, id);

        if (s == NULL || s[2] != AC_FEATURE_UNIT || s[0] < 10u) {
            break;                                              /* the input terminal, or a unit with more sources */
        }
        if (out->fu_id == 0u) {
            size_t channels = ((size_t) s[0] - 6u) / 4u;        /* master and the logical channels */

            out->fu_id = s[3];
            for (size_t ch = 0; ch < channels && ch < 16u; ch++) {
                uint32_t controls = le32(&s[5 + 4 * ch]);

                if ((controls & 0x3u) == 0x3u) {
                    out->mute_map |= (uint16_t) (1u << ch);
                }
                if (((controls >> 2) & 0x3u) == 0x3u) {
                    out->vol_map |= (uint16_t) (1u << ch);
                }
            }
        }
        id = s[4];
    }
    for (uint8_t hops = 0; hops < 4u; hops++) {
        e = entity(&en, out->clock_id);
        if (e != NULL && e[2] == AC_CLOCK_SOURCE && e[0] >= 8u) {
            out->clock_settable = (e[5] & 0x3u) == 0x3u;
            return 0;
        }
        if (e != NULL && e[2] == AC_CLOCK_SELECTOR && e[0] >= 7u && e[4] == 1u) {
            out->clock_id = e[5];                               /* one input: nothing to select */
            continue;
        }
        break;
    }
    LOG_ERR("interface %u: clock %u is not a clock source (a clock multiplier, or a selector of several, "
            "is not supported)", as_iface, out->clock_id);
    return -EIO;
}

/*
 * The capture interface: every UAC 2.0 AudioStreaming interface with an
 * isochronous IN data endpoint, in the configuration's order, until one
 * parses (a playback interface's feedback endpoint is not data, so it is
 * not tried).  0, or -EIO.
 */
static int find_capture(const usb_config_desc_t *cd, struct uac2_desc *out)
{
    const uint8_t *p = (const uint8_t *) cd;
    size_t total = cd->wTotalLength;
    uint32_t tried[8] = {0};    /* interface numbers, a bit each */
    bool streaming = false;
    uint8_t iface = 0;

    FOR_EACH_DESC(p, total, off) {
        const uint8_t *d = &p[off];

        if (d[1] == USB_B_DESCRIPTOR_TYPE_INTERFACE && d[0] >= USB_INTF_DESC_SIZE) {
            streaming = is_audio_if(d, AUDIO_SUBCLASS_STREAMING);
            iface = d[2];
        } else if (d[1] == USB_B_DESCRIPTOR_TYPE_ENDPOINT && streaming && is_data_in_iso(d)
                   && (tried[iface >> 5] & (1u << (iface & 31u))) == 0u) {
            tried[iface >> 5] |= 1u << (iface & 31u);
            if (parse(cd, iface, out) == 0) {
                return 0;
            }
        }
    }
    return -EIO;
}

/*
 * A UAC 1.0 device's configuration, checked for what esp-usb's
 * usb_host_uac reads when uac_host_device_open() opens streaming
 * interface iface (_uac_host_device_add(), uac_host_interface_add() and the
 * feature unit search): it walks with the library's helpers, which keep
 * the next descriptor's first 2 bytes inside wTotalLength but read its
 * other fields without looking at its bLength, and it reads class-specific
 * fields without length checks.  So the device is refused unless:
 *   - every descriptor has a bLength of 2 or more and ends within
 *     wTotalLength, interface descriptors 9 bytes or more (at most
 *     USB_MAX_ENDPOINTS_PER_INTERFACE endpoints), endpoints 7 or more;
 *   - the first AudioControl interface's header (8 bytes or more) has a
 *     wTotalLength within the configuration, a descriptor follows that
 *     interface's class-specific descriptors (the driver reads the next
 *     one's type), and inside the header's span every descriptor ends
 *     within the span and holds what is read for its subtype: an output
 *     terminal 8 bytes, a feature unit 7, a mixer or selector unit 5 plus
 *     its input pins;
 *   - interface iface has an alternate setting 0, and from it on every
 *     class-specific interface descriptor holds what the driver reads: a
 *     general one 7 bytes, a format type one 4, and for type I 8 plus its
 *     sampling frequencies (6 bytes for a range, 3 for each of up to
 *     UAC_FREQ_NUM_MAX discrete ones), every class-specific endpoint
 *     descriptor 4.
 * 0, or -EINVAL with *why set.
 */
#define UAC1_FREQ_KEPT          CONFIG_UAC_FREQ_NUM_MAX /* usb_host_uac keeps this many */

static int uac1_config_check(const usb_config_desc_t *cd, uint8_t iface, const char **why)
{
    const uint8_t *p = (const uint8_t *) cd;
    size_t total = cd->wTotalLength;
    size_t end = 0;
    size_t span_end = 0;            /* of the AudioControl header's wTotalLength */
    bool in_ac = false;             /* among the first AudioControl interface's descriptors */
    bool ac_seen = false;
    bool ac_needs_next = false;     /* its class-specific block ended the configuration so far */
    bool from_iface = false;        /* at or after interface iface alternate 0 */

    FOR_EACH_DESC(p, total, off) {
        const uint8_t *d = &p[off];
        uint8_t len = d[0];

        end = off + len;
        ac_needs_next = false;
        if (d[1] == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
            if (len < USB_INTF_DESC_SIZE || d[4] > USB_MAX_ENDPOINTS_PER_INTERFACE) {
                *why = "a short interface descriptor";
                return -EINVAL;
            }
            in_ac = !ac_seen && d[5] == USB_CLASS_AUDIO && d[6] == AUDIO_SUBCLASS_CONTROL;
            ac_seen = ac_seen || in_ac;
            if (d[2] == iface && d[3] == 0u) {
                from_iface = true;
            }
            continue;
        }
        if (d[1] == USB_B_DESCRIPTOR_TYPE_ENDPOINT) {
            if (len < USB_EP_DESC_SIZE) {
                *why = "a short endpoint descriptor";
                return -EINVAL;
            }
            in_ac = false;
            continue;
        }
        if (d[1] == CS_INTERFACE && in_ac) {
            ac_needs_next = true;
            if (len < 3u) {
                *why = "a short AudioControl descriptor";
                return -EINVAL;
            }
            if (d[2] == 0x01u && span_end == 0u) {
                size_t span;

                if (len < 8u || (span = le16(&d[5])) < len || off + span > total) {
                    *why = "an AudioControl header that is short or runs past the configuration";
                    return -EINVAL;
                }
                span_end = off + span;
            }
        } else if (d[1] != CS_INTERFACE) {
            in_ac = false;
        }
        if (off < span_end) {
            size_t need = 3;

            if (end > span_end) {
                *why = "an AudioControl descriptor that runs past the header's wTotalLength";
                return -EINVAL;
            }
            switch (d[2]) {
            case 0x03:                  /* output terminal: bSourceID */
                need = 8;
                break;
            case 0x06:                  /* feature unit */
                need = 7;
                break;
            case 0x04:                  /* mixer unit */
            case 0x05:                  /* selector unit */
                need = len >= 5u ? 5u + d[4] : 5u;
                break;
            default:
                break;
            }
            if (len < need) {
                *why = "a short AudioControl unit or terminal";
                return -EINVAL;
            }
            continue;
        }
        if (from_iface && d[1] == CS_INTERFACE) {
            size_t need = 3;

            if (len >= 3u && d[2] == 0x01u) {               /* AS general */
                need = 7;
            } else if (len >= 3u && d[2] == 0x02u) {        /* format type */
                need = 4;
                if (len >= 4u && d[3] == 0x01u) {
                    uint8_t n = len >= 8u ? d[7] : 0u;

                    need = 8u + (n == 0u ? 6u : 3u * (n < UAC1_FREQ_KEPT ? n : UAC1_FREQ_KEPT));
                }
            }
            if (len < need) {
                *why = "a short AudioStreaming descriptor (general or format type)";
                return -EINVAL;
            }
        } else if (from_iface && d[1] == 0x25u && len < 4u) {
            *why = "a short class-specific endpoint descriptor";
            return -EINVAL;
        }
    }
    if (end != total) {
        *why = "a descriptor shorter than 2 bytes, or one that runs past wTotalLength";
        return -EINVAL;
    }
    if (ac_needs_next) {
        *why = "the AudioControl descriptors end the configuration";
        return -EINVAL;
    }
    if (!from_iface) {
        *why = "no alternate setting 0 of the streaming interface";
        return -EINVAL;
    }
    return 0;
}

/* ---- Control requests ------------------------------------------------------ */

static void ctrl_done(usb_transfer_t *transfer)
{
    (void) transfer;
    u2.ctrl_busy = false;
    (void) xSemaphoreGive(u2.ctrl_done);
}

/*
 * One control transfer to the open device, len bytes of data (IN or OUT
 * by type); *actual the bytes received.  0, or -EIO (logged).
 *
 * The library has no timeout for a control transfer and no way to take
 * one back from endpoint 0: one that the device never answers (it NAKs)
 * stays in flight until the device goes (unplugged, or the root port
 * powered off: the library then flushes endpoint 0 and completes it).  So
 * after CTRL_TIMEOUT_MS the device is marked failed: no request is sent
 * to it any more, the transfer and its semaphore stay as they are, and the
 * device is not closed while the transfer is in flight (the library
 * asserts on that).  Every call therefore waits at most CTRL_TIMEOUT_MS,
 * and after a timeout returns at once.
 */
static int control(uint8_t type, uint8_t request, uint16_t value, uint16_t index, uint8_t *data,
                   uint16_t len, size_t *actual)
{
    usb_transfer_t *x = u2.ctrl;
    usb_setup_packet_t *setup = (usb_setup_packet_t *) x->data_buffer;
    bool in = (type & 0x80u) != 0u;
    esp_err_t err;

    if (len > CTRL_DATA_MAX) {
        return -EINVAL;
    }
    if (u2.failed || u2.ctrl_busy || u2.gone) {
        return -EIO;
    }
    setup->bmRequestType = type;
    setup->bRequest = request;
    setup->wValue = value;
    setup->wIndex = index;
    setup->wLength = len;
    if (!in && len > 0u) {
        memcpy(x->data_buffer + sizeof(usb_setup_packet_t), data, len);
    }
    x->device_handle = u2.dev;
    x->bEndpointAddress = 0;
    x->callback = ctrl_done;
    x->context = NULL;
    x->timeout_ms = CTRL_TIMEOUT_MS;
    x->num_bytes = (int) (sizeof(usb_setup_packet_t) + len);
    (void) xSemaphoreTake(u2.ctrl_done, 0);
    u2.ctrl_busy = true;
    err = usb_host_transfer_submit_control(u2.client, x);
    if (err != ESP_OK) {
        u2.ctrl_busy = false;
        LOG_ERR("request %02x %02x %04x %04x: submit error 0x%x", type, request, value, index,
                (unsigned int) err);
        return -EIO;
    }
    if (xSemaphoreTake(u2.ctrl_done, pdMS_TO_TICKS(CTRL_TIMEOUT_MS)) != pdTRUE) {
        u2.failed = true;
        LOG_ERR("request %02x %02x %04x %04x: no answer in %u ms; the device is not used any more "
                "(unplug it)", type, request, value, index, (unsigned int) CTRL_TIMEOUT_MS);
        return -EIO;
    }
    if (x->status != USB_TRANSFER_STATUS_COMPLETED) {
        LOG_ERR("request %02x %02x %04x %04x: status %d", type, request, value, index, (int) x->status);
        return -EIO;
    }
    if (in) {
        size_t n = x->actual_num_bytes > (int) sizeof(usb_setup_packet_t)
                       ? (size_t) x->actual_num_bytes - sizeof(usb_setup_packet_t) : 0u;

        if (n > len) {
            n = len;
        }
        memcpy(data, x->data_buffer + sizeof(usb_setup_packet_t), n);
        if (actual != NULL) {
            *actual = n;
        }
    }
    return 0;
}

static uint16_t ac_index(uint8_t entity_id)
{
    return (uint16_t) ((entity_id << 8) | u2.d.ac_iface);
}

static int freq_get(uint32_t *rate)
{
    uint8_t b[4];
    size_t n = 0;
    int ret = control(REQ_CLASS_IF_IN, REQUEST_CUR, CS_SAM_FREQ_CONTROL << 8, ac_index(u2.d.clock_id), b,
                      sizeof(b), &n);

    if (ret == 0 && n != sizeof(b)) {
        ret = -EIO;
    }
    if (ret == 0) {
        *rate = le32(b);
    }
    return ret;
}

/* The clock's ranges: the count first, then that many (at most FREQ_RANGES_READ). */
static void freq_ranges_read(void)
{
    static uint8_t b[2 + 12 * FREQ_RANGES_READ];
    size_t n = 0;
    uint16_t count;

    u2.range_count = 0;
    if (control(REQ_CLASS_IF_IN, REQUEST_RANGE, CS_SAM_FREQ_CONTROL << 8, ac_index(u2.d.clock_id), b, 2, &n)
            != 0 || n != 2u) {
        return;
    }
    count = le16(b);
    if (count > FREQ_RANGES_READ) {
        count = FREQ_RANGES_READ;
    }
    if (count == 0u || control(REQ_CLASS_IF_IN, REQUEST_RANGE, CS_SAM_FREQ_CONTROL << 8,
                               ac_index(u2.d.clock_id), b, (uint16_t) (2u + 12u * count), &n) != 0) {
        return;
    }
    for (uint16_t i = 0; i < count && 2u + 12u * (i + 1u) <= n && u2.range_count < USBAUDIO_RATES_MAX; i++) {
        struct usbaudio_rate_range *r = &u2.ranges[u2.range_count++];

        r->min = le32(&b[2 + 12 * i]);
        r->max = le32(&b[6 + 12 * i]);
        r->res = le32(&b[10 + 12 * i]);
    }
}

static bool rate_offered(uint32_t rate)
{
    if (u2.range_count == 0u) {
        return true;                    /* unknown: the device decides */
    }
    for (uint8_t i = 0; i < u2.range_count; i++) {
        const struct usbaudio_rate_range *r = &u2.ranges[i];

        if (rate >= r->min && rate <= r->max && (r->res == 0u || (rate - r->min) % r->res == 0u)) {
            return true;
        }
    }
    return false;
}

static int first_channel(uint16_t map)
{
    for (int ch = 0; ch < 16; ch++) {
        if ((map & (1u << ch)) != 0u) {
            return ch;
        }
    }
    return -1;
}

static void volume_range_read(void)
{
    uint8_t b[8];
    size_t n = 0;
    int ch = first_channel(u2.d.vol_map);

    u2.vol_range_valid = false;
    if (u2.d.fu_id == 0u || ch < 0) {
        return;
    }
    if (control(REQ_CLASS_IF_IN, REQUEST_RANGE, (uint16_t) ((FU_VOLUME_CONTROL << 8) | ch),
                ac_index(u2.d.fu_id), b, sizeof(b), &n) == 0 && n >= 8u) {
        u2.vol_min = (int16_t) le16(&b[2]);
        u2.vol_max = (int16_t) le16(&b[4]);
        u2.vol_res = (int16_t) le16(&b[6]);
        u2.vol_range_valid = u2.vol_max > u2.vol_min;
    }
}

/* ---- Client task ------------------------------------------------------------- */

/* A device that came: opened for a look at its configuration. */
static void look(uint8_t address)
{
    usb_device_handle_t dev;
    const usb_config_desc_t *cd;
    bool found = false;
    uint8_t as = 0;

    (void) tx_mutex_get(&u2.open_lock, TX_WAIT_FOREVER);
    if (usb_host_device_open(u2.client, address, &dev) == ESP_OK) {
        if (usb_host_get_active_config_descriptor(dev, &cd) == ESP_OK && cd != NULL) {
            found = is_uac2_capture(cd, &as);
        }
        (void) usb_host_device_close(u2.client, dev);
    }
    (void) tx_mutex_put(&u2.open_lock);
    if (found) {
        u2.cb->connected(address, as);
    }
}

static void client_event(const usb_host_client_event_msg_t *msg, void *arg)
{
    (void) arg;

    switch (msg->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
        look(msg->new_dev.address);
        break;
    case USB_HOST_CLIENT_EVENT_DEV_GONE:
        if (msg->dev_gone.dev_hdl == u2.dev && u2.dev != NULL) {
            u2.gone = true;
            u2.cb->gone();
        }
        break;
    default:
        break;
    }
}

/*
 * Runs the client's events until uac2_uninstall() asks it to end; ends
 * only once the client is deregistered, so a failed deregistration leaves
 * the task there for the next attempt.
 */
static void client_task(void *arg)
{
    const usb_host_client_config_t config = {
        .is_synchronous = false,
        .max_num_event_msg = 8,
        .async = {
            .client_event_callback = client_event,
            .callback_arg = NULL,
        },
    };
    esp_err_t err;

    (void) arg;

    err = usb_host_client_register(&config, &u2.client);
    u2.result = err;
    (void) tx_semaphore_put(&u2.done);
    if (err != ESP_OK) {
        vTaskDelete(NULL);
        return;
    }
    for (;;) {
        while (!u2.quit) {
            (void) usb_host_client_handle_events(u2.client, portMAX_DELAY);
        }
        err = usb_host_client_deregister(u2.client);
        u2.quit = false;
        u2.result = err;
        (void) tx_semaphore_put(&u2.done);
        if (err == ESP_OK) {
            break;
        }
    }
    vTaskDelete(NULL);
}

/* ---- Isochronous IN ------------------------------------------------------------ */

/*
 * The transfers in flight: counted up by "uac" when it submits, down by
 * "uac2" (which preempts it) when one comes back for good, so the update
 * is made in a critical section.
 */
static void inflight_add(int n)
{
    portENTER_CRITICAL();
    u2.inflight += n;
    portEXIT_CRITICAL();
}

static void iso_done(usb_transfer_t *x)
{
    UBaseType_t waiting = 0;

    if (!u2.running || x->status == USB_TRANSFER_STATUS_NO_DEVICE
        || x->status == USB_TRANSFER_STATUS_CANCELED) {
        inflight_add(-1);
        return;
    }
    if (x->status == USB_TRANSFER_STATUS_COMPLETED) {
        for (int i = 0; i < x->num_isoc_packets; i++) {
            const usb_isoc_packet_desc_t *pk = &x->isoc_packet_desc[i];

            if (pk->status != USB_TRANSFER_STATUS_COMPLETED) {
                u2.packet_errors++;
                continue;
            }
            if (pk->actual_num_bytes > 0
                && xRingbufferSend(u2.ring, x->data_buffer + (size_t) i * u2.cur->mps,
                                   (size_t) pk->actual_num_bytes, 0) != pdTRUE) {
                u2.overruns++;
            }
        }
    } else {
        u2.transfer_errors++;
    }
    vRingbufferGetInfo(u2.ring, NULL, NULL, NULL, NULL, &waiting);
    if (waiting >= u2.threshold) {
        u2.wakeups++;
        u2.cb->data();
    }
    /* Last: once it is not in flight, uac2_stop() may free the ring buffer. */
    if (usb_host_transfer_submit(x) != ESP_OK) {
        u2.transfer_errors++;
        inflight_add(-1);
    }
}

/* ---- API --------------------------------------------------------------------- */

int uac2_install(const struct uac2_callbacks *callbacks)
{
    int ret = -ENOMEM;

    if (u2.installed) {
        return 0;
    }
    /* A failed install leaves nothing behind, so that a later one can succeed. */
    if (!u2.done_created) {
        if (tx_semaphore_create(&u2.done, "uac2", 0) != TX_SUCCESS) {
            return -ENOMEM;
        }
        if (tx_mutex_create(&u2.open_lock, "uac2", TX_INHERIT) != TX_SUCCESS) {
            (void) tx_semaphore_delete(&u2.done);
            return -ENOMEM;
        }
        u2.done_created = true;
    }
    u2.cb = callbacks;
    u2.quit = false;
    u2.dev = NULL;
    u2.ctrl_done = xSemaphoreCreateBinary();
    if (u2.ctrl_done == NULL) {
        return -ENOMEM;
    }
    if (usb_host_transfer_alloc(sizeof(usb_setup_packet_t) + CTRL_DATA_MAX, 0, &u2.ctrl) != ESP_OK) {
        u2.ctrl = NULL;
        goto fail_sem;
    }
    if (xTaskCreate(client_task, "uac2", APP_STACK_UAC2_CLIENT, NULL, APP_PRIO_UAC_DRIVER, NULL) != pdPASS) {
        goto fail_transfer;
    }
    (void) tx_semaphore_get(&u2.done, TX_WAIT_FOREVER);
    if (u2.result != ESP_OK) {
        LOG_ERR("usb_host_client_register: error 0x%x", (unsigned int) u2.result);
        ret = -EIO;
        goto fail_transfer;
    }
    u2.installed = true;
    return 0;

fail_transfer:
    (void) usb_host_transfer_free(u2.ctrl);
    u2.ctrl = NULL;
fail_sem:
    vSemaphoreDelete(u2.ctrl_done);
    u2.ctrl_done = NULL;
    return ret;
}

/*
 * Closes the device first.  A device with a control transfer still in
 * flight (failed) cannot be closed until it goes: the root port is
 * powered off (the library then reports every device gone and completes
 * the transfer) and that is waited for up to GONE_WAIT_MS.  The client's
 * task answers within 5 s.  -EIO when the device stays open (unplugging it
 * and stopping again ends it) or the deregistration fails (the task stays
 * for the next attempt).
 */
int uac2_uninstall(void)
{
    if (!u2.installed) {
        return 0;
    }
    if (u2.dev != NULL && uac2_close() != 0) {
        if (u2.ctrl_busy && !u2.gone) {
            LOG_WRN("a request is still in flight: the root port is powered off to end it");
            (void) usb_host_lib_set_root_port_power(false);
            for (uint32_t ms = 0; (u2.ctrl_busy || !u2.gone) && ms < GONE_WAIT_MS; ms += 10u) {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        }
        if (uac2_close() != 0) {
            LOG_ERR("the UAC 2.0 device stays open; unplug it, then stop again");
            return -EIO;
        }
    }
    u2.quit = true;
    (void) usb_host_client_unblock(u2.client);
    if (tx_semaphore_get(&u2.done, 5u * TX_TIMER_TICKS_PER_SECOND) != TX_SUCCESS) {
        LOG_ERR("no answer from uac2");
        return -ETIMEDOUT;
    }
    if (u2.result != ESP_OK) {
        LOG_ERR("usb_host_client_deregister: error 0x%x", (unsigned int) u2.result);
        return -EIO;
    }
    (void) usb_host_transfer_free(u2.ctrl);
    vSemaphoreDelete(u2.ctrl_done);
    u2.installed = false;
    return 0;
}

static void string_of(const usb_str_desc_t *desc, char *out, size_t size)
{
    size_t n = 0;

    if (desc != NULL && desc->bLength >= 2u) {
        for (size_t i = 0; i < (desc->bLength - 2u) / 2u && n + 1u < size; i++) {
            uint16_t c = desc->wData[i];

            out[n++] = (c >= 0x20u && c < 0x7Fu) ? (char) c : '?';
        }
    }
    out[n] = '\0';
}

int uac2_open(uint8_t address, uint8_t interface, struct usbaudio_info *info)
{
    const usb_config_desc_t *cd;
    const usb_device_desc_t *dd;
    usb_device_info_t dev_info;
    uint8_t as = 0;
    uint32_t rate = 0;

    if (!u2.installed || u2.dev != NULL || u2.ctrl_busy) {
        return -EIO;
    }
    (void) tx_mutex_get(&u2.open_lock, TX_WAIT_FOREVER);
    if (usb_host_device_open(u2.client, address, &u2.dev) != ESP_OK) {
        (void) tx_mutex_put(&u2.open_lock);
        u2.dev = NULL;
        return -EIO;
    }
    (void) tx_mutex_put(&u2.open_lock);
    u2.gone = false;
    u2.failed = false;
    u2.claimed = false;
    u2.range_count = 0;
    if (usb_host_get_active_config_descriptor(u2.dev, &cd) != ESP_OK || cd == NULL) {
        (void) uac2_close();
        return -EIO;
    }
    if (!is_uac2_capture(cd, &as)) {
        /* For usb_host_uac: its parser is only safe on what this lets through. */
        const char *why = "";
        int ret = uac1_config_check(cd, interface, &why);

        if (ret != 0) {
            LOG_ERR("device %u interface %u refused: %s", address, interface, why);
        }
        (void) uac2_close();
        return ret != 0 ? ret : -ENOTSUP;
    }
    if (find_capture(cd, &u2.d) != 0) {
        (void) uac2_close();
        return -EIO;
    }
    as = u2.d.as_iface;
    freq_ranges_read();
    (void) freq_get(&rate);
    volume_range_read();

    memset(info, 0, sizeof(*info));
    info->open = true;
    info->uac_version = 2;
    info->address = address;
    info->interface = as;
    if (usb_host_get_device_descriptor(u2.dev, &dd) == ESP_OK && dd != NULL) {
        info->vid = dd->idVendor;
        info->pid = dd->idProduct;
    }
    if (usb_host_device_info(u2.dev, &dev_info) == ESP_OK) {
        string_of(dev_info.str_desc_manufacturer, info->manufacturer, sizeof(info->manufacturer));
        string_of(dev_info.str_desc_product, info->product, sizeof(info->product));
    }
    for (uint8_t i = 0; i < u2.d.alt_count; i++) {
        info->alts[i].channels = u2.d.alts[i].channels;
        info->alts[i].bits = u2.d.alts[i].bits;
        info->alts[i].subframe = u2.d.alts[i].subslot;
        info->alts[i].max_packet = u2.d.alts[i].mps;
        info->alts[i].sync = u2.d.alts[i].sync;
    }
    info->alt_count = u2.d.alt_count;
    info->clock_id = u2.d.clock_id;
    info->clock_range_count = u2.range_count;
    memcpy(info->clock_ranges, u2.ranges, sizeof(info->clock_ranges));
    info->clock_rate = rate;
    info->feature_unit = u2.d.fu_id;
    info->volume_range_valid = u2.vol_range_valid;
    info->volume_min = u2.vol_min;
    info->volume_max = u2.vol_max;
    info->volume_res = u2.vol_res;
    LOG_INF("UAC 2.0: interface %u, %u alternate settings, clock %u%s at %u Hz (%u ranges), "
            "feature unit %u", as, u2.d.alt_count, u2.d.clock_id, u2.d.clock_settable ? "" : " (fixed)",
            (unsigned int) rate, u2.range_count, u2.d.fu_id);
    return 0;
}

/*
 * What cannot be released now stays as it is, for the next call (when the
 * device goes, or at uac2_uninstall()): a stream whose transfers did not
 * come back, a claim that was not released, a control transfer in flight.
 */
int uac2_close(void)
{
    esp_err_t err;

    if (u2.dev == NULL) {
        return 0;
    }
    if (u2.ring != NULL && uac2_stop() != 0 && u2.ring != NULL) {
        return -EBUSY;
    }
    if (u2.claimed) {
        if (usb_host_interface_release(u2.client, u2.dev, u2.d.as_iface) != ESP_OK) {
            return -EBUSY;
        }
        u2.claimed = false;
    }
    if (u2.ctrl_busy) {
        return -EBUSY;
    }
    (void) tx_mutex_get(&u2.open_lock, TX_WAIT_FOREVER);
    err = usb_host_device_close(u2.client, u2.dev);
    if (err == ESP_OK) {
        u2.dev = NULL;
    }
    (void) tx_mutex_put(&u2.open_lock);
    if (err != ESP_OK) {
        LOG_ERR("usb_host_device_close: error 0x%x", (unsigned int) err);
        return -EIO;
    }
    u2.failed = false;
    return 0;
}

bool uac2_failed(void)
{
    return u2.dev != NULL && u2.failed;
}

static void transfers_free(void)
{
    for (int i = 0; i < CONFIG_UAC_NUM_ISOC_URBS; i++) {
        if (u2.xfers[i] != NULL) {
            (void) usb_host_transfer_free(u2.xfers[i]);
            u2.xfers[i] = NULL;
        }
    }
}

static int set_interface(uint8_t alt)
{
    return control(USB_BM_REQUEST_TYPE_DIR_OUT | USB_BM_REQUEST_TYPE_TYPE_STANDARD
                   | USB_BM_REQUEST_TYPE_RECIP_INTERFACE, USB_B_REQUEST_SET_INTERFACE, alt, u2.d.as_iface,
                   NULL, 0, NULL);
}

/*
 * The largest packet a stream can bring in a 1 ms frame: rate / 1000
 * frames of channels subslots, exactly when the endpoint is synchronous
 * or adaptive (locked to the host's frames) and the rate is a whole number
 * of frames per millisecond; one frame more when it is asynchronous (its
 * own clock, a frame more or less now and then) or the rate is not (44.1 kHz:
 * 44 or 45 frames).
 */
uint32_t uac2_packet_need(uint32_t rate, uint8_t channels, uint8_t subslot, uint8_t sync)
{
    uint32_t frames = rate / 1000u;

    if (rate % 1000u != 0u || sync == USBAUDIO_SYNC_ASYNC) {
        frames++;
    }
    return frames * channels * subslot;
}

int uac2_start(uint32_t rate, uint8_t channels, uint32_t buffer_bytes, uint32_t threshold)
{
    const struct uac2_alt *a = NULL;
    uint32_t cur = 0;
    uint8_t b[4];
    esp_err_t err;

    if (u2.dev == NULL || u2.gone || u2.failed || u2.ring != NULL || u2.claimed) {
        return -EIO;
    }
    for (uint8_t i = 0; i < u2.d.alt_count; i++) {
        const struct uac2_alt *c = &u2.d.alts[i];

        if (c->channels == channels && c->bits == 16u && c->subslot == 2u
            && c->mps >= uac2_packet_need(rate, channels, c->subslot, c->sync)) {
            a = c;
            break;
        }
    }
    if (a == NULL || !rate_offered(rate)) {
        return -ENOTSUP;
    }

    /* The clock first (it is not part of the alternate setting), verified. */
    if (u2.d.clock_settable) {
        b[0] = (uint8_t) rate;
        b[1] = (uint8_t) (rate >> 8);
        b[2] = (uint8_t) (rate >> 16);
        b[3] = (uint8_t) (rate >> 24);
        if (control(REQ_CLASS_IF_OUT, REQUEST_CUR, CS_SAM_FREQ_CONTROL << 8, ac_index(u2.d.clock_id), b,
                    sizeof(b), NULL) != 0) {
            return -EIO;
        }
    }
    if (freq_get(&cur) != 0 || cur != rate) {
        LOG_ERR("clock %u runs at %u Hz, not %u", u2.d.clock_id, (unsigned int) cur, (unsigned int) rate);
        return -EIO;
    }

    u2.ring = xRingbufferCreate(buffer_bytes, RINGBUF_TYPE_BYTEBUF);
    if (u2.ring == NULL) {
        return -ENOMEM;
    }
    u2.threshold = threshold;
    u2.cur = a;
    u2.overruns = 0;
    u2.packet_errors = 0;
    u2.transfer_errors = 0;
    u2.wakeups = 0;
    for (int i = 0; i < CONFIG_UAC_NUM_ISOC_URBS; i++) {
        usb_transfer_t *x;

        if (usb_host_transfer_alloc((size_t) a->mps * CONFIG_UAC_NUM_PACKETS_PER_URB,
                                    CONFIG_UAC_NUM_PACKETS_PER_URB, &u2.xfers[i]) != ESP_OK) {
            transfers_free();
            vRingbufferDelete(u2.ring);
            u2.ring = NULL;
            return -ENOMEM;
        }
        x = u2.xfers[i];
        x->device_handle = u2.dev;
        x->bEndpointAddress = a->ep;
        x->callback = iso_done;
        x->context = NULL;
        x->timeout_ms = 100;
        x->num_bytes = (int) a->mps * CONFIG_UAC_NUM_PACKETS_PER_URB;
        for (int j = 0; j < CONFIG_UAC_NUM_PACKETS_PER_URB; j++) {
            x->isoc_packet_desc[j].num_bytes = a->mps;
        }
    }
    err = usb_host_interface_claim(u2.client, u2.dev, u2.d.as_iface, a->alt);
    if (err != ESP_OK) {
        LOG_ERR("interface %u alternate %u: claim error 0x%x", u2.d.as_iface, a->alt, (unsigned int) err);
        transfers_free();
        vRingbufferDelete(u2.ring);
        u2.ring = NULL;
        return -EIO;
    }
    u2.claimed = true;
    if (set_interface(a->alt) != 0) {
        if (usb_host_interface_release(u2.client, u2.dev, u2.d.as_iface) == ESP_OK) {
            u2.claimed = false;         /* else uac2_close() tries again */
        }
        transfers_free();
        vRingbufferDelete(u2.ring);
        u2.ring = NULL;
        return -EIO;
    }
    u2.running = true;
    for (int i = 0; i < CONFIG_UAC_NUM_ISOC_URBS; i++) {
        inflight_add(1);
        err = usb_host_transfer_submit(u2.xfers[i]);
        if (err != ESP_OK) {
            inflight_add(-1);
            LOG_ERR("endpoint 0x%02x: submit error 0x%x", a->ep, (unsigned int) err);
            (void) uac2_stop();
            return -EIO;
        }
    }
    LOG_INF("interface %u alternate %u: endpoint 0x%02x, %u bytes per packet", u2.d.as_iface, a->alt, a->ep,
            a->mps);
    return 0;
}

int uac2_stop(void)
{
    int ret = 0;

    if (u2.ring == NULL) {
        return 0;
    }
    u2.running = false;
    /* Alternate setting 0 for a device that is there and answers; the transfers are taken back all the same. */
    if (!u2.gone && !u2.failed && set_interface(0) != 0) {
        ret = -EIO;
    }
    (void) usb_host_endpoint_halt(u2.dev, u2.cur->ep);
    (void) usb_host_endpoint_flush(u2.dev, u2.cur->ep);
    for (uint32_t ms = 0; u2.inflight > 0 && ms < STOP_WAIT_MS; ms++) {
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    (void) usb_host_endpoint_clear(u2.dev, u2.cur->ep);
    if (u2.inflight > 0) {
        /* The transfers stay allocated rather than freed while in flight. */
        LOG_ERR("%d transfers still in flight", u2.inflight);
        return -EIO;
    }
    if (usb_host_interface_release(u2.client, u2.dev, u2.d.as_iface) != ESP_OK) {
        LOG_ERR("interface %u: release failed; tried again at the close", u2.d.as_iface);
        ret = -EIO;
    } else {
        u2.claimed = false;
    }
    transfers_free();
    vRingbufferDelete(u2.ring);
    u2.ring = NULL;
    return ret;
}

int uac2_read(uint8_t *buf, size_t size, size_t *got)
{
    size_t total = 0;

    while (total < size && u2.ring != NULL) {
        size_t n = 0;
        uint8_t *item = xRingbufferReceiveUpTo(u2.ring, &n, 0, size - total);

        if (item == NULL) {
            break;
        }
        memcpy(buf + total, item, n);
        vRingbufferReturnItem(u2.ring, item);
        total += n;
    }
    *got = total;
    return total > 0u ? 0 : -EAGAIN;
}

int uac2_volume_get(int16_t *volume, uint8_t *percent)
{
    uint8_t b[2];
    size_t n = 0;
    int ch = first_channel(u2.d.vol_map);
    int16_t v;

    if (u2.dev == NULL || ch < 0) {
        return -ENOTSUP;
    }
    if (control(REQ_CLASS_IF_IN, REQUEST_CUR, (uint16_t) ((FU_VOLUME_CONTROL << 8) | ch),
                ac_index(u2.d.fu_id), b, sizeof(b), &n) != 0 || n != 2u) {
        return -EIO;
    }
    v = (int16_t) le16(b);
    *volume = v;
    *percent = 0;
    if (u2.vol_range_valid) {
        int32_t span = u2.vol_max - u2.vol_min;
        int32_t p = ((v - u2.vol_min) * 100 + span / 2) / span;

        *percent = (uint8_t) (p < 0 ? 0 : p > 100 ? 100 : p);
    }
    return 0;
}

/*
 * As esp-usb's usb_host_uac maps a percentage: min + (max - min) p / 100,
 * rounded to a multiple of the resolution, within the range.
 */
int uac2_volume_set(uint8_t percent)
{
    int32_t v;
    uint8_t b[2];

    if (u2.dev == NULL || u2.d.vol_map == 0u || !u2.vol_range_valid) {
        return -ENOTSUP;
    }
    v = u2.vol_min + ((u2.vol_max - u2.vol_min) * (int32_t) percent + 50) / 100;
    if (u2.vol_res > 0) {
        v = (v >= 0 ? v + u2.vol_res / 2 : v - u2.vol_res / 2) / u2.vol_res * u2.vol_res;
    }
    v = v < u2.vol_min ? u2.vol_min : v > u2.vol_max ? u2.vol_max : v;
    b[0] = (uint8_t) v;
    b[1] = (uint8_t) ((uint16_t) v >> 8);
    for (int ch = 0; ch < 16; ch++) {
        if ((u2.d.vol_map & (1u << ch)) != 0u
            && control(REQ_CLASS_IF_OUT, REQUEST_CUR, (uint16_t) ((FU_VOLUME_CONTROL << 8) | ch),
                       ac_index(u2.d.fu_id), b, sizeof(b), NULL) != 0) {
            return -EIO;
        }
    }
    LOG_INF("volume %u%%: %d/256 dB", percent, (int) v);
    return 0;
}

int uac2_mute_get(bool *mute)
{
    uint8_t b[1];
    size_t n = 0;
    int ch = first_channel(u2.d.mute_map);

    if (u2.dev == NULL || ch < 0) {
        return -ENOTSUP;
    }
    if (control(REQ_CLASS_IF_IN, REQUEST_CUR, (uint16_t) ((FU_MUTE_CONTROL << 8) | ch), ac_index(u2.d.fu_id),
                b, sizeof(b), &n) != 0 || n != 1u) {
        return -EIO;
    }
    *mute = b[0] != 0u;
    return 0;
}

int uac2_mute_set(bool mute)
{
    uint8_t b[1] = { mute ? 1u : 0u };

    if (u2.dev == NULL || u2.d.mute_map == 0u) {
        return -ENOTSUP;
    }
    for (int ch = 0; ch < 16; ch++) {
        if ((u2.d.mute_map & (1u << ch)) != 0u
            && control(REQ_CLASS_IF_OUT, REQUEST_CUR, (uint16_t) ((FU_MUTE_CONTROL << 8) | ch),
                       ac_index(u2.d.fu_id), b, sizeof(b), NULL) != 0) {
            return -EIO;
        }
    }
    return 0;
}

void uac2_counters(struct uac2_counters *c)
{
    c->overruns = u2.overruns;
    c->packet_errors = u2.packet_errors;
    c->transfer_errors = u2.transfer_errors;
    c->wakeups = u2.wakeups;
}

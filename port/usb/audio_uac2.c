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
#define AC_INPUT_TERMINAL       0x02u
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
#define CTRL_TIMEOUT_MS         1000u
#define FREQ_RANGES_READ        8u      /* subranges asked for */
#define STOP_WAIT_MS            200u

struct uac2_alt {
    uint8_t alt;
    uint8_t channels;
    uint8_t subslot;
    uint8_t bits;
    uint8_t ep;
    uint16_t mps;
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

    /* The open device ("uac", but gone, which the client task sets). */
    usb_device_handle_t dev;
    volatile bool gone;
    uint8_t ac_iface;
    uint8_t as_iface;
    uint8_t clock_id;
    bool clock_settable;
    uint8_t fu_id;
    uint16_t vol_map;           /* channels (0: master) with a programmable control */
    uint16_t mute_map;
    bool vol_range_valid;
    int16_t vol_min;
    int16_t vol_max;
    int16_t vol_res;
    uint8_t alt_count;
    struct uac2_alt alts[USBAUDIO_ALTS_MAX];
    uint8_t range_count;
    struct usbaudio_rate_range ranges[USBAUDIO_RATES_MAX];

    /* The stream. */
    const struct uac2_alt *cur;
    usb_transfer_t *xfers[CONFIG_UAC_NUM_ISOC_URBS];
    RingbufHandle_t ring;
    uint32_t threshold;
    volatile bool running;
    volatile int inflight;
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
 * Whether a configuration has a UAC 2.0 AudioControl interface; the first
 * AudioStreaming interface of it with an isochronous IN endpoint in *as.
 */
static bool is_uac2_capture(const usb_config_desc_t *cd, uint8_t *as)
{
    const uint8_t *p = (const uint8_t *) cd;
    size_t total = cd->wTotalLength;
    bool uac2 = false;
    bool streaming = false;
    uint8_t iface = 0;

    for (size_t off = 0; off + 2u <= total && p[off] >= 2u && off + p[off] <= total; off += p[off]) {
        const uint8_t *d = &p[off];

        if (d[1] == USB_B_DESCRIPTOR_TYPE_INTERFACE && d[0] >= USB_INTF_DESC_SIZE) {
            const usb_intf_desc_t *i = (const usb_intf_desc_t *) d;

            streaming = i->bInterfaceClass == USB_CLASS_AUDIO
                        && i->bInterfaceSubClass == AUDIO_SUBCLASS_STREAMING
                        && i->bInterfaceProtocol == UAC2_PROTOCOL;
            iface = i->bInterfaceNumber;
            if (i->bInterfaceClass == USB_CLASS_AUDIO && i->bInterfaceSubClass == AUDIO_SUBCLASS_CONTROL
                && i->bInterfaceProtocol == UAC2_PROTOCOL) {
                uac2 = true;
            }
        } else if (d[1] == USB_B_DESCRIPTOR_TYPE_ENDPOINT && d[0] >= USB_EP_DESC_SIZE && streaming && uac2) {
            const usb_ep_desc_t *e = (const usb_ep_desc_t *) d;

            if (USB_EP_DESC_GET_EP_DIR(e) && USB_EP_DESC_GET_XFERTYPE(e) == USB_TRANSFER_TYPE_ISOCHRONOUS) {
                *as = iface;
                return true;
            }
        }
    }
    return false;
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
 * The capture interface's alternates, the AudioControl entities and from
 * them the clock and the feature unit.  0 or -EIO (logged).
 */
static int parse(const usb_config_desc_t *cd, uint8_t as_iface)
{
    const uint8_t *p = (const uint8_t *) cd;
    size_t total = cd->wTotalLength;
    struct entities en = {0};
    struct uac2_alt alt = {0};
    bool in_ac = false;
    bool in_as = false;
    uint8_t link = 0;
    const uint8_t *e;

    u2.alt_count = 0;
    for (size_t off = 0; off + 2u <= total && p[off] >= 2u && off + p[off] <= total; off += p[off]) {
        const uint8_t *d = &p[off];

        if (d[1] == USB_B_DESCRIPTOR_TYPE_INTERFACE && d[0] >= USB_INTF_DESC_SIZE) {
            const usb_intf_desc_t *i = (const usb_intf_desc_t *) d;

            in_ac = i->bInterfaceClass == USB_CLASS_AUDIO && i->bInterfaceSubClass == AUDIO_SUBCLASS_CONTROL
                    && i->bInterfaceProtocol == UAC2_PROTOCOL;
            if (in_ac) {
                u2.ac_iface = i->bInterfaceNumber;
            }
            in_as = i->bInterfaceNumber == as_iface && i->bAlternateSetting != 0u;
            memset(&alt, 0, sizeof(alt));
            alt.alt = i->bAlternateSetting;
        } else if (d[1] == CS_INTERFACE && d[0] >= 4u && in_ac) {
            if (d[2] != 0x01u && en.count < ENTITIES_MAX) {      /* not the header */
                en.desc[en.count++] = d;
            }
        } else if (d[1] == CS_INTERFACE && in_as) {
            if (d[2] == AS_GENERAL && d[0] >= 16u) {
                if ((d[6] & 0x01u) == 0u || d[5] != FORMAT_TYPE_I) {
                    in_as = false;                              /* not PCM: skipped */
                    continue;
                }
                link = link != 0u ? link : d[3];
                alt.channels = d[10];
            } else if (d[2] == AS_FORMAT_TYPE && d[0] >= 6u && d[3] == FORMAT_TYPE_I) {
                alt.subslot = d[4];
                alt.bits = d[5];
            }
        } else if (d[1] == USB_B_DESCRIPTOR_TYPE_ENDPOINT && d[0] >= USB_EP_DESC_SIZE && in_as) {
            const usb_ep_desc_t *ep = (const usb_ep_desc_t *) d;

            if (USB_EP_DESC_GET_EP_DIR(ep) && USB_EP_DESC_GET_XFERTYPE(ep) == USB_TRANSFER_TYPE_ISOCHRONOUS
                && alt.channels != 0u && alt.subslot != 0u && u2.alt_count < USBAUDIO_ALTS_MAX) {
                alt.ep = ep->bEndpointAddress;
                alt.mps = (uint16_t) USB_EP_DESC_GET_MPS(ep);
                u2.alts[u2.alt_count++] = alt;
            }
            in_as = false;
        }
    }
    if (u2.alt_count == 0u || link == 0u) {
        LOG_ERR("interface %u: no PCM alternate setting with an isochronous IN endpoint", as_iface);
        return -EIO;
    }

    /* The output terminal the stream is linked to, its clock, its source chain. */
    e = entity(&en, link);
    if (e == NULL || e[2] != AC_OUTPUT_TERMINAL || e[0] < 12u) {
        LOG_ERR("terminal link %u: not an output terminal", link);
        return -EIO;
    }
    u2.clock_id = e[8];
    for (uint8_t id = e[7], hops = 0; id != 0u && hops < 8u; hops++) {
        const uint8_t *s = entity(&en, id);

        if (s == NULL || s[2] != AC_FEATURE_UNIT || s[0] < 10u) {
            break;                                              /* the input terminal, or a unit with more sources */
        }
        if (u2.fu_id == 0u) {
            size_t channels = ((size_t) s[0] - 6u) / 4u;        /* master and the logical channels */

            u2.fu_id = s[3];
            for (size_t ch = 0; ch < channels && ch < 16u; ch++) {
                uint32_t controls = le32(&s[5 + 4 * ch]);

                if ((controls & 0x3u) == 0x3u) {
                    u2.mute_map |= (uint16_t) (1u << ch);
                }
                if (((controls >> 2) & 0x3u) == 0x3u) {
                    u2.vol_map |= (uint16_t) (1u << ch);
                }
            }
        }
        id = s[4];
    }
    for (uint8_t hops = 0; hops < 4u; hops++) {
        e = entity(&en, u2.clock_id);
        if (e != NULL && e[2] == AC_CLOCK_SOURCE && e[0] >= 8u) {
            u2.clock_settable = (e[5] & 0x3u) == 0x3u;
            return 0;
        }
        if (e != NULL && e[2] == AC_CLOCK_SELECTOR && e[0] >= 7u && e[4] == 1u) {
            u2.clock_id = e[5];                                 /* one input: nothing to select */
            continue;
        }
        break;
    }
    LOG_ERR("clock %u: not a clock source (a clock multiplier, or a selector of several, is not supported)",
            u2.clock_id);
    return -EIO;
}

/* ---- Control requests ------------------------------------------------------ */

static void ctrl_done(usb_transfer_t *transfer)
{
    (void) transfer;
    (void) xSemaphoreGive(u2.ctrl_done);
}

/*
 * One control transfer to the open device, len bytes of data (IN or OUT
 * by type); *actual the bytes received.  0, or -EIO (logged).
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
    err = usb_host_transfer_submit_control(u2.client, x);
    if (err != ESP_OK) {
        LOG_ERR("request %02x %02x %04x %04x: submit error 0x%x", type, request, value, index,
                (unsigned int) err);
        return -EIO;
    }
    if (xSemaphoreTake(u2.ctrl_done, pdMS_TO_TICKS(CTRL_TIMEOUT_MS + 500u)) != pdTRUE) {
        LOG_ERR("request %02x %02x %04x %04x: no answer", type, request, value, index);
        (void) usb_host_endpoint_halt(u2.dev, 0);
        (void) usb_host_endpoint_flush(u2.dev, 0);
        (void) usb_host_endpoint_clear(u2.dev, 0);
        (void) xSemaphoreTake(u2.ctrl_done, pdMS_TO_TICKS(CTRL_TIMEOUT_MS));
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
    return (uint16_t) ((entity_id << 8) | u2.ac_iface);
}

static int freq_get(uint32_t *rate)
{
    uint8_t b[4];
    size_t n = 0;
    int ret = control(REQ_CLASS_IF_IN, REQUEST_CUR, CS_SAM_FREQ_CONTROL << 8, ac_index(u2.clock_id), b,
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
    if (control(REQ_CLASS_IF_IN, REQUEST_RANGE, CS_SAM_FREQ_CONTROL << 8, ac_index(u2.clock_id), b, 2, &n)
            != 0 || n != 2u) {
        return;
    }
    count = le16(b);
    if (count > FREQ_RANGES_READ) {
        count = FREQ_RANGES_READ;
    }
    if (count == 0u || control(REQ_CLASS_IF_IN, REQUEST_RANGE, CS_SAM_FREQ_CONTROL << 8,
                               ac_index(u2.clock_id), b, (uint16_t) (2u + 12u * count), &n) != 0) {
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
    int ch = first_channel(u2.vol_map);

    u2.vol_range_valid = false;
    if (u2.fu_id == 0u || ch < 0) {
        return;
    }
    if (control(REQ_CLASS_IF_IN, REQUEST_RANGE, (uint16_t) ((FU_VOLUME_CONTROL << 8) | ch),
                ac_index(u2.fu_id), b, sizeof(b), &n) == 0 && n >= 8u) {
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
    while (!u2.quit) {
        (void) usb_host_client_handle_events(u2.client, portMAX_DELAY);
    }
    u2.result = usb_host_client_deregister(u2.client);
    (void) tx_semaphore_put(&u2.done);
    vTaskDelete(NULL);
}

/* ---- Isochronous IN ------------------------------------------------------------ */

static void iso_done(usb_transfer_t *x)
{
    UBaseType_t waiting = 0;

    if (!u2.running || x->status == USB_TRANSFER_STATUS_NO_DEVICE
        || x->status == USB_TRANSFER_STATUS_CANCELED) {
        u2.inflight--;
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
        u2.inflight--;
    }
}

/* ---- API --------------------------------------------------------------------- */

int uac2_install(const struct uac2_callbacks *callbacks)
{
    if (u2.installed) {
        return 0;
    }
    if (!u2.done_created) {
        if (tx_semaphore_create(&u2.done, "uac2", 0) != TX_SUCCESS
            || tx_mutex_create(&u2.open_lock, "uac2", TX_INHERIT) != TX_SUCCESS) {
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
        vSemaphoreDelete(u2.ctrl_done);
        return -ENOMEM;
    }
    if (xTaskCreate(client_task, "uac2", APP_STACK_UAC2_CLIENT, NULL, APP_PRIO_UAC_DRIVER, NULL) != pdPASS) {
        (void) usb_host_transfer_free(u2.ctrl);
        vSemaphoreDelete(u2.ctrl_done);
        return -ENOMEM;
    }
    (void) tx_semaphore_get(&u2.done, TX_WAIT_FOREVER);
    if (u2.result != ESP_OK) {
        LOG_ERR("usb_host_client_register: error 0x%x", (unsigned int) u2.result);
        (void) usb_host_transfer_free(u2.ctrl);
        vSemaphoreDelete(u2.ctrl_done);
        return -EIO;
    }
    u2.installed = true;
    return 0;
}

int uac2_uninstall(void)
{
    if (!u2.installed) {
        return 0;
    }
    if (u2.dev != NULL) {
        return -EIO;
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

int uac2_open(uint8_t address, struct usbaudio_info *info)
{
    const usb_config_desc_t *cd;
    const usb_device_desc_t *dd;
    usb_device_info_t dev_info;
    uint8_t as = 0;
    uint32_t rate = 0;

    if (!u2.installed || u2.dev != NULL) {
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
    if (usb_host_get_active_config_descriptor(u2.dev, &cd) != ESP_OK || cd == NULL || !is_uac2_capture(cd, &as)) {
        uac2_close();
        return -ENOTSUP;
    }
    u2.fu_id = 0;
    u2.vol_map = 0;
    u2.mute_map = 0;
    u2.range_count = 0;
    if (parse(cd, as) != 0) {
        uac2_close();
        return -EIO;
    }
    u2.as_iface = as;
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
    for (uint8_t i = 0; i < u2.alt_count; i++) {
        info->alts[i].channels = u2.alts[i].channels;
        info->alts[i].bits = u2.alts[i].bits;
        info->alts[i].subframe = u2.alts[i].subslot;
    }
    info->alt_count = u2.alt_count;
    info->clock_id = u2.clock_id;
    info->clock_range_count = u2.range_count;
    memcpy(info->clock_ranges, u2.ranges, sizeof(info->clock_ranges));
    info->clock_rate = rate;
    info->feature_unit = u2.fu_id;
    info->volume_range_valid = u2.vol_range_valid;
    info->volume_min = u2.vol_min;
    info->volume_max = u2.vol_max;
    info->volume_res = u2.vol_res;
    LOG_INF("UAC 2.0: interface %u, %u alternate settings, clock %u%s at %u Hz (%u ranges), "
            "feature unit %u", as, u2.alt_count, u2.clock_id, u2.clock_settable ? "" : " (fixed)",
            (unsigned int) rate, u2.range_count, u2.fu_id);
    return 0;
}

void uac2_close(void)
{
    if (u2.dev == NULL) {
        return;
    }
    if (u2.ring != NULL) {
        (void) uac2_stop();
    }
    (void) tx_mutex_get(&u2.open_lock, TX_WAIT_FOREVER);
    (void) usb_host_device_close(u2.client, u2.dev);
    u2.dev = NULL;
    (void) tx_mutex_put(&u2.open_lock);
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
                   | USB_BM_REQUEST_TYPE_RECIP_INTERFACE, USB_B_REQUEST_SET_INTERFACE, alt, u2.as_iface,
                   NULL, 0, NULL);
}

int uac2_start(uint32_t rate, uint8_t channels, uint32_t buffer_bytes, uint32_t threshold)
{
    const struct uac2_alt *a = NULL;
    uint32_t cur = 0;
    uint8_t b[4];
    esp_err_t err;

    if (u2.dev == NULL || u2.gone || u2.ring != NULL) {
        return -EIO;
    }
    for (uint8_t i = 0; i < u2.alt_count; i++) {
        if (u2.alts[i].channels == channels && u2.alts[i].bits == 16u && u2.alts[i].subslot == 2u) {
            a = &u2.alts[i];
            break;
        }
    }
    if (a == NULL || !rate_offered(rate)
        || (uint32_t) a->mps < (rate / 1000u + 1u) * channels * 2u) {
        return -ENOTSUP;
    }

    /* The clock first (it is not part of the alternate setting), verified. */
    if (u2.clock_settable) {
        b[0] = (uint8_t) rate;
        b[1] = (uint8_t) (rate >> 8);
        b[2] = (uint8_t) (rate >> 16);
        b[3] = (uint8_t) (rate >> 24);
        if (control(REQ_CLASS_IF_OUT, REQUEST_CUR, CS_SAM_FREQ_CONTROL << 8, ac_index(u2.clock_id), b,
                    sizeof(b), NULL) != 0) {
            return -EIO;
        }
    }
    if (freq_get(&cur) != 0 || cur != rate) {
        LOG_ERR("clock %u runs at %u Hz, not %u", u2.clock_id, (unsigned int) cur, (unsigned int) rate);
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
    err = usb_host_interface_claim(u2.client, u2.dev, u2.as_iface, a->alt);
    if (err != ESP_OK) {
        LOG_ERR("interface %u alternate %u: claim error 0x%x", u2.as_iface, a->alt, (unsigned int) err);
        transfers_free();
        vRingbufferDelete(u2.ring);
        u2.ring = NULL;
        return -EIO;
    }
    if (set_interface(a->alt) != 0) {
        (void) usb_host_interface_release(u2.client, u2.dev, u2.as_iface);
        transfers_free();
        vRingbufferDelete(u2.ring);
        u2.ring = NULL;
        return -EIO;
    }
    u2.running = true;
    for (int i = 0; i < CONFIG_UAC_NUM_ISOC_URBS; i++) {
        u2.inflight++;
        err = usb_host_transfer_submit(u2.xfers[i]);
        if (err != ESP_OK) {
            u2.inflight--;
            LOG_ERR("endpoint 0x%02x: submit error 0x%x", a->ep, (unsigned int) err);
            (void) uac2_stop();
            return -EIO;
        }
    }
    LOG_INF("interface %u alternate %u: endpoint 0x%02x, %u bytes per packet", u2.as_iface, a->alt, a->ep,
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
    if (!u2.gone && set_interface(0) != 0) {
        ret = -EIO;                     /* the transfers are taken back all the same */
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
    if (usb_host_interface_release(u2.client, u2.dev, u2.as_iface) != ESP_OK) {
        LOG_ERR("interface %u: release failed", u2.as_iface);
        ret = -EIO;
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
    int ch = first_channel(u2.vol_map);
    int16_t v;

    if (u2.dev == NULL || ch < 0) {
        return -ENOTSUP;
    }
    if (control(REQ_CLASS_IF_IN, REQUEST_CUR, (uint16_t) ((FU_VOLUME_CONTROL << 8) | ch),
                ac_index(u2.fu_id), b, sizeof(b), &n) != 0 || n != 2u) {
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

    if (u2.dev == NULL || u2.vol_map == 0u || !u2.vol_range_valid) {
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
        if ((u2.vol_map & (1u << ch)) != 0u
            && control(REQ_CLASS_IF_OUT, REQUEST_CUR, (uint16_t) ((FU_VOLUME_CONTROL << 8) | ch),
                       ac_index(u2.fu_id), b, sizeof(b), NULL) != 0) {
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
    int ch = first_channel(u2.mute_map);

    if (u2.dev == NULL || ch < 0) {
        return -ENOTSUP;
    }
    if (control(REQ_CLASS_IF_IN, REQUEST_CUR, (uint16_t) ((FU_MUTE_CONTROL << 8) | ch), ac_index(u2.fu_id),
                b, sizeof(b), &n) != 0 || n != 1u) {
        return -EIO;
    }
    *mute = b[0] != 0u;
    return 0;
}

int uac2_mute_set(bool mute)
{
    uint8_t b[1] = { mute ? 1u : 0u };

    if (u2.dev == NULL || u2.mute_map == 0u) {
        return -ENOTSUP;
    }
    for (int ch = 0; ch < 16; ch++) {
        if ((u2.mute_map & (1u << ch)) != 0u
            && control(REQ_CLASS_IF_OUT, REQUEST_CUR, (uint16_t) ((FU_MUTE_CONTROL << 8) | ch),
                       ac_index(u2.fu_id), b, sizeof(b), NULL) != 0) {
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

/*
 * USB host on the XIAO ESP32S3: esp-usb's USB Host Library (lib/esp-usb,
 * host/usb) with its hub driver, on the USB OTG controller and the
 * internal full-speed PHY, which the USB-C connector's D+ and D- reach
 * (GPIO20, GPIO19).  The same PHY serves the USB Serial/JTAG controller,
 * the console on that connector, until the host starts; usb_host_install()
 * switches it over (esp_hw_support's usb_phy.c, with the pull-downs of a
 * host on D+ and D-).
 *
 * Two FreeRTOS tasks, as in ESP-IDF's USB host examples, created by
 * usbhost_start() and ending in usbhost_stop() (their stacks and task
 * blocks go back to the FreeRTOS layer's pool):
 *
 *   "usb_host"    installs the library and runs usb_host_lib_handle_events():
 *                 the hub driver (root port and external hubs), the
 *                 enumeration, the device list.  It also watches the
 *                 enumeration, which the library does not bound in time
 *                 (see enum_watch()).  At the stop it frees every
 *                 device, uninstalls the library and gives the PHY back.
 *   "usb_client"  registers the one client and runs its events: it logs
 *                 the devices that come and go, and serves usbhost_list()
 *                 and usbhost_descriptors(), so that every call of the
 *                 client's API is made from the client's own task.  It
 *                 opens a device only for the time of such a request.
 *
 * The USB audio input (port/usb/audio.c) is the library's second client,
 * esp-usb's UAC driver with tasks of its own: usbhost_start() starts it
 * right after the library, before the client above, and usbhost_stop()
 * ends it first, so that its device is closed before every device is
 * freed.
 *
 * Both are FreeRTOS tasks because the library waits for the controller in
 * the caller's task with task notifications (hcd_dwc.c), which exist only
 * for the FreeRTOS layer's tasks.  The caller (the shell's thread) waits
 * for each answer on a ThreadX semaphore, at most HOST_ANSWER_TIMEOUT_MS.
 *
 * The library's automatic suspend is never armed: its timer callback
 * would run in the tick interrupt here (the FreeRTOS layer's timers), and
 * usb_host_lib_set_auto_suspend() is not called.  The library creates the
 * timer; it stays stopped.
 *
 * The controller's interrupt is a level-1 one (esp_intr_alloc(); its
 * handler in flash), masked while the flash driver has the cache off.
 */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "tx_api.h"

#include "esp_err.h"
#include "esp_intr_alloc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/usb_dwc_ll.h"
#include "hal/usb_serial_jtag_hal.h"
#include "usb/usb_host.h"

#include "app_log.h"
#include "board.h"
#include "priorities.h"
#include "usb/audio.h"
#include "usb/host.h"

LOG_MODULE_REGISTER(usbhost, LOG_LEVEL_INF);

/* How long a caller waits for a task's answer. */
#define HOST_ANSWER_TIMEOUT_MS  5000u

/* How often the library task looks at the enumeration and at a stop in progress. */
#define HOST_POLL_MS            1000u

/* A device that has not got its address and descriptors after this long is given up. */
#define ENUM_TIMEOUT_MS         5000u

/* How often in a row the root port is powered off and on for such a device. */
#define ENUM_RETRIES            3u

/* How long the root port stays off then. */
#define ENUM_POWER_OFF_MS       500u

/* At a stop, how long the devices get to be freed before the root port is powered off. */
#define STOP_FREE_TIMEOUT_MS    2000u

/* Event messages the client's queue holds (ESP-IDF's examples: 5). */
#define CLIENT_EVENT_MSG_MAX    8

enum host_state {
    HOST_STOPPED,
    HOST_RUNNING,
    HOST_BROKEN,                /* a task did not answer, or the stop failed half-way */
};

enum host_request {
    REQUEST_NONE,
    REQUEST_LIST,
    REQUEST_DESCRIPTORS,
};

static struct {
    enum host_state state;
    bool done_created;
    TX_SEMAPHORE done;          /* a task's answer to the caller */
    volatile bool stop;         /* usbhost_stop(): the tasks end */
    volatile bool client_up;    /* the client is registered */
    usb_host_client_handle_t client;
    esp_err_t result;           /* of the step a task answers for */
    /* The client task's request, with the caller's arguments and results. */
    volatile enum host_request request;
    struct usbhost_device *devices;
    int max;
    int count;
    uint8_t address;
    uint8_t *device_desc;
    uint8_t *config;
    size_t size;
    size_t config_len;
} host;

/* ---- Tasks' answers ---------------------------------------------------- */

static void answer(esp_err_t result)
{
    host.result = result;
    (void) tx_semaphore_put(&host.done);
}

/* 0, or -ETIMEDOUT: then nothing can be told about the tasks any more. */
static int wait_answer(const char *what)
{
    if (tx_semaphore_get(&host.done, (ULONG) HOST_ANSWER_TIMEOUT_MS * TX_TIMER_TICKS_PER_SECOND / 1000u)
        != TX_SUCCESS) {
        LOG_ERR("no answer from %s in %u ms", what, (unsigned int) HOST_ANSWER_TIMEOUT_MS);
        host.state = HOST_BROKEN;
        return -ETIMEDOUT;
    }
    return 0;
}

/*
 * The internal PHY back to the USB Serial/JTAG controller, as usb_phy.c
 * routes it for that controller (usb_new_phy() with USB_PHY_CTRL_SERIAL_JTAG),
 * and the console's USB transport on again.  After usb_host_uninstall(),
 * whose usb_del_phy() leaves the routing as it was, or after
 * usb_host_install() failed.
 */
static void phy_to_serial_jtag(void)
{
    usb_serial_jtag_hal_phy_set_external(NULL, false);
    console_usb_attach();
}

/* ---- Devices ----------------------------------------------------------- */

static enum usbhost_speed speed_of(usb_speed_t speed)
{
    switch (speed) {
    case USB_SPEED_LOW:
        return USBHOST_SPEED_LOW;
    case USB_SPEED_HIGH:
        return USBHOST_SPEED_HIGH;
    default:
        return USBHOST_SPEED_FULL;
    }
}

/* A string descriptor (UTF-16LE) in ASCII, '?' for any other character. */
static void string_of(const usb_str_desc_t *desc, char *out, size_t size)
{
    size_t n = 0;

    if (desc != NULL && desc->bLength >= 2u) {
        size_t chars = (desc->bLength - 2u) / 2u;

        for (size_t i = 0; i < chars && n + 1u < size; i++) {
            uint16_t c = desc->wData[i];

            out[n++] = (c >= 0x20u && c < 0x7Fu) ? (char) c : '?';
        }
    }
    out[n] = '\0';
}

/* The entry of an open device, but for its parent's address. */
static void device_fill(usb_device_handle_t dev, const usb_device_info_t *info,
                        struct usbhost_device *out)
{
    const usb_device_desc_t *dd;
    const usb_config_desc_t *cd;

    memset(out, 0, sizeof(*out));
    out->address = info->dev_addr;
    out->speed = speed_of(info->speed);
    out->parent_port = info->parent.port_num;
    string_of(info->str_desc_manufacturer, out->manufacturer, sizeof(out->manufacturer));
    string_of(info->str_desc_product, out->product, sizeof(out->product));
    string_of(info->str_desc_serial_num, out->serial, sizeof(out->serial));

    if (usb_host_get_device_descriptor(dev, &dd) == ESP_OK && dd != NULL) {
        out->vid = dd->idVendor;
        out->pid = dd->idProduct;
        out->device_class = dd->bDeviceClass;
        out->device_subclass = dd->bDeviceSubClass;
        out->device_protocol = dd->bDeviceProtocol;
    }
    if (usb_host_get_active_config_descriptor(dev, &cd) == ESP_OK && cd != NULL) {
        const uint8_t *p = (const uint8_t *) cd;
        size_t total = cd->wTotalLength;
        size_t off = 0;

        while (off + 2u <= total && p[off] >= 2u && off + p[off] <= total) {
            if (p[off + 1] == USB_B_DESCRIPTOR_TYPE_INTERFACE && p[off] >= USB_INTF_DESC_SIZE) {
                const usb_intf_desc_t *intf = (const usb_intf_desc_t *) &p[off];

                if (intf->bAlternateSetting == 0u
                    && out->interface_count < sizeof(out->interface_classes)) {
                    out->interface_classes[out->interface_count++] = intf->bInterfaceClass;
                }
            }
            off += p[off];
        }
    }
}

/*
 * Client task: opens every enumerated device (hubs included), with its
 * information.  The number open.
 */
static int open_all(usb_device_handle_t handles[USBHOST_DEVICES_MAX],
                    usb_device_info_t infos[USBHOST_DEVICES_MAX])
{
    uint8_t addrs[USBHOST_DEVICES_MAX];
    int n = 0;
    int open = 0;

    if (usb_host_device_addr_list_fill(USBHOST_DEVICES_MAX, addrs, &n) != ESP_OK) {
        return 0;
    }
    for (int i = 0; i < n; i++) {
        if (addrs[i] == 0u || usb_host_device_open(host.client, addrs[i], &handles[open]) != ESP_OK) {
            continue;
        }
        if (usb_host_device_info(handles[open], &infos[open]) != ESP_OK) {
            (void) usb_host_device_close(host.client, handles[open]);
            continue;
        }
        open++;
    }
    return open;
}

static void close_all(usb_device_handle_t handles[USBHOST_DEVICES_MAX], int open)
{
    for (int i = 0; i < open; i++) {
        (void) usb_host_device_close(host.client, handles[i]);
    }
}

/* The entry of open device i, its parent found among the open ones. */
static void entry_fill(usb_device_handle_t handles[USBHOST_DEVICES_MAX],
                       const usb_device_info_t infos[USBHOST_DEVICES_MAX], int open, int i,
                       struct usbhost_device *out)
{
    device_fill(handles[i], &infos[i], out);
    for (int j = 0; j < open; j++) {
        if (infos[i].parent.dev_hdl != NULL && handles[j] == infos[i].parent.dev_hdl) {
            out->parent_address = infos[j].dev_addr;
        }
    }
}

/* Client task: the list. */
static void serve_list(void)
{
    usb_device_handle_t handles[USBHOST_DEVICES_MAX];
    usb_device_info_t infos[USBHOST_DEVICES_MAX];
    int open = open_all(handles, infos);

    host.count = 0;
    for (int i = 0; i < open && host.count < host.max; i++) {
        entry_fill(handles, infos, open, i, &host.devices[host.count++]);
    }
    close_all(handles, open);
}

/* Client task: one device's entry and descriptors.  ESP_ERR_NOT_FOUND when it is not there. */
static esp_err_t serve_descriptors(void)
{
    usb_device_handle_t handles[USBHOST_DEVICES_MAX];
    usb_device_info_t infos[USBHOST_DEVICES_MAX];
    const usb_device_desc_t *dd;
    const usb_config_desc_t *cd;
    int open = open_all(handles, infos);
    esp_err_t err = ESP_ERR_NOT_FOUND;

    for (int i = 0; i < open; i++) {
        if (infos[i].dev_addr != host.address) {
            continue;
        }
        entry_fill(handles, infos, open, i, host.devices);
        err = usb_host_get_device_descriptor(handles[i], &dd);
        if (err == ESP_OK) {
            memcpy(host.device_desc, dd, USBHOST_DEVICE_DESC_LEN);
            err = usb_host_get_active_config_descriptor(handles[i], &cd);
        }
        if (err == ESP_OK) {
            host.config_len = cd->wTotalLength;
            if (host.size > 0u) {
                memcpy(host.config, cd, host.config_len < host.size ? host.config_len : host.size);
            }
        }
        break;
    }
    close_all(handles, open);
    return err;
}

/* ---- Client task ------------------------------------------------------- */

static void log_new_device(uint8_t address)
{
    usb_device_handle_t dev;
    usb_device_info_t info;
    struct usbhost_device d;

    if (usb_host_device_open(host.client, address, &dev) != ESP_OK) {
        LOG_INF("device %u connected", address);
        return;
    }
    if (usb_host_device_info(dev, &info) == ESP_OK) {
        device_fill(dev, &info, &d);
        LOG_INF("device %u connected: %04x:%04x, %s speed, \"%s\"", address, d.vid, d.pid,
                d.speed == USBHOST_SPEED_LOW ? "low" : d.speed == USBHOST_SPEED_HIGH ? "high" : "full",
                d.product);
    }
    (void) usb_host_device_close(host.client, dev);
}

/*
 * The client's events, in its task.  DEV_REMOVED comes for every device
 * this client does not have open when it goes; DEV_GONE for one it had
 * open at that moment, which it has closed since (it opens a device only
 * within a request), so only the fact is logged.  The hub driver keeps
 * hubs to itself: no event for them.
 */
static void client_event(const usb_host_client_event_msg_t *msg, void *arg)
{
    (void) arg;

    switch (msg->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
        log_new_device(msg->new_dev.address);
        break;
    case USB_HOST_CLIENT_EVENT_DEV_REMOVED:
        LOG_INF("device %u disconnected", msg->dev_removed.address);
        break;
    case USB_HOST_CLIENT_EVENT_DEV_GONE:
        LOG_INF("a device disconnected during a request");
        break;
    default:
        break;
    }
}

static void client_task(void *arg)
{
    const usb_host_client_config_t config = {
        .is_synchronous = false,
        .max_num_event_msg = CLIENT_EVENT_MSG_MAX,
        .flags.notify_dev_removed = 1,
        .async = {
            .client_event_callback = client_event,
            .callback_arg = NULL,
        },
    };
    esp_err_t err;

    (void) arg;

    err = usb_host_client_register(&config, &host.client);
    if (err != ESP_OK) {
        LOG_ERR("usb_host_client_register: error 0x%x", (unsigned int) err);
        answer(err);
        vTaskDelete(NULL);
        return;
    }
    host.client_up = true;
    answer(ESP_OK);

    while (!host.stop) {
        (void) usb_host_client_handle_events(host.client, portMAX_DELAY);
        switch (host.request) {
        case REQUEST_LIST:
            serve_list();
            host.request = REQUEST_NONE;
            answer(ESP_OK);
            break;
        case REQUEST_DESCRIPTORS:
            err = serve_descriptors();
            host.request = REQUEST_NONE;
            answer(err);
            break;
        default:
            break;
        }
    }

    /* No device is open here: nothing to close before deregistering. */
    err = usb_host_client_deregister(host.client);
    if (err != ESP_OK) {
        LOG_ERR("usb_host_client_deregister: error 0x%x", (unsigned int) err);
    } else {
        host.client_up = false;
    }
    answer(err);
    vTaskDelete(NULL);
}

/* ---- Library task ------------------------------------------------------ */

/*
 * The library on the root port of the one USB OTG controller (peripheral
 * map 0: the full-speed one on this chip), the PHY set up by the library
 * (the internal one, host mode), the root port powered at once, the FIFO
 * split of Kconfig (sdkconfig.h: balanced).
 */
/*
 * The library's enumeration has no timeout: a device that acknowledges the
 * first request and then never delivers the data (a CX31993 plugged in
 * again right after it went, seen on the board) keeps the control transfer
 * in flight for ever.  No other device is enumerated behind it, and at a
 * stop the library never reports that all devices are free.  The library
 * counts such a device (usb_host_lib_info()) and does not list it
 * (usb_host_device_addr_list_fill()); when that lasts ENUM_TIMEOUT_MS, the
 * root port is powered off, which ends the transfer and frees the device,
 * and on again ENUM_POWER_OFF_MS later, which starts the enumeration anew.
 * After ENUM_RETRIES attempts in a row it is left alone until it is
 * plugged in again or the host is restarted.  Called by the library task
 * every HOST_POLL_MS at most.
 */
static struct {
    TickType_t since;           /* when the pending device was first seen, or the power went off */
    bool pending;
    bool power_off;
    unsigned int retries;
} enum_state;                   /* cleared when the library task starts */

static void enum_watch(void)
{
    TickType_t now = xTaskGetTickCount();
    usb_host_lib_info_t info;
    uint8_t addrs[USBHOST_DEVICES_MAX];
    int listed = 0;

    if (enum_state.power_off) {
        if (now - enum_state.since >= pdMS_TO_TICKS(ENUM_POWER_OFF_MS)) {
            enum_state.power_off = false;
            (void) usb_host_lib_set_root_port_power(true);
        }
        return;
    }
    if (usb_host_lib_info(&info) != ESP_OK
        || usb_host_device_addr_list_fill(USBHOST_DEVICES_MAX, addrs, &listed) != ESP_OK) {
        return;
    }
    if (info.num_devices <= listed) {
        enum_state.pending = false;
        if (listed > 0) {
            enum_state.retries = 0;
        }
        return;
    }
    if (!enum_state.pending) {
        enum_state.pending = true;
        enum_state.since = now;
        return;
    }
    if (now - enum_state.since < pdMS_TO_TICKS(ENUM_TIMEOUT_MS) || enum_state.retries > ENUM_RETRIES) {
        return;
    }
    if (enum_state.retries == ENUM_RETRIES) {
        enum_state.retries++;
        LOG_ERR("a device does not answer the enumeration; plug it in again or restart the host");
        return;
    }
    enum_state.retries++;
    LOG_WRN("a device does not answer the enumeration: root port off and on, attempt %u of %u", enum_state.retries,
            ENUM_RETRIES);
    enum_state.pending = false;
    enum_state.power_off = true;
    enum_state.since = now;
    (void) usb_host_lib_set_root_port_power(false);
}

static void lib_task(void *arg)
{
    const usb_host_config_t config = {
        .skip_phy_setup = false,
        .root_port_unpowered = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    bool freeing = false;
    bool forced = false;
    TickType_t freeing_since = 0;
    esp_err_t err;

    (void) arg;

    err = usb_host_install(&config);
    if (err != ESP_OK) {
        LOG_ERR("usb_host_install: error 0x%x", (unsigned int) err);
        phy_to_serial_jtag();
        answer(err);
        vTaskDelete(NULL);
        return;
    }
    memset(&enum_state, 0, sizeof(enum_state));
    answer(ESP_OK);

    /*
     * ESP-IDF's order for the end: once the client has deregistered, free
     * every device (hubs included); when that is not done at once, the
     * library reports ALL_FREE when it is.  A device with a transfer in
     * flight that never ends (see enum_watch()) is never freed: after
     * STOP_FREE_TIMEOUT_MS the root port is powered off, which ends it.
     */
    for (;;) {
        uint32_t flags = 0;

        (void) usb_host_lib_handle_events(pdMS_TO_TICKS(HOST_POLL_MS), &flags);
        if (!host.stop) {
            enum_watch();
            continue;
        }
        if (host.client_up) {
            continue;
        }
        if (!freeing) {
            freeing = true;
            freeing_since = xTaskGetTickCount();
            err = usb_host_device_free_all();
            if (err == ESP_OK) {
                break;
            }
            if (err != ESP_ERR_NOT_FINISHED) {
                LOG_ERR("usb_host_device_free_all: error 0x%x", (unsigned int) err);
                break;
            }
        } else if ((flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) != 0u) {
            break;
        } else if (!forced && xTaskGetTickCount() - freeing_since >= pdMS_TO_TICKS(STOP_FREE_TIMEOUT_MS)) {
            forced = true;
            LOG_WRN("a device is not freed: root port off");
            (void) usb_host_lib_set_root_port_power(false);
        }
    }

    err = usb_host_uninstall();
    if (err != ESP_OK) {
        /* The library stays installed; its events keep being handled. */
        LOG_ERR("usb_host_uninstall: error 0x%x", (unsigned int) err);
        answer(err);
        for (;;) {
            (void) usb_host_lib_handle_events(portMAX_DELAY, NULL);
        }
    }
    phy_to_serial_jtag();
    answer(ESP_OK);
    vTaskDelete(NULL);
}

/* ---- API ---------------------------------------------------------------- */

static int ready(void)
{
    if (host.state == HOST_BROKEN) {
        return -EIO;
    }
    return host.state == HOST_RUNNING ? 0 : -EALREADY;
}

static int stop_tasks(void)
{
    int ret;

    host.stop = true;
    if (host.client_up) {
        (void) usb_host_client_unblock(host.client);
        ret = wait_answer("usb_client");
        if (ret != 0) {
            return ret;
        }
        if (host.result != ESP_OK) {
            host.state = HOST_BROKEN;
            return -EIO;
        }
    }
    (void) usb_host_lib_unblock();
    ret = wait_answer("usb_host");
    if (ret != 0) {
        return ret;
    }
    if (host.result != ESP_OK) {
        host.state = HOST_BROKEN;
        return -EIO;
    }
    host.state = HOST_STOPPED;
    return 0;
}

int usbhost_start(void)
{
    int ret;

    if (host.state != HOST_STOPPED) {
        return host.state == HOST_BROKEN ? -EIO : -EALREADY;
    }
    if (!host.done_created) {
        if (tx_semaphore_create(&host.done, "usbhost", 0) != TX_SUCCESS) {
            return -ENOMEM;
        }
        host.done_created = true;
    }
    host.stop = false;
    host.client_up = false;
    host.request = REQUEST_NONE;

    /* From here on the USB-C connector is the host's. */
    console_usb_detach();
    if (xTaskCreate(lib_task, "usb_host", APP_STACK_USB_HOST, NULL, APP_PRIO_USB_HOST, NULL) != pdPASS) {
        console_usb_attach();
        return -ENOMEM;
    }
    ret = wait_answer("usb_host");
    if (ret != 0) {
        return ret;
    }
    if (host.result != ESP_OK) {
        return host.result == ESP_ERR_NO_MEM ? -ENOMEM : -EIO;
    }
    host.state = HOST_RUNNING;

    /*
     * The audio driver's client registers before any device enumerates
     * (the debounce alone is CONFIG_USB_HOST_DEBOUNCE_DELAY_MS): a client
     * learns only of the devices that come after it.
     */
    ret = usbaudio_start();
    if (ret == -ETIMEDOUT) {
        host.state = HOST_BROKEN;
        return ret;
    }
    if (ret != 0) {
        int stopped = stop_tasks();

        return stopped != 0 ? stopped : ret;
    }

    if (xTaskCreate(client_task, "usb_client", APP_STACK_USB_CLIENT, NULL, APP_PRIO_USB_HOST, NULL)
        != pdPASS) {
        ret = -ENOMEM;
    } else {
        ret = wait_answer("usb_client");
        if (ret != 0) {
            return ret;
        }
        if (host.result != ESP_OK) {
            ret = host.result == ESP_ERR_NO_MEM ? -ENOMEM : -EIO;
        }
    }
    if (ret != 0) {
        /* The library without its client: uninstalled again, the audio driver first. */
        int stopped = usbaudio_stop();

        if (stopped == -ETIMEDOUT) {
            host.state = HOST_BROKEN;
            return stopped;
        }
        stopped = stopped != 0 ? stopped : stop_tasks();

        return stopped != 0 ? stopped : ret;
    }
    LOG_INF("host started");
    return 0;
}

int usbhost_stop(void)
{
    int ret = ready();

    if (ret != 0) {
        return ret;
    }
    /* The audio driver first: its client closes its device and deregisters. */
    ret = usbaudio_stop();
    if (ret == -ETIMEDOUT) {
        host.state = HOST_BROKEN;
        return ret;
    }
    if (ret != 0 && ret != -EALREADY) {
        /* The driver stays installed, the host running. */
        return ret;
    }
    ret = stop_tasks();
    if (ret == 0) {
        LOG_INF("host stopped");
    }
    return ret;
}

int usbhost_status(struct usbhost_status *status)
{
    usb_dwc_dev_t *hw = USB_DWC_LL_GET_HW(0);
    usb_host_lib_info_t info;
    uint32_t lines;

    memset(status, 0, sizeof(*status));
    if (ready() != 0) {
        status->broken = host.state == HOST_BROKEN;
        return 0;
    }
    status->running = true;
    /* Read only: the HCD owns the register (its change bits are write-1-to-clear). */
    status->powered = hw->hprt_reg.prtpwr != 0u;
    status->connected = usb_dwc_ll_hprt_get_conn_status(hw);
    status->enabled = usb_dwc_ll_hprt_get_port_en(hw);
    status->suspended = usb_dwc_ll_hprt_get_port_suspend(hw);
    status->overcurrent = usb_dwc_ll_hprt_get_port_overcur(hw);
    switch (usb_dwc_ll_hprt_get_speed(hw)) {
    case USB_DWC_SPEED_LOW:
        status->speed = USBHOST_SPEED_LOW;
        break;
    case USB_DWC_SPEED_HIGH:
        status->speed = USBHOST_SPEED_HIGH;
        break;
    default:
        status->speed = USBHOST_SPEED_FULL;
        break;
    }
    lines = usb_dwc_ll_hprt_get_pwr_line_status(hw);
    status->line_dp = (uint8_t) (lines & 1u);
    status->line_dm = (uint8_t) ((lines >> 1) & 1u);
    if (usb_host_lib_info(&info) == ESP_OK) {
        status->devices = info.num_devices;
        status->clients = info.num_clients;
    }
    return 0;
}

/* A request to the client task; its answer is host.result. */
static int request(enum host_request what)
{
    int ret;

    host.request = what;
    (void) usb_host_client_unblock(host.client);
    ret = wait_answer("usb_client");
    if (ret != 0) {
        return ret;
    }
    return host.result == ESP_OK ? 0 : host.result == ESP_ERR_NOT_FOUND ? -ENOENT : -EIO;
}

/*
 * The client task writes a request's results into storage of this file,
 * allocated for the request, never into the caller's buffers: the caller
 * gets a copy once the answer has come.  When the answer does not come in
 * time, the task may still write later, so the storage is left allocated
 * (it is not freed again; the host is BROKEN from then on and takes no
 * more requests).  The arguments the task reads are in host, written
 * before the request.
 */
int usbhost_list(struct usbhost_device *devices, int max)
{
    struct usbhost_device *result;
    int ret = ready();

    if (ret != 0) {
        return ret;
    }
    if (max <= 0) {
        return 0;
    }
    if (max > USBHOST_DEVICES_MAX) {
        max = USBHOST_DEVICES_MAX;
    }
    result = calloc((size_t) max, sizeof(*result));
    if (result == NULL) {
        return -ENOMEM;
    }
    host.devices = result;
    host.max = max;
    host.count = 0;
    ret = request(REQUEST_LIST);
    if (ret == -ETIMEDOUT) {
        return ret;                     /* result stays: see above */
    }
    if (ret == 0) {
        memcpy(devices, result, (size_t) host.count * sizeof(*result));
        ret = host.count;
    }
    free(result);
    return ret;
}

struct descriptors_result {
    struct usbhost_device device;
    uint8_t device_desc[USBHOST_DEVICE_DESC_LEN];
    uint8_t config[];
};

int usbhost_descriptors(uint8_t address, struct usbhost_device *device,
                        uint8_t device_desc[USBHOST_DEVICE_DESC_LEN],
                        uint8_t *config, size_t size, size_t *config_len)
{
    struct descriptors_result *result;
    int ret = ready();

    *config_len = 0;
    if (ret != 0) {
        return ret;
    }
    result = calloc(1, sizeof(*result) + size);
    if (result == NULL) {
        return -ENOMEM;
    }
    host.address = address;
    host.devices = &result->device;
    host.device_desc = result->device_desc;
    host.config = result->config;
    host.size = size;
    host.config_len = 0;
    ret = request(REQUEST_DESCRIPTORS);
    if (ret == -ETIMEDOUT) {
        return ret;                     /* result stays: see usbhost_list() */
    }
    if (ret == 0) {
        *device = result->device;
        memcpy(device_desc, result->device_desc, USBHOST_DEVICE_DESC_LEN);
        if (size > 0u) {
            memcpy(config, result->config, host.config_len < size ? host.config_len : size);
        }
        *config_len = host.config_len;
    }
    free(result);
    return ret;
}

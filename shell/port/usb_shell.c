/* SPDX-License-Identifier: MIT */
/*
 * "usb": the USB host of port/usb/host.h, written as a Zephyr shell command.
 *
 *   usb host start    take the USB-C connector from the console's USB
 *                     Serial/JTAG and start the host (the console stays on
 *                     UART0 and Telnet)
 *   usb host stop     stop it and give the connector back
 *   usb status        the host, its root port, the number of devices
 *   usb list          the enumerated devices, hubs included
 *   usb desc <addr>   a device's device descriptor and its active
 *                     configuration, one line per descriptor; class-specific
 *                     ones (an audio device's AudioControl and AudioStreaming
 *                     descriptors, for one) as their type, subtype and bytes
 *
 * The host's calls take one caller at a time: the command holds a lock
 * around each, for the shells on the console and on Telnet.
 */
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

#include "usb/host.h"
#include "usb/usb_types_ch9.h"

static K_MUTEX_DEFINE(usb_lock);

static const char *speed_name(enum usbhost_speed speed)
{
    switch (speed) {
    case USBHOST_SPEED_LOW:
        return "low";
    case USBHOST_SPEED_HIGH:
        return "high";
    default:
        return "full";
    }
}

static const char *class_name(uint8_t cls)
{
    switch (cls) {
    case USB_CLASS_PER_INTERFACE:
        return "per interface";
    case USB_CLASS_AUDIO:
        return "audio";
    case USB_CLASS_COMM:
        return "communications";
    case USB_CLASS_HID:
        return "HID";
    case USB_CLASS_MASS_STORAGE:
        return "mass storage";
    case USB_CLASS_HUB:
        return "hub";
    case USB_CLASS_CDC_DATA:
        return "CDC data";
    case USB_CLASS_VIDEO:
        return "video";
    case USB_CLASS_MISC:
        return "miscellaneous";
    case USB_CLASS_APP_SPEC:
        return "application specific";
    case USB_CLASS_VENDOR_SPEC:
        return "vendor specific";
    default:
        return "other";
    }
}

static int report(const struct shell *sh, int ret)
{
    switch (ret) {
    case 0:
        break;
    case -EALREADY:
        shell_error(sh, "The USB host is not running");
        break;
    case -ENOENT:
        shell_error(sh, "No such device");
        break;
    case -ENOMEM:
        shell_error(sh, "Out of memory");
        break;
    case -ETIMEDOUT:
        shell_error(sh, "The USB host did not answer; it stays as it is until a reboot");
        break;
    default:
        shell_error(sh, "Failed (%d); \"dmesg\" shows why", ret);
        break;
    }
    return ret;
}

/* ---- usb host ------------------------------------------------------------ */

static int cmd_usb_host_start(const struct shell *sh, size_t argc, char **argv)
{
    int ret;

    (void) argc;
    (void) argv;

    k_mutex_lock(&usb_lock, K_FOREVER);
    ret = usbhost_start();
    k_mutex_unlock(&usb_lock);
    if (ret == -EALREADY) {
        shell_error(sh, "The USB host is already running");
        return ret;
    }
    if (ret == 0) {
        shell_print(sh, "USB host started; the console is on UART0 and Telnet only");
    }
    return report(sh, ret);
}

static int cmd_usb_host_stop(const struct shell *sh, size_t argc, char **argv)
{
    int ret;

    (void) argc;
    (void) argv;

    k_mutex_lock(&usb_lock, K_FOREVER);
    ret = usbhost_stop();
    k_mutex_unlock(&usb_lock);
    if (ret == 0) {
        shell_print(sh, "USB host stopped; USB-C is the console's again");
    }
    return report(sh, ret);
}

/* ---- usb status ---------------------------------------------------------- */

static int cmd_usb_status(const struct shell *sh, size_t argc, char **argv)
{
    struct usbhost_status st;

    (void) argc;
    (void) argv;

    k_mutex_lock(&usb_lock, K_FOREVER);
    (void) usbhost_status(&st);
    k_mutex_unlock(&usb_lock);

    if (st.broken) {
        shell_print(sh, "host: not answering; it stays as it is until a reboot");
        return 0;
    }
    if (!st.running) {
        shell_print(sh, "host: stopped");
        return 0;
    }
    shell_print(sh, "host: running");
    shell_print(sh, "root port: power %s, %s, %s%s%s%s, D+ %u D- %u", st.powered ? "on" : "off",
                st.connected ? "connected" : "not connected", st.enabled ? "enabled" : "disabled",
                st.enabled ? (st.speed == USBHOST_SPEED_LOW ? ", low speed" : ", full speed") : "",
                st.suspended ? ", suspended" : "", st.overcurrent ? ", over-current" : "",
                st.line_dp, st.line_dm);
    shell_print(sh, "devices: %d, clients: %d", st.devices, st.clients);
    return 0;
}

/* ---- usb list ------------------------------------------------------------ */

static void print_class(char *buf, size_t size, const struct usbhost_device *d)
{
    int n = snprintf(buf, size, "%02x/%02x/%02x", d->device_class, d->device_subclass,
                     d->device_protocol);

    if (d->device_class == USB_CLASS_PER_INTERFACE && d->interface_count > 0u) {
        for (uint8_t i = 0; i < d->interface_count && n > 0 && (size_t) n < size; i++) {
            n += snprintf(buf + n, size - (size_t) n, "%s%02x", i == 0u ? " (" : ",",
                          d->interface_classes[i]);
        }
        if (n > 0 && (size_t) n < size) {
            (void) snprintf(buf + n, size - (size_t) n, ")");
        }
    }
}

static int cmd_usb_list(const struct shell *sh, size_t argc, char **argv)
{
    struct usbhost_device *devices = malloc(USBHOST_DEVICES_MAX * sizeof(*devices));
    int n;

    (void) argc;
    (void) argv;

    if (devices == NULL) {
        return report(sh, -ENOMEM);
    }
    k_mutex_lock(&usb_lock, K_FOREVER);
    n = usbhost_list(devices, USBHOST_DEVICES_MAX);
    k_mutex_unlock(&usb_lock);
    if (n < 0) {
        free(devices);
        return report(sh, n);
    }
    if (n == 0) {
        shell_print(sh, "No devices");
    } else {
        shell_print(sh, "%4s %-5s %-6s %-9s %-24s %s", "ADDR", "SPEED", "PARENT", "VID:PID",
                    "CLASS", "MANUFACTURER, PRODUCT");
    }
    for (int i = 0; i < n; i++) {
        const struct usbhost_device *d = &devices[i];
        char parent[8];
        char cls[32];

        if (d->parent_address == 0u) {
            (void) snprintf(parent, sizeof(parent), "root");
        } else {
            (void) snprintf(parent, sizeof(parent), "%u.%u", d->parent_address, d->parent_port);
        }
        print_class(cls, sizeof(cls), d);
        shell_print(sh, "%4u %-5s %-6s %04x:%04x %-24s \"%s\", \"%s\"", d->address,
                    speed_name(d->speed), parent, d->vid, d->pid, cls, d->manufacturer,
                    d->product);
    }
    free(devices);
    return 0;
}

/* ---- usb desc ------------------------------------------------------------ */

static const char *const ep_types[] = {"control", "isochronous", "bulk", "interrupt"};
static const char *const ep_syncs[] = {"no sync", "async", "adaptive", "sync"};
static const char *const ep_usages[] = {"data", "feedback", "implicit feedback", "reserved"};

/* A descriptor's bytes from offset from on, in hex, on lines of 16. */
static void print_bytes(const struct shell *sh, const char *indent, const uint8_t *p, size_t from,
                        size_t len)
{
    char line[16 * 3 + 1];

    for (size_t off = from; off < len; off += 16u) {
        size_t n = 0;

        for (size_t i = off; i < len && i < off + 16u; i++) {
            n += (size_t) snprintf(line + n, sizeof(line) - n, "%s%02x", i == off ? "" : " ", p[i]);
        }
        shell_print(sh, "%s%s", indent, line);
    }
}

static void print_bcd(char *buf, size_t size, uint16_t bcd)
{
    (void) snprintf(buf, size, "%x.%02x", bcd >> 8, bcd & 0xffu);
}

static void print_device(const struct shell *sh, const struct usbhost_device *d,
                         const usb_device_desc_t *dd)
{
    char usb[8];
    char dev[8];

    print_bcd(usb, sizeof(usb), dd->bcdUSB);
    print_bcd(dev, sizeof(dev), dd->bcdDevice);
    shell_print(sh, "device %u: %s speed, parent %u port %u, \"%s\", \"%s\", serial \"%s\"",
                d->address, speed_name(d->speed), d->parent_address, d->parent_port,
                d->manufacturer, d->product, d->serial);
    shell_print(sh, "device descriptor: bcdUSB %s, class %02x/%02x/%02x (%s), "
                "bMaxPacketSize0 %u, %04x:%04x, bcdDevice %s, iManufacturer %u, iProduct %u, "
                "iSerialNumber %u, bNumConfigurations %u",
                usb, dd->bDeviceClass, dd->bDeviceSubClass, dd->bDeviceProtocol,
                class_name(dd->bDeviceClass), dd->bMaxPacketSize0, dd->idVendor, dd->idProduct, dev,
                dd->iManufacturer, dd->iProduct, dd->iSerialNumber, dd->bNumConfigurations);
}

/* One descriptor of the configuration, p[0] bytes, at least 2. */
static void print_one(const struct shell *sh, const uint8_t *p)
{
    uint8_t len = p[0];

    switch (p[1]) {
    case USB_B_DESCRIPTOR_TYPE_CONFIGURATION:
        if (len >= USB_CONFIG_DESC_SIZE) {
            const usb_config_desc_t *c = (const usb_config_desc_t *) p;

            shell_print(sh, "configuration %u: wTotalLength %u, bNumInterfaces %u, "
                        "iConfiguration %u, bmAttributes 0x%02x (%s%s), bMaxPower %u mA",
                        c->bConfigurationValue, c->wTotalLength, c->bNumInterfaces,
                        c->iConfiguration, c->bmAttributes,
                        (c->bmAttributes & USB_BM_ATTRIBUTES_SELFPOWER) ? "self-powered" : "bus-powered",
                        (c->bmAttributes & USB_BM_ATTRIBUTES_WAKEUP) ? ", remote wakeup" : "",
                        c->bMaxPower * 2u);
            return;
        }
        break;
    case USB_B_DESCRIPTOR_TYPE_INTERFACE_ASSOCIATION:
        if (len >= USB_IAD_DESC_SIZE) {
            const usb_iad_desc_t *a = (const usb_iad_desc_t *) p;

            shell_print(sh, "  association: interfaces %u..%u, class %02x/%02x/%02x (%s), "
                        "iFunction %u",
                        a->bFirstInterface, a->bFirstInterface + a->bInterfaceCount - 1u,
                        a->bFunctionClass, a->bFunctionSubClass, a->bFunctionProtocol,
                        class_name(a->bFunctionClass), a->iFunction);
            return;
        }
        break;
    case USB_B_DESCRIPTOR_TYPE_INTERFACE:
        if (len >= USB_INTF_DESC_SIZE) {
            const usb_intf_desc_t *i = (const usb_intf_desc_t *) p;

            shell_print(sh, "  interface %u alt %u: bNumEndpoints %u, class %02x/%02x/%02x (%s), "
                        "iInterface %u",
                        i->bInterfaceNumber, i->bAlternateSetting, i->bNumEndpoints,
                        i->bInterfaceClass, i->bInterfaceSubClass, i->bInterfaceProtocol,
                        class_name(i->bInterfaceClass), i->iInterface);
            return;
        }
        break;
    case USB_B_DESCRIPTOR_TYPE_ENDPOINT:
        if (len >= USB_EP_DESC_SIZE) {
            const usb_ep_desc_t *e = (const usb_ep_desc_t *) p;
            unsigned int type = e->bmAttributes & USB_BM_ATTRIBUTES_XFERTYPE_MASK;
            char iso[40] = "";

            if (type == USB_BM_ATTRIBUTES_XFER_ISOC) {
                (void) snprintf(iso, sizeof(iso), " %s %s", ep_syncs[USB_EP_DESC_GET_SYNCTYPE(e)],
                                ep_usages[USB_EP_DESC_GET_USAGETYPE(e)]);
            }
            shell_print(sh, "    endpoint 0x%02x (%u %s): %s%s, wMaxPacketSize %u (x%u), bInterval %u",
                        e->bEndpointAddress, USB_EP_DESC_GET_EP_NUM(e),
                        USB_EP_DESC_GET_EP_DIR(e) ? "IN" : "OUT", ep_types[type], iso,
                        USB_EP_DESC_GET_MPS(e), USB_EP_DESC_GET_MULT(e) + 1u, e->bInterval);
            if (len > USB_EP_DESC_SIZE) {
                /* The audio class's endpoints: bRefresh, bSynchAddress. */
                shell_print(sh, "      and %u more bytes:", len - USB_EP_DESC_SIZE);
                print_bytes(sh, "        ", p, USB_EP_DESC_SIZE, len);
            }
            return;
        }
        break;
    case 0x24: /* CS_INTERFACE */
    case 0x25: /* CS_ENDPOINT */
        if (len >= 3u) {
            shell_print(sh, "    class-specific %s 0x%02x, subtype 0x%02x, %u bytes:",
                        p[1] == 0x24 ? "interface" : "endpoint", p[1], p[2], len);
            print_bytes(sh, "      ", p, 0, len);
            return;
        }
        break;
    default:
        break;
    }
    shell_print(sh, "    descriptor 0x%02x, %u bytes:", p[1], len);
    print_bytes(sh, "      ", p, 0, len);
}

static int cmd_usb_desc(const struct shell *sh, size_t argc, char **argv)
{
    struct usbhost_device d;
    uint8_t dd[USBHOST_DEVICE_DESC_LEN];
    uint8_t *config = NULL;
    size_t len = 0;
    size_t total = 0;
    char *end;
    unsigned long addr;
    int ret;

    (void) argc;
    addr = strtoul(argv[1], &end, 0);
    if (*argv[1] == '\0' || *end != '\0' || addr < 1u || addr > 127u) {
        shell_error(sh, "Not a device address (1 .. 127): %s", argv[1]);
        return -EINVAL;
    }

    k_mutex_lock(&usb_lock, K_FOREVER);
    /* The length first, then the whole configuration. */
    ret = usbhost_descriptors((uint8_t) addr, &d, dd, NULL, 0, &len);
    if (ret == 0 && len > 0u) {
        config = malloc(len);
        if (config == NULL) {
            ret = -ENOMEM;
        } else {
            ret = usbhost_descriptors((uint8_t) addr, &d, dd, config, len, &total);
            if (ret == 0 && total < len) {
                len = total;
            }
        }
    }
    k_mutex_unlock(&usb_lock);
    if (ret != 0) {
        free(config);
        return report(sh, ret);
    }

    print_device(sh, &d, (const usb_device_desc_t *) dd);
    for (size_t off = 0; off + 2u <= len;) {
        uint8_t n = config[off];

        if (n < 2u || off + n > len) {
            shell_print(sh, "    bad descriptor length %u at offset %u:", n, (unsigned int) off);
            print_bytes(sh, "      ", config, off, len);
            break;
        }
        print_one(sh, &config[off]);
        off += n;
    }
    free(config);
    return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_usb_host,
    SHELL_CMD(start, NULL, "Start the USB host on USB-C (the console stays on UART0 and Telnet)",
              cmd_usb_host_start),
    SHELL_CMD(stop, NULL, "Stop the USB host and give USB-C back to the console", cmd_usb_host_stop),
    SHELL_SUBCMD_SET_END);

SHELL_STATIC_SUBCMD_SET_CREATE(sub_usb,
    SHELL_CMD_ARG(desc, NULL, "Show a device's descriptors: desc <address>", cmd_usb_desc, 2, 0),
    SHELL_CMD(host, &sub_usb_host, "Start or stop the USB host", NULL),
    SHELL_CMD(list, NULL, "List the USB devices", cmd_usb_list),
    SHELL_CMD(status, NULL, "Show the USB host and its root port", cmd_usb_status),
    SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(usb, &sub_usb, "USB host", NULL);

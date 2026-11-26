/*
 * USB host on the XIAO ESP32S3 (port/usb/host.c): esp-usb's USB Host
 * Library on the USB OTG controller, full speed, through the internal USB
 * PHY on the USB-C connector, with external hubs.  This header does not
 * include esp-usb's or ESP-IDF's: its users (the shell's "usb" command,
 * shell/port) see only these types.
 *
 * Not reentrant: one caller at a time, from a thread (the "usb" command
 * holds a lock around each call).
 */
#ifndef PORT_USB_HOST_H
#define PORT_USB_HOST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Takes the internal USB PHY from the USB Serial/JTAG controller (the
 * console on USB-C goes silent, port/board.c: console_usb_detach()), gives
 * it to USB OTG in host mode and starts the library with the root port
 * powered, its event task "usb_host", the USB audio input (port/usb/
 * audio.h: usbaudio_start()) and the client task "usb_client", which logs
 * devices as they come and go.  0, -EALREADY when it runs,
 * -ENOMEM, -EIO when the library or its client did not start (logged;
 * the PHY is back with USB Serial/JTAG then), -ETIMEDOUT when a task did
 * not answer (the host is then left as it is, and every later call
 * returns -EIO).
 */
int usbhost_start(void);

/*
 * Ends what usbhost_start() started, in the library's order: the audio
 * input stops (usbaudio_stop(); when it fails, the host stays as it is and
 * its error is returned), the client deregisters, every device is freed, the library is uninstalled (root
 * port off, PHY deleted), the PHY goes back to USB Serial/JTAG and the
 * console's USB transport with it.  0, -EALREADY when it does not run,
 * -ETIMEDOUT (see above), -EIO.
 */
int usbhost_stop(void);

/* The speeds, numbered as esp-usb's usb_speed_t. */
enum usbhost_speed {
    USBHOST_SPEED_LOW,
    USBHOST_SPEED_FULL,
    USBHOST_SPEED_HIGH,
};

/*
 * The root port as the controller's port register shows it, and the
 * library's counts.  Only running and broken are meaningful while the host
 * does not run.
 */
struct usbhost_status {
    bool running;
    bool broken;                /* not running: a task did not answer (see above) */
    bool powered;               /* port power on */
    bool connected;             /* a device is attached */
    bool enabled;               /* reset done, the port is in use */
    bool suspended;
    bool overcurrent;
    enum usbhost_speed speed;   /* of the attached device, once enabled */
    uint8_t line_dp;            /* the levels of D+ and D- (0 or 1) */
    uint8_t line_dm;
    int devices;                /* enumerated devices, hubs included */
    int clients;                /* registered clients (this port's one) */
};

/* Always 0. */
int usbhost_status(struct usbhost_status *status);

#define USBHOST_DEVICES_MAX     8
#define USBHOST_STRING_MAX      32

/* An enumerated device. */
struct usbhost_device {
    uint8_t address;
    enum usbhost_speed speed;
    uint8_t parent_address;     /* 0: on the root port */
    uint8_t parent_port;        /* the hub's port, 1 .. */
    uint16_t vid;
    uint16_t pid;
    uint8_t device_class;       /* bDeviceClass, bDeviceSubClass, bDeviceProtocol */
    uint8_t device_subclass;
    uint8_t device_protocol;
    uint8_t interface_classes[4]; /* bInterfaceClass of alternate setting 0, in order, */
    uint8_t interface_count;    /* up to 4 of them */
    uint8_t configuration;      /* bConfigurationValue, 0 when not configured */
    /* The string descriptors in ASCII ('?' for any other character), "" when absent. */
    char manufacturer[USBHOST_STRING_MAX];
    char product[USBHOST_STRING_MAX];
    char serial[USBHOST_STRING_MAX];
};

/*
 * The enumerated devices, up to max of them, in the library's order.  The
 * number listed, or -EALREADY (not running), -ETIMEDOUT, -EIO.
 */
int usbhost_list(struct usbhost_device *devices, int max);

#define USBHOST_DEVICE_DESC_LEN 18

/*
 * The device at address: its entry as in usbhost_list(), its device
 * descriptor, and up to size bytes of its active configuration
 * descriptor, everything that wTotalLength covers (*config_len is
 * wTotalLength; when it is larger than size, only size bytes were
 * copied).  0, -ENOENT (no such device), -EALREADY, -ETIMEDOUT, -EIO.
 */
int usbhost_descriptors(uint8_t address, struct usbhost_device *device,
                        uint8_t device_desc[USBHOST_DEVICE_DESC_LEN],
                        uint8_t *config, size_t size, size_t *config_len);

#endif

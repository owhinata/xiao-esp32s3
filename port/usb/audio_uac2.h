/*
 * USB Audio Class 2.0 capture (port/usb/audio_uac2.c), the part of the USB
 * audio input (port/usb/audio.c) that esp-usb's usb_host_uac does not
 * have: a client of the USB Host Library of its own that finds UAC 2.0
 * microphones, sets their clock and streams their isochronous IN endpoint
 * into a byte ring buffer, which audio.c drains with uac2_read() into the
 * same pipeline as the UAC 1.0 driver's.  Private to audio.c.
 *
 * Every function but the callbacks runs in audio.c's task "uac"; the
 * callbacks run in this file's client task "uac2" and must not block.
 */
#ifndef PORT_USB_AUDIO_UAC2_H
#define PORT_USB_AUDIO_UAC2_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "usb/audio.h"

struct uac2_callbacks {
    /* A device with a UAC 2.0 capture interface enumerated. */
    void (*connected)(uint8_t address, uint8_t interface);
    /* The open device went. */
    void (*gone)(void);
    /* The ring buffer holds at least the threshold that uac2_start() was given. */
    void (*data)(void);
};

/* Registers the client and starts its task.  0, -ENOMEM, -EIO. */
int uac2_install(const struct uac2_callbacks *callbacks);

/*
 * Closes the device (a failed one by powering the root port off, which
 * makes the library end its last request), ends the task and deregisters
 * the client.  0, -EIO, -ETIMEDOUT; on an error the client and its task
 * stay, and a later call tries again.
 */
int uac2_uninstall(void);

/*
 * Opens the device at address when it is a UAC 2.0 one with a capture
 * interface that this file can stream, reads its clock's rates and its
 * volume range, and fills info.  0; -ENOTSUP when it is not a UAC 2.0
 * device, and its configuration is safe for usb_host_uac to open
 * streaming interface interface; -EINVAL when it is not (logged); -EIO
 * (logged: what in the descriptors or the requests failed); -ENOMEM.  The
 * device is closed again on every error.
 */
int uac2_open(uint8_t address, uint8_t interface, struct usbaudio_info *info);

/*
 * Stops the stream if it runs and closes the device.  0, -EBUSY or -EIO
 * when something could not be released yet (the device stays open; a
 * later call, when the device has gone or from uac2_uninstall(), goes on
 * from there).
 */
int uac2_close(void);

/*
 * The open device did not answer a control request in time: no request is
 * sent to it any more (every call that would send one returns -EIO at
 * once) until it is closed.
 */
bool uac2_failed(void);

/*
 * Sets the clock to rate, selects the alternate setting with channels
 * channels of 16 bits, and starts the isochronous IN transfers into a
 * ring buffer of buffer_bytes; data() comes when it holds threshold.
 * 0, -ENOTSUP (no such alternate setting or rate), -ENOMEM, -EIO.
 */
int uac2_start(uint32_t rate, uint8_t channels, uint32_t buffer_bytes, uint32_t threshold);

/*
 * The packet a 1 ms frame of the stream needs: exact for a synchronous or
 * adaptive endpoint at a rate of whole frames per millisecond, one frame
 * more otherwise.  uac2_start() takes the first alternate setting of the
 * format whose wMaxPacketSize holds it.
 */
uint32_t uac2_packet_need(uint32_t rate, uint8_t channels, uint8_t subslot, uint8_t sync);

/* Stops the transfers, selects alternate setting 0, frees the ring buffer.  0, -EIO. */
int uac2_stop(void);

/* Up to size bytes of the ring buffer, without waiting.  0, -EAGAIN when it is empty. */
int uac2_read(uint8_t *buf, size_t size, size_t *got);

/* The volume (1/256 dB, and percent of its range) and the mute.  0, -ENOTSUP, -EIO. */
int uac2_volume_get(int16_t *volume, uint8_t *percent);
int uac2_volume_set(uint8_t percent);
int uac2_mute_get(bool *mute);
int uac2_mute_set(bool mute);

/* The stream's counters since uac2_start(). */
struct uac2_counters {
    uint32_t overruns;          /* packets dropped: the ring buffer was full */
    uint32_t packet_errors;     /* packets with an error status */
    uint32_t transfer_errors;   /* transfers that failed or could not be resubmitted */
    uint32_t wakeups;           /* data() calls */
};

void uac2_counters(struct uac2_counters *counters);

#endif

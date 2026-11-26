/*
 * USB audio input (port/usb/audio.c): a USB Audio Class 1.0 microphone
 * through esp-usb's usb_host_uac driver, on the USB host of port/usb/host.h.
 * The stream's PCM samples (16 bits, one or two channels) are mixed to one
 * channel, decimated by 3, to 16 kHz from a 48 kHz stream, and freed of
 * their DC offset (a 30 Hz high-pass), into a ring
 * that a consumer reads with usbaudio_read_16k().  This header does not
 * include esp-usb's or ESP-IDF's: its users see only these types.
 *
 * The functions are for threads.  The requests (usbaudio_info(),
 * usbaudio_stream_start(), usbaudio_stream_stop()) take one caller at a
 * time and wait for the task "uac", which makes every call of the driver.
 */
#ifndef PORT_USB_AUDIO_H
#define PORT_USB_AUDIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Called by usbhost_start() once the USB Host Library runs, before any
 * device can have enumerated (the driver's client sees only the devices
 * that come after it registers): creates the task "uac", which installs
 * the driver (with its event task "USB UAC Host") and opens the first
 * microphone interface that connects.  0, -ENOMEM, -EIO (logged),
 * -ETIMEDOUT.
 */
int usbaudio_start(void);

/*
 * Called by usbhost_stop() before the library goes: stops the stream,
 * closes the interface, uninstalls the driver, ends the task.  0,
 * -EALREADY when not started, -EIO (logged; the driver stays installed),
 * -ETIMEDOUT (the task did not answer; every later call returns -EIO).
 */
int usbaudio_stop(void);

#define USBAUDIO_ALTS_MAX       4
#define USBAUDIO_RATES_MAX      4
#define USBAUDIO_STRING_MAX     32

/* An alternate setting of the microphone's streaming interface. */
struct usbaudio_alt {
    uint8_t channels;
    uint8_t bits;               /* bBitResolution */
    uint8_t subframe;           /* bSubframeSize, bytes per sample */
    uint8_t rate_count;         /* discrete rates in rates[], 0: the range */
    uint32_t rates[USBAUDIO_RATES_MAX];
    uint32_t rate_min;          /* the continuous range, when rate_count is 0 */
    uint32_t rate_max;
};

struct usbaudio_info {
    bool open;                  /* a microphone interface is open */
    uint8_t address;            /* its device */
    uint8_t interface;          /* its AudioStreaming interface */
    uint16_t vid;
    uint16_t pid;
    char manufacturer[USBAUDIO_STRING_MAX]; /* ASCII, '?' for other characters */
    char product[USBAUDIO_STRING_MAX];
    uint8_t alt_count;          /* alternate settings with an endpoint (1 ..) */
    struct usbaudio_alt alts[USBAUDIO_ALTS_MAX];
    bool streaming;
    /* While streaming, from the device (GET_CUR of its feature unit). */
    bool volume_valid;
    int16_t volume;             /* 1/256 dB */
    bool mute_valid;
    bool mute;
};

/* 0, -EALREADY (the host does not run), -ETIMEDOUT, -EIO. */
int usbaudio_info(struct usbaudio_info *info);

/*
 * Starts the stream at rate Hz with channels channels (1 or 2) of 16
 * bits, the driver choosing the alternate setting that has them.  0,
 * -ENODEV (no microphone), -EBUSY (streaming), -EINVAL, -ENOTSUP (no
 * alternate setting has them), -EALREADY, -ETIMEDOUT, -EIO.
 */
int usbaudio_stream_start(uint32_t rate, uint8_t channels);

/* 0, -ENODEV, -ESRCH (not streaming), -EALREADY, -ETIMEDOUT, -EIO. */
int usbaudio_stream_stop(void);

/*
 * The microphone's volume and mute.  The driver reads and sets them only
 * while the stream runs.  What was set last is kept for the session, in
 * RAM, and set again at every stream start; until one is set the device
 * keeps its own default.
 */
struct usbaudio_volume {
    bool percent_valid;
    uint8_t percent;            /* 0 .. 100 of the device's range (uac_host_device_get_volume()) */
    bool db_valid;
    int16_t db;                 /* 1/256 dB */
    bool mute_valid;
    bool mute;
    bool set;                   /* a volume was set in this session: */
    uint8_t set_percent;        /* that one */
};

/* 0, -ENODEV (no microphone), -ESRCH (not streaming), -EALREADY, -ETIMEDOUT, -EIO. */
int usbaudio_volume_get(struct usbaudio_volume *volume);

/*
 * Sets the volume, percent 0 .. 100 of the device's range (rounded by the
 * driver to the device's resolution), or the mute.  0, -EINVAL,
 * -ENOTSUP (the device has no such control), -ENODEV, -ESRCH, -EALREADY,
 * -ETIMEDOUT, -EIO.
 */
int usbaudio_volume_set(uint8_t percent);
int usbaudio_mute_set(bool mute);

/* What the stream has delivered since it started; kept after it stops. */
struct usbaudio_stats {
    bool streaming;
    uint32_t rate;              /* of the stream, Hz */
    uint8_t channels;
    uint32_t out_rate;          /* of the decimated samples, rate / 3 */
    uint32_t buffer_bytes;      /* the driver's ring buffer */
    uint64_t bytes;             /* PCM bytes read from the driver */
    uint64_t frames;            /* audio frames (a sample of every channel) */
    uint32_t first_frames;      /* of them, in the first read that returned data */
    uint64_t out_samples;       /* decimated samples */
    bool dc_valid;              /* a second of input has come */
    int32_t dc;                 /* the mean of the raw input samples (the channels' mix) over
                                   the last whole second, LSB */
    uint64_t clipped;           /* input samples (of every channel) at full scale, -32768 or 32767 */
    uint32_t reads;             /* uac_host_device_read() calls */
    uint32_t short_reads;       /* returned less than asked: the buffer emptied */
    uint32_t empty_reads;       /* returned nothing */
    uint32_t read_errors;
    uint32_t wakeups;           /* the driver's RX_DONE events (its buffer passed the threshold) */
    uint32_t near_full;         /* drains that found the driver's buffer within one
                                   transfer of full, where it drops a transfer's data */
    uint32_t transfer_errors;   /* the driver's TRANSFER_ERROR events */
    uint32_t ring_lost;         /* decimated samples overwritten before the consumer read them */
    int64_t start_us;           /* esp_timer_get_time() of the start */
    int64_t first_us;           /* of the first and the last read that returned data, 0: none */
    int64_t last_us;
};

/* Always 0. */
int usbaudio_stats(struct usbaudio_stats *stats);

/*
 * The level of the decimated samples over the last complete window of
 * about 100 ms (out_rate / 10 samples).  seq counts the windows since the
 * stream started (0: none yet).
 */
struct usbaudio_level {
    uint32_t seq;
    uint32_t samples;
    uint32_t peak;              /* largest magnitude, 0 .. 32768 */
    uint64_t sum_squares;       /* of the samples */
};

/* Always 0. */
int usbaudio_level(struct usbaudio_level *level);

/*
 * The next n decimated samples of a 48 kHz stream (16 kHz, one channel,
 * the mix of the stream's channels), waiting up to timeout_ms for them.
 * For one consumer: the ring keeps the latest USBAUDIO_RING_SAMPLES; a
 * consumer that falls further behind loses the oldest (counted in
 * ring_lost) and goes on from the oldest kept.  A stream start empties the
 * ring.  The number of samples copied (fewer than n on the timeout), or
 * -ENODATA when no stream runs, -ENOTSUP when it runs at another rate.
 */
#define USBAUDIO_RING_SAMPLES   4096u

int usbaudio_read_16k(int16_t *dst, size_t n, uint32_t timeout_ms);

#endif

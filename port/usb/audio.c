/*
 * USB audio input on the USB host of port/usb/host.c, for a microphone of
 * either USB Audio Class: a UAC 1.0 one through esp-usb's driver (lib/
 * esp-usb, host/class/uac/usb_host_uac, unmodified), a UAC 2.0 one through
 * port/usb/audio_uac2.c (usb_host_uac refuses UAC 2.0).  Both are clients
 * of the USB Host Library that fill a byte ring buffer (ESP-IDF's API,
 * port/esp_compat/freertos/ringbuf.c) from their isochronous IN transfers;
 * what comes after, the same for both, is here.
 *
 * Three FreeRTOS tasks, created when the host starts and ending when it
 * stops (their stacks go back to the FreeRTOS layer's pool):
 *
 *   "USB UAC Host"  usb_host_uac's own, which runs its client's events: the
 *                   devices that come and go, the isochronous IN transfers
 *                   (their completion copies the packets into the driver's
 *                   ring buffer and resubmits them) and the callbacks
 *                   below.  It must resubmit a transfer before the two
 *                   still queued are done (6 ms with three transfers of
 *                   three 1 ms packets), so it runs above lwIP's "tcpip"
 *                   (shell/port/priorities_os.h).
 *   "uac2"          audio_uac2.c's client task, the same for a UAC 2.0
 *                   device, at the same priority.
 *   "uac"           this file's, which makes every other call of either,
 *                   so that none of them races another: it opens the first
 *                   microphone that either reports (a UAC 2.0 one with
 *                   audio_uac2.c, any other with usb_host_uac), closes it
 *                   when the device goes, serves the requests of
 *                   usbaudio_info(), usbaudio_stream_start(),
 *                   usbaudio_stream_stop(), the volume and mute and
 *                   usbaudio_stop(), and reads the samples: woken when the
 *                   buffer passed a quarter, it drains it without waiting,
 *                   mixes the channels to one, decimates by 3 (not a 16 kHz
 *                   stream), removes the DC (a 30 Hz high-pass) and the
 *                   mains hum (notches), keeps the level and the ring of
 *                   usbaudio_read_16k().
 *
 * The callbacks run in the clients' tasks and must not block: they only
 * count, note and wake "uac".  Neither client can be called from its own
 * task for anything that sends a control transfer: the transfer completes
 * in that same task.
 *
 * When a UAC 1.0 device goes, the driver stops the stream and releases the
 * interface in its task, then reports DISCONNECTED; for a UAC 2.0 one
 * audio_uac2.c reports DEV_GONE and its transfers end.  "uac" then closes
 * the microphone, which frees its buffer.  Reading and closing happen in
 * the same task, so no read is under way when the buffer goes.
 *
 * No interrupt handler and no xTimer is involved; the C library is used
 * from the tasks only.
 */
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

#include "tx_api.h"

#include "esp_err.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "usb/uac_host.h"

#include "app_log.h"
#include "priorities.h"
#include "usb/audio.h"
#include "usb/audio_uac2.h"

LOG_MODULE_REGISTER(usbaudio, LOG_LEVEL_INF);

/*
 * The driver's ring buffer: 40 ms of the largest stream this file takes,
 * 48 kHz, two channels of 16 bits (192 bytes per ms); 80 ms of a mono one.
 * RX_DONE comes once it holds a quarter (the driver's default threshold).
 * Allocated when the interface opens (the stream's format is not known
 * yet), freed when it closes.
 */
#define RX_BUFFER_BYTES         (40u * 192u)
#define RX_THRESHOLD_BYTES      (RX_BUFFER_BYTES / 4u)

/* One uac_host_device_read(): a multiple of every frame size (2, 4). */
#define RX_CHUNK                512u

/* How long a caller waits for the task: a start sends two control transfers of up to 5 s each. */
#define AUDIO_ANSWER_TIMEOUT_MS 15000u

/* Events of the task. */
#define EV_CONNECTED            0x1u    /* the driver found a microphone interface */
#define EV_DISCONNECTED         0x2u    /* the open interface's device went */
#define EV_DATA                 0x4u    /* RX_DONE */
#define EV_REQUEST              0x8u
#define EV_ALL                  (EV_CONNECTED | EV_DISCONNECTED | EV_DATA | EV_REQUEST)

/* The 16 kHz ring's event for its consumer. */
#define RING_EV_DATA            0x1u
#define RING_MASK               (USBAUDIO_RING_SAMPLES - 1u)

#if (USBAUDIO_RING_SAMPLES & RING_MASK) != 0
#error "USBAUDIO_RING_SAMPLES must be a power of 2"
#endif

_Static_assert(USBAUDIO_RATES_MAX <= UAC_FREQ_NUM_MAX, "the driver keeps fewer rates");

enum audio_request {
    REQUEST_NONE,
    REQUEST_INFO,
    REQUEST_START,
    REQUEST_STOP,
    REQUEST_VOLUME_GET,
    REQUEST_VOLUME_SET,
    REQUEST_MUTE_SET,
    REQUEST_HUM_SET,
    REQUEST_QUIT,
};

/* A request's arguments; the results go where the pointers point. */
struct request_args {
    uint32_t rate;
    uint8_t channels;
    uint8_t percent;
    bool mute;
    uint8_t hum_hz;
    struct usbaudio_info *info;
    struct usbaudio_volume *volume;
};

enum audio_state {
    AUDIO_STOPPED,
    AUDIO_RUNNING,
    AUDIO_BROKEN,               /* the task did not answer */
};

/*
 * Anti-aliasing low-pass filter of the decimation by 3: a 51-tap FIR,
 * the windowed sinc of a cut-off at 1/6 of the input rate (8 kHz of 48 kHz,
 * the output's Nyquist frequency) in a Kaiser window (beta 5.0), in Q15,
 * rounded to a DC gain of exactly 1 (the taps sum to 32768).  At 48 kHz:
 * within 0.03 dB to 6.4 kHz, -6 dB at 8 kHz, 55 dB down from 9.6 kHz on, so
 * what folds into 0 .. 6.4 kHz is at least 55 dB down.  Symmetric, and
 * every third tap but the centre is 0 (the sinc's zeros), so an output
 * sample takes 18 multiplications of a sum of two inputs.  The sum of the
 * magnitudes is 54284 (1.66 in Q15): with 16-bit inputs the sum stays
 * within 32 bits.  The first half and the centre tap.
 */
#define FIR_TAPS                51
#define FIR_CENTRE              25

static const int16_t fir_half[FIR_CENTRE + 1] = {
    13, 0, -32, -45, 0, 81, 105, 0, -166, -205, 0, 303, 365,
    0, -523, -624, 0, 894, 1080, 0, -1651, -2133, 0, 4454, 9004, 10928,
};

struct decimator {
    int16_t hist[2 * FIR_TAPS]; /* the last FIR_TAPS inputs, twice: a window never wraps */
    uint8_t pos;                /* where the next input goes */
    uint8_t phase;              /* inputs since the last output */
};

/*
 * DC blocker after the decimation, at the output rate (the FIR passes DC
 * with a gain of exactly 1, so the order makes no difference to the
 * result, and here it runs at a third of the rate): y[n] = x[n] - x[n-1] +
 * a y[n-1], a = 32382 / 32768 = 0.98822, a corner at (1 - a) 16000 / 2 pi
 * = 30 Hz at 16 kHz output (rate / 3 * 0.001875 at another rate).  y is
 * kept with 15 fraction bits in 64 bits, so no step from x - x[n-1] (up to
 * 65535) or its sum can overflow, and the products are rounded: the dead
 * band where the state stops decaying is below 0.5 / (1 - a) units of
 * 2^-15, under 0.002 LSB, so no limit cycle reaches the output.  The
 * output is rounded and saturated to 16 bits.  The first input primes
 * x[n-1], so a DC offset gives no step at the start; the decimator's empty
 * history still ramps in over its 51 taps, which decays below 1 LSB in
 * ln(1000) / (1 - a) = about 590 samples (37 ms at 16 kHz).
 */
#define DC_BLOCK_A_Q15          32382

struct dc_block {
    bool primed;
    int32_t x1;
    int64_t y;                  /* Q15 */
};

/*
 * Mains hum: a cascade of notch filters at the mains frequency and its
 * harmonics up to HUM_TOP_HZ, at the output rate, after the DC blocker and
 * before the level and the ring: 50 .. 400 Hz (8) for 50 Hz mains,
 * 60 .. 420 Hz (7) for 60 Hz.  Those are the lines measured above the
 * floor with a microphone worn (the hum was most of a silent signal);
 * above them the lines were weaker, and a voice's harmonics are denser.
 *
 * Each one: H(z) = (1 - 2 cos w z^-1 + z^-2) / (1 - 2 r cos w z^-1 + r^2 z^-2),
 * w = 2 pi f / fs, zeros on the unit circle at f, poles at radius
 * r = 1 - pi B / fs for a -3 dB width of B Hz.  The n-th harmonic drifts n
 * times as far as the mains (0.2 Hz), and the notch is about
 * 20 log10(2 drift / B) dB deep there, so B grows with n where 12 Hz would
 * leave less than about 15 dB: B = max(12, 2.4 n) Hz, 12 Hz up to the 5th,
 * 19.2 Hz at the 8th.  A voice harmonic d Hz from a notch loses about
 * 10 log10(1 + (B / 2d)^2) dB: 1.3 dB at 10 Hz from a 12 Hz notch, 2.8 dB
 * from the 19.2 Hz one.  The narrowest poles decay with a time constant of
 * 1 / (pi 12 Hz) = 27 ms.  The coefficients are computed for the output
 * rate at the stream start or when the setting changes (in the task, with
 * the FPU: cos()), in Q26.
 *
 * Separate biquads rather than one comb filter: a comb (1 - z^-N) would
 * notch every multiple of the mains up to the Nyquist frequency at one
 * width, deep in the voice's band, and needs fs / f to be a whole number.
 *
 * At such a low w (0.02 rad at 16 kHz) the poles sit near z = 1 and the
 * feedback amplifies rounding by about 500, so the state is not kept in
 * 16 bits: the stages pass values with 16 fraction bits in 64 bits (Q16),
 * the products are rounded, and the rounding noise stays near 0.002 LSB,
 * far below one output LSB, so no limit cycle reaches the output.  Each
 * value is limited to +-2^17 LSB, so no sum exceeds 2^62 (inputs up to
 * 2^17 LSB in Q16 are 2^33, coefficients below 2^27); the output is
 * rounded and saturated to 16 bits.
 */
#define HUM_HARMONICS           8
#define HUM_TOP_HZ              USBAUDIO_HUM_TOP_HZ
#define HUM_WIDTH_HZ            12.0
#define HUM_WIDTH_PER_HARMONIC  2.4
#define NOTCH_COEF_BITS         26
#define NOTCH_FRAC_BITS         16
#define NOTCH_LIMIT             ((int64_t) 1 << (17 + NOTCH_FRAC_BITS))

struct notch {
    int32_t b1;                 /* -2 cos w, Q26 */
    int32_t a1;                 /* 2 r cos w */
    int32_t a2;                 /* r^2 */
    int64_t x1;                 /* Q16 */
    int64_t x2;
    int64_t y1;
    int64_t y2;
};

/* Running level of the decimated samples. */
struct level_acc {
    uint32_t window;            /* samples per window */
    uint32_t count;
    uint32_t peak;
    uint64_t sum_squares;
};

static struct {
    enum audio_state state;
    bool created;
    TX_MUTEX request_lock;      /* one request at a time */
    TX_MUTEX data_lock;         /* stats, level, ring */
    TX_SEMAPHORE done;          /* the task's answers */
    TX_EVENT_FLAGS_GROUP events;
    TX_EVENT_FLAGS_GROUP ring_events;

    /* A request and its answer. */
    enum audio_request request;
    struct request_args arg;

    /*
     * The volume and mute last set, kept for the session (across streams,
     * devices and host restarts); until one is set, the device keeps its own.
     */
    bool want_percent_set;
    uint8_t want_percent;
    bool want_mute_set;
    bool want_mute;
    /* The mains frequency of the hum filter, Hz, 0: off; for the session. */
    uint8_t hum_hz;
    int result;

    /* Written by the drivers' callbacks (their tasks), read by "uac". */
    volatile uint32_t pending;          /* a microphone interface to open: 0x10000 | address << 8
                                           | interface, 0: none */
    volatile uint8_t open_address;      /* of the open microphone, 0: none */
    volatile uint32_t wakeups;
    volatile uint32_t transfer_errors;

    /* The task's own. */
    uint8_t version;                    /* of the open microphone: 1, 2, 0: none */
    uac_host_device_handle_t dev;       /* UAC 1.0: the driver's interface */
    struct usbaudio_info info;          /* of the open interface, without the live fields */
    bool streaming;
    uint32_t rate;
    uint8_t channels;
    uint8_t factor;                     /* of the decimation: 3, or 1 for a 16 kHz stream */
    uint32_t urb_bytes;                 /* the most one transfer brings */
    uint8_t carry[4];                   /* the start of a frame cut by a read */
    uint8_t carry_len;
    struct decimator dec;
    struct dc_block dc_block;
    uint8_t notches;                    /* in use */
    struct notch notch[HUM_HARMONICS];
    int64_t dc_sum;                     /* of the raw (mixed) input samples, this second */
    uint32_t dc_count;
    struct level_acc acc;
    struct usbaudio_stats work;         /* published to stats */

    /* Under data_lock. */
    struct usbaudio_stats stats;
    struct usbaudio_level level;
    int16_t *ring;
    uint32_t written;                   /* samples written since the start */
    uint32_t consumed;                  /* by the consumer */
} audio;

/* Read buffer of the task, with room for a carried part of a frame in front. */
static uint8_t rx_buf[sizeof(audio.carry) + RX_CHUNK] __attribute__((aligned(4)));

static ULONG ms_to_ticks(uint32_t ms)
{
    return (ULONG) (((uint64_t) ms * TX_TIMER_TICKS_PER_SECOND + 999u) / 1000u);
}

/* ---- Decimation and level ------------------------------------------------ */

static void decimator_reset(struct decimator *d)
{
    memset(d, 0, sizeof(*d));
}

/* One input sample; true with an output sample in *out every third one. */
static bool decimate(struct decimator *d, int16_t x, int16_t *out)
{
    const int16_t *w;
    int32_t acc;

    d->hist[d->pos] = x;
    d->hist[d->pos + FIR_TAPS] = x;
    w = &d->hist[d->pos + 1];               /* oldest first, w[FIR_TAPS - 1] == x */
    d->pos = (uint8_t) (d->pos + 1u == FIR_TAPS ? 0u : d->pos + 1u);
    if (++d->phase < 3u) {
        return false;
    }
    d->phase = 0;

    acc = (int32_t) fir_half[FIR_CENTRE] * w[FIR_CENTRE];
    for (int k = 0; k < 24; k += 3) {
        acc += (int32_t) fir_half[k] * (w[k] + w[FIR_TAPS - 1 - k]);
        acc += (int32_t) fir_half[k + 2] * (w[k + 2] + w[FIR_TAPS - 3 - k]);
    }
    acc += (int32_t) fir_half[24] * (w[24] + w[26]);
    acc = (acc + (1 << 14)) >> 15;
    if (acc > INT16_MAX) {
        acc = INT16_MAX;
    } else if (acc < INT16_MIN) {
        acc = INT16_MIN;
    }
    *out = (int16_t) acc;
    return true;
}

static int16_t dc_block(struct dc_block *f, int16_t x)
{
    int64_t out;

    if (!f->primed) {
        f->primed = true;
        f->x1 = x;
    }
    f->y = (int64_t) (x - f->x1) * 32768 + ((DC_BLOCK_A_Q15 * f->y + (1 << 14)) >> 15);
    f->x1 = x;
    out = (f->y + (1 << 14)) >> 15;
    if (out > INT16_MAX) {
        out = INT16_MAX;
    } else if (out < INT16_MIN) {
        out = INT16_MIN;
    }
    return (int16_t) out;
}

/* The notches for the mains frequency at the output rate, their state cleared. */
static void hum_setup(void)
{
    uint32_t fs = audio.rate / audio.factor;

    audio.notches = 0;
    memset(audio.notch, 0, sizeof(audio.notch));
    if (audio.hum_hz == 0u || fs == 0u) {
        return;
    }
    for (int h = 1; h <= HUM_HARMONICS && (unsigned int) audio.hum_hz * h <= HUM_TOP_HZ; h++) {
        double f = (double) audio.hum_hz * h;
        double w = 2.0 * M_PI * f / fs;
        double c = cos(w);
        double width = HUM_WIDTH_PER_HARMONIC * h > HUM_WIDTH_HZ ? HUM_WIDTH_PER_HARMONIC * h : HUM_WIDTH_HZ;
        double rr = 1.0 - M_PI * width / fs;
        double one = (double) (1 << NOTCH_COEF_BITS);
        struct notch *n = &audio.notch[audio.notches];

        if (f >= 0.45 * fs) {
            break;
        }
        n->b1 = (int32_t) lround(-2.0 * c * one);
        n->a1 = (int32_t) lround(2.0 * rr * c * one);
        n->a2 = (int32_t) lround(rr * rr * one);
        audio.notches++;
    }
}

static int64_t notch_limit(int64_t v)
{
    return v > NOTCH_LIMIT ? NOTCH_LIMIT : v < -NOTCH_LIMIT ? -NOTCH_LIMIT : v;
}

static int16_t hum_filter(int16_t in)
{
    int64_t x = (int64_t) in * (1 << NOTCH_FRAC_BITS);
    int64_t out;

    for (uint8_t i = 0; i < audio.notches; i++) {
        struct notch *n = &audio.notch[i];
        int64_t acc = (x + n->x2) * (1 << NOTCH_COEF_BITS) + (int64_t) n->b1 * n->x1
                      + (int64_t) n->a1 * n->y1 - (int64_t) n->a2 * n->y2;
        int64_t y = notch_limit((acc + (1 << (NOTCH_COEF_BITS - 1))) >> NOTCH_COEF_BITS);

        n->x2 = n->x1;
        n->x1 = x;
        n->y2 = n->y1;
        n->y1 = y;
        x = y;
    }
    out = (x + (1 << (NOTCH_FRAC_BITS - 1))) >> NOTCH_FRAC_BITS;
    return (int16_t) (out > INT16_MAX ? INT16_MAX : out < INT16_MIN ? INT16_MIN : out);
}

/* A decimated sample into the level; a complete window is published (data_lock held). */
static void level_add(int16_t s)
{
    uint32_t mag = (uint32_t) (s < 0 ? -(int32_t) s : s);

    if (mag > audio.acc.peak) {
        audio.acc.peak = mag;
    }
    audio.acc.sum_squares += (uint64_t) ((int32_t) s * s);
    if (++audio.acc.count < audio.acc.window) {
        return;
    }
    audio.level.seq++;
    audio.level.samples = audio.acc.count;
    audio.level.peak = audio.acc.peak;
    audio.level.sum_squares = audio.acc.sum_squares;
    audio.acc.count = 0;
    audio.acc.peak = 0;
    audio.acc.sum_squares = 0;
}

/*
 * PCM bytes from the microphone: whole frames are mixed, decimated, measured
 * and put into the ring; a cut frame's start waits for the next read.  The
 * bytes are at rx_buf + sizeof(audio.carry).
 */
static void process(size_t got)
{
    size_t frame = (size_t) audio.channels * 2u;
    uint8_t *p = &rx_buf[sizeof(audio.carry) - audio.carry_len];
    size_t len = audio.carry_len + got;
    size_t frames = len / frame;
    uint32_t outs = 0;
    uint32_t clipped = 0;

    memcpy(p, audio.carry, audio.carry_len);

    (void) tx_mutex_get(&audio.data_lock, TX_WAIT_FOREVER);
    for (size_t i = 0; i < frames; i++, p += frame) {
        int32_t s = (int16_t) (p[0] | (p[1] << 8));
        int16_t out;

        clipped += (s == INT16_MIN || s == INT16_MAX) ? 1u : 0u;
        if (audio.channels == 2u) {
            int32_t right = (int16_t) (p[2] | (p[3] << 8));

            clipped += (right == INT16_MIN || right == INT16_MAX) ? 1u : 0u;
            s = (s + right) >> 1;
        }
        audio.dc_sum += s;
        if (++audio.dc_count >= audio.rate) {
            /* The mean, rounded to the nearest. */
            int64_t sum = audio.dc_sum;
            int64_t half = (int64_t) (audio.dc_count / 2u);

            audio.work.dc = (int32_t) ((sum + (sum < 0 ? -half : half)) / (int64_t) audio.dc_count);
            audio.work.dc_valid = true;
            audio.dc_sum = 0;
            audio.dc_count = 0;
        }
        if (audio.factor == 1u) {
            out = (int16_t) s;
        } else if (!decimate(&audio.dec, (int16_t) s, &out)) {
            continue;
        }
        out = hum_filter(dc_block(&audio.dc_block, out));
        level_add(out);
        if (audio.ring != NULL) {
            audio.ring[audio.written & RING_MASK] = out;
            audio.written++;
        }
        outs++;
    }
    (void) tx_mutex_put(&audio.data_lock);

    audio.carry_len = (uint8_t) (len - frames * frame);
    memcpy(audio.carry, p, audio.carry_len);
    audio.work.bytes += got;
    audio.work.frames += frames;
    audio.work.out_samples += outs;
    audio.work.clipped += clipped;
    if (outs > 0u) {
        (void) tx_event_flags_set(&audio.ring_events, RING_EV_DATA, TX_OR);
    }
}

/* The task's counters to the shared copy. */
static void publish(void)
{
    if (audio.version == 2u) {
        struct uac2_counters c;

        uac2_counters(&c);
        audio.work.wakeups = c.wakeups;
        audio.work.transfer_errors = c.transfer_errors;
        audio.work.overruns = c.overruns;
        audio.work.packet_errors = c.packet_errors;
    } else {
        audio.work.wakeups = audio.wakeups;
        audio.work.transfer_errors = audio.transfer_errors;
    }
    audio.work.streaming = audio.streaming;
    (void) tx_mutex_get(&audio.data_lock, TX_WAIT_FOREVER);
    audio.work.ring_lost = audio.stats.ring_lost;   /* the consumer's count */
    audio.stats = audio.work;
    (void) tx_mutex_put(&audio.data_lock);
}

/* Up to size bytes of the microphone's buffer, without waiting: 0, -EAGAIN (empty), -EIO. */
static int mic_read(uint8_t *buf, size_t size, size_t *got)
{
    uint32_t n = 0;
    esp_err_t err;

    if (audio.version == 2u) {
        return uac2_read(buf, size, got);
    }
    err = uac_host_device_read(audio.dev, buf, (uint32_t) size, &n, 0);
    *got = n;
    if (err == ESP_ERR_TIMEOUT) {
        return -EAGAIN;
    }
    return err == ESP_OK ? 0 : -EIO;
}

/* Everything the microphone's buffer holds, without waiting. */
static void drain(void)
{
    size_t total = 0;

    if (!audio.streaming) {
        return;
    }
    for (;;) {
        size_t got = 0;
        int err = mic_read(&rx_buf[sizeof(audio.carry)], RX_CHUNK, &got);

        audio.work.reads++;
        if (err == -EAGAIN) {
            audio.work.empty_reads++;
            break;
        }
        if (err != 0) {
            audio.work.read_errors++;
            break;
        }
        if (got > 0u) {
            int64_t now = esp_timer_get_time();

            if (audio.work.first_us == 0) {
                audio.work.first_us = now;
                audio.work.first_frames = (uint32_t) ((audio.carry_len + got) / ((size_t) audio.channels * 2u));
                LOG_INF("first samples %u ms after the start",
                        (unsigned int) ((now - audio.work.start_us) / 1000));
            }
            audio.work.last_us = now;
            total += got;
            process(got);
        }
        if (got < RX_CHUNK) {
            audio.work.short_reads++;
            break;
        }
    }
    if (total + audio.urb_bytes > RX_BUFFER_BYTES) {
        audio.work.near_full++;
    }
    publish();
}

/* ---- Driver callbacks (in the driver's task) ---------------------------- */

static void device_event(uac_host_device_handle_t handle, const uac_host_device_event_t event, void *arg)
{
    (void) handle;
    (void) arg;

    switch (event) {
    case UAC_HOST_DEVICE_EVENT_RX_DONE:
        audio.wakeups++;
        (void) tx_event_flags_set(&audio.events, EV_DATA, TX_OR);
        break;
    case UAC_HOST_DEVICE_EVENT_TRANSFER_ERROR:
        audio.transfer_errors++;
        break;
    case UAC_HOST_DRIVER_EVENT_DISCONNECTED:
        (void) tx_event_flags_set(&audio.events, EV_DISCONNECTED, TX_OR);
        break;
    default:
        break;
    }
}

/*
 * A microphone interface found in a device that enumerated, by either
 * driver (a UAC 2.0 device is reported by both: usb_host_uac reports every
 * streaming interface).  The first one that is not open yet is taken; it
 * is opened when no other is open.
 */
static void offer(uint8_t addr, uint8_t iface)
{
    bool take;

    portENTER_CRITICAL();
    take = audio.pending == 0u && addr != audio.open_address;
    if (take) {
        audio.pending = 0x10000u | ((uint32_t) addr << 8) | iface;
    }
    portEXIT_CRITICAL();
    if (take) {
        (void) tx_event_flags_set(&audio.events, EV_CONNECTED, TX_OR);
    }
}

static void driver_event(uint8_t addr, uint8_t iface_num, const uac_host_driver_event_t event, void *arg)
{
    (void) arg;

    if (event == UAC_HOST_DRIVER_EVENT_RX_CONNECTED) {
        offer(addr, iface_num);
    }
}

/* The UAC 2.0 client's callbacks (in its task). */
static void uac2_connected(uint8_t addr, uint8_t iface)
{
    offer(addr, iface);
}

static void uac2_gone(void)
{
    (void) tx_event_flags_set(&audio.events, EV_DISCONNECTED, TX_OR);
}

static void uac2_data(void)
{
    (void) tx_event_flags_set(&audio.events, EV_DATA, TX_OR);
}

static const struct uac2_callbacks uac2_cb = {
    .connected = uac2_connected,
    .gone = uac2_gone,
    .data = uac2_data,
};

/* ---- The task's steps --------------------------------------------------- */

static void ascii_of(const wchar_t *s, char *out, size_t size)
{
    size_t n = 0;

    for (; *s != 0 && n + 1u < size; s++) {
        out[n++] = (*s >= 0x20 && *s < 0x7F) ? (char) *s : '?';
    }
    out[n] = '\0';
}

/* Whether alternate setting a (UAC 1.0) or the clock (UAC 2.0) offers rate. */
static bool offers(const struct usbaudio_info *info, const struct usbaudio_alt *a, uint32_t rate)
{
    if (info->uac_version == 2u) {
        if (info->clock_range_count == 0u) {
            return rate == info->clock_rate;
        }
        for (uint8_t i = 0; i < info->clock_range_count; i++) {
            const struct usbaudio_rate_range *r = &info->clock_ranges[i];

            if (rate >= r->min && rate <= r->max && (r->res == 0u || (rate - r->min) % r->res == 0u)) {
                return true;
            }
        }
        return false;
    }
    if (a->rate_count == 0u) {
        return rate >= a->rate_min && rate <= a->rate_max;
    }
    for (uint8_t i = 0; i < a->rate_count; i++) {
        if (a->rates[i] == rate) {
            return true;
        }
    }
    return false;
}

/* The highest rate up to limit that alternate setting a or the clock offers, 0: none. */
static uint32_t highest(const struct usbaudio_info *info, const struct usbaudio_alt *a, uint32_t limit)
{
    uint32_t best = 0;

    if (info->uac_version == 2u) {
        for (uint8_t i = 0; i < info->clock_range_count; i++) {
            const struct usbaudio_rate_range *r = &info->clock_ranges[i];
            uint32_t top = r->max < limit ? r->max : limit;

            if (top >= r->min) {
                top = r->res == 0u ? top : r->min + (top - r->min) / r->res * r->res;
                best = top > best ? top : best;
            }
        }
        if (info->clock_range_count == 0u && info->clock_rate <= limit) {
            best = info->clock_rate;
        }
        return best;
    }
    if (a->rate_count == 0u) {
        return a->rate_min <= limit ? (a->rate_max < limit ? a->rate_max : limit) : 0u;
    }
    for (uint8_t i = 0; i < a->rate_count; i++) {
        if (a->rates[i] <= limit && a->rates[i] > best) {
            best = a->rates[i];
        }
    }
    return best;
}

/* The defaults of "start" for the open microphone (usbaudio_info). */
static void default_format(struct usbaudio_info *info)
{
    const struct usbaudio_alt *pick = NULL;

    info->default_rate = 0;
    info->default_channels = 0;
    for (uint8_t i = 0; i < info->alt_count; i++) {
        const struct usbaudio_alt *a = &info->alts[i];

        if (a->bits != 16u || a->subframe != 2u || a->channels < 1u || a->channels > 2u) {
            continue;
        }
        if (pick == NULL || (a->channels == 1u && pick->channels != 1u)) {
            pick = a;
        }
    }
    if (pick == NULL) {
        return;
    }
    info->default_channels = pick->channels;
    if (offers(info, pick, 48000u)) {
        info->default_rate = 48000u;
    } else if (offers(info, pick, 16000u)) {
        info->default_rate = 16000u;
    } else {
        info->default_rate = highest(info, pick, 48000u);
    }
}

static void interface_open(void)
{
    uint32_t c = audio.pending;
    uint8_t addr = (uint8_t) (c >> 8);
    uint8_t iface = (uint8_t) c;
    const uac_host_device_config_t config = {
        .addr = addr,
        .iface_num = iface,
        .buffer_size = RX_BUFFER_BYTES,
        .buffer_threshold = RX_THRESHOLD_BYTES,
        .callback = device_event,
        .callback_arg = NULL,
    };
    uac_host_dev_info_t dev_info;
    struct usbaudio_info *info = &audio.info;
    esp_err_t err;

    int ret;

    if (c == 0u || audio.version != 0u) {
        return;                 /* none, or it waits until the open one goes */
    }
    audio.pending = 0;
    /* A UAC 2.0 device goes to audio_uac2.c; usb_host_uac would refuse it. */
    ret = uac2_open(addr, info);
    if (ret == 0) {
        audio.version = 2;
        audio.open_address = addr;
        default_format(info);
        LOG_INF("microphone: device %u interface %u, %04x:%04x \"%s\", UAC 2.0", addr, info->interface,
                info->vid, info->pid, info->product);
        return;
    }
    if (ret != -ENOTSUP) {
        LOG_ERR("device %u: UAC 2.0 microphone not usable (%d)", addr, ret);
        return;
    }
    err = uac_host_device_open(&config, &audio.dev);
    if (err != ESP_OK) {
        LOG_ERR("device %u interface %u: uac_host_device_open: error 0x%x", addr, iface,
                (unsigned int) err);
        audio.dev = NULL;
        return;
    }
    audio.version = 1;
    audio.open_address = addr;
    memset(info, 0, sizeof(*info));
    info->open = true;
    info->uac_version = 1;
    info->address = addr;
    info->interface = iface;
    if (uac_host_get_device_info(audio.dev, &dev_info) == ESP_OK) {
        info->vid = dev_info.VID;
        info->pid = dev_info.PID;
        ascii_of(dev_info.iManufacturer, info->manufacturer, sizeof(info->manufacturer));
        ascii_of(dev_info.iProduct, info->product, sizeof(info->product));
        for (uint8_t i = 1; i <= dev_info.iface_alt_num && info->alt_count < USBAUDIO_ALTS_MAX; i++) {
            uac_host_dev_alt_param_t p;
            struct usbaudio_alt *a = &info->alts[info->alt_count];

            if (uac_host_get_device_alt_param(audio.dev, i, &p) != ESP_OK) {
                continue;
            }
            a->channels = p.channels;
            a->bits = p.bit_resolution;
            a->subframe = p.subframe_size;
            if (p.sample_freq_type == 0u) {
                a->rate_min = p.sample_freq_lower;
                a->rate_max = p.sample_freq_upper;
            } else {
                a->rate_count = p.sample_freq_type < USBAUDIO_RATES_MAX ? p.sample_freq_type
                                                                         : USBAUDIO_RATES_MAX;
                memcpy(a->rates, p.sample_freq, a->rate_count * sizeof(a->rates[0]));
            }
            info->alt_count++;
        }
    }
    default_format(info);
    LOG_INF("microphone: device %u interface %u, %04x:%04x \"%s\", %u alternate settings", addr,
            iface, info->vid, info->pid, info->product, info->alt_count);
}

static void stream_end(void)
{
    if (!audio.streaming) {
        return;
    }
    audio.streaming = false;
    publish();
}

/*
 * Stops the stream if it runs (the UAC 1.0 driver has done that itself
 * when the device went) and closes the microphone.  0, or -EIO when the
 * UAC 1.0 driver kept it open.
 */
static int mic_close(void)
{
    esp_err_t err = ESP_OK;

    if (audio.version == 2u) {
        uac2_close();
    } else if (audio.version == 1u) {
        err = uac_host_device_close(audio.dev);
        if (err != ESP_OK) {
            LOG_ERR("uac_host_device_close: error 0x%x", (unsigned int) err);
        }
        audio.dev = NULL;
    }
    audio.version = 0;
    audio.open_address = 0;
    memset(&audio.info, 0, sizeof(audio.info));
    return err == ESP_OK ? 0 : -EIO;
}

/* After DISCONNECTED: the driver has stopped a UAC 1.0 stream; the microphone is closed here. */
static void interface_gone(void)
{
    if (audio.version == 0u) {
        return;
    }
    stream_end();
    (void) mic_close();
    LOG_INF("microphone disconnected");
}

static int errno_of(esp_err_t err)
{
    switch (err) {
    case ESP_OK:
        return 0;
    case ESP_ERR_NO_MEM:
        return -ENOMEM;
    case ESP_ERR_NOT_FOUND:
    case ESP_ERR_INVALID_SIZE:
        return -ENOTSUP;
    case ESP_ERR_TIMEOUT:
        return -ETIMEDOUT;
    default:
        return -EIO;
    }
}

/* ---- Volume and mute, by the microphone's class ---------------------- */

static int mic_volume_set(uint8_t percent)
{
    esp_err_t err;

    if (audio.version == 2u) {
        return uac2_volume_set(percent);
    }
    err = uac_host_device_set_volume(audio.dev, percent);
    if (err != ESP_OK) {
        LOG_ERR("uac_host_device_set_volume(%u): error 0x%x", percent, (unsigned int) err);
    }
    return err == ESP_ERR_NOT_SUPPORTED ? -ENOTSUP : errno_of(err);
}

static int mic_mute_set(bool mute)
{
    esp_err_t err;

    if (audio.version == 2u) {
        return uac2_mute_set(mute);
    }
    err = uac_host_device_set_mute(audio.dev, mute);
    if (err != ESP_OK) {
        LOG_ERR("uac_host_device_set_mute(%s): error 0x%x", mute ? "on" : "off", (unsigned int) err);
    }
    return err == ESP_ERR_NOT_SUPPORTED ? -ENOTSUP : errno_of(err);
}

/* What the device reports; the fields it cannot give stay invalid. */
static void mic_volume_get(struct usbaudio_volume *v)
{
    if (audio.version == 2u) {
        int16_t db;
        uint8_t percent;
        bool mute;

        if (uac2_volume_get(&db, &percent) == 0) {
            v->db_valid = true;
            v->db = db;
            v->percent_valid = audio.info.volume_range_valid;
            v->percent = percent;
        }
        if (uac2_mute_get(&mute) == 0) {
            v->mute_valid = true;
            v->mute = mute;
        }
    } else {
        uint8_t percent;
        int16_t db;
        bool mute;

        if (uac_host_device_get_volume(audio.dev, &percent) == ESP_OK) {
            v->percent_valid = true;
            v->percent = percent;
        }
        if (uac_host_device_get_volume_db(audio.dev, &db) == ESP_OK) {
            v->db_valid = true;
            v->db = db;
        }
        if (uac_host_device_get_mute(audio.dev, &mute) == ESP_OK) {
            v->mute_valid = true;
            v->mute = mute;
        }
    }
}

/*
 * Volume and mute are taken only while the stream runs, the same for both
 * classes (usb_host_uac takes them only once its interface has started).
 * The UAC 1.0 driver keeps the percentage only in the interface it opened,
 * and a device that went and came back has its own default again, so what
 * was set last is set again at every start.
 */
static void apply_settings(void)
{
    int ret;

    if (audio.want_percent_set) {
        ret = mic_volume_set(audio.want_percent);
        if (ret != 0) {
            LOG_WRN("volume %u%% not set again (%d)", audio.want_percent, ret);
        }
    }
    if (audio.want_mute_set) {
        ret = mic_mute_set(audio.want_mute);
        if (ret != 0) {
            LOG_WRN("mute %s not set again (%d)", audio.want_mute ? "on" : "off", ret);
        }
    }
}

/* -ENODEV, -ESRCH (not streaming), or 0. */
static int controls_ready(void)
{
    if (audio.version == 0u) {
        return -ENODEV;
    }
    return audio.streaming ? 0 : -ESRCH;
}

static int serve_volume_get(void)
{
    struct usbaudio_volume *v = audio.arg.volume;
    int ret = controls_ready();

    v->set = audio.want_percent_set;
    v->set_percent = audio.want_percent;
    if (ret == 0) {
        mic_volume_get(v);
    }
    return ret;
}

static int serve_volume_set(void)
{
    int ret = controls_ready();

    if (ret == 0 && audio.arg.percent > 100u) {
        ret = -EINVAL;
    }
    if (ret == 0) {
        ret = mic_volume_set(audio.arg.percent);
    }
    if (ret == 0) {
        audio.want_percent_set = true;
        audio.want_percent = audio.arg.percent;
    }
    return ret;
}

static int serve_mute_set(void)
{
    int ret = controls_ready();

    if (ret == 0) {
        ret = mic_mute_set(audio.arg.mute);
    }
    if (ret == 0) {
        audio.want_mute_set = true;
        audio.want_mute = audio.arg.mute;
    }
    return ret;
}

/* ---- Stream ------------------------------------------------------------ */

static int mic_start(void)
{
    const uac_host_stream_config_t config = {
        .channels = audio.channels,
        .bit_resolution = 16,
        .sample_freq = audio.rate,
        .flags = 0,
    };
    esp_err_t err;

    if (audio.version == 2u) {
        return uac2_start(audio.rate, audio.channels, RX_BUFFER_BYTES, RX_THRESHOLD_BYTES);
    }
    err = uac_host_device_start(audio.dev, &config);
    if (err != ESP_OK) {
        LOG_ERR("uac_host_device_start(%u Hz, %u channels): error 0x%x", (unsigned int) audio.rate,
                audio.channels, (unsigned int) err);
    }
    return errno_of(err);
}

static int mic_stop(void)
{
    esp_err_t err;

    if (audio.version == 2u) {
        return uac2_stop();
    }
    err = uac_host_device_stop(audio.dev);
    if (err != ESP_OK) {
        LOG_ERR("uac_host_device_stop: error 0x%x", (unsigned int) err);
    }
    return errno_of(err);
}

static int serve_start(void)
{
    uint32_t frame;
    int ret;

    if (audio.version == 0u) {
        return -ENODEV;
    }
    if (audio.arg.rate == 0u) {
        audio.arg.rate = audio.info.default_rate;
    }
    if (audio.arg.channels == 0u) {
        audio.arg.channels = audio.info.default_channels;
    }
    if (audio.arg.rate == 0u || audio.arg.channels == 0u) {
        return -ENOTSUP;            /* no 16-bit format */
    }
    frame = (uint32_t) audio.arg.channels * 2u;
    if (audio.streaming) {
        return -EBUSY;
    }
    if (audio.arg.channels < 1u || audio.arg.channels > 2u || audio.arg.rate < 3u
        || (uint64_t) audio.arg.rate * frame > 192000u) {
        return -EINVAL;
    }

    audio.rate = audio.arg.rate;
    audio.channels = audio.arg.channels;
    /* A 16 kHz stream is already at the output rate: only the DC blocker. */
    audio.factor = audio.rate == 16000u ? 1u : 3u;
    audio.urb_bytes = (audio.rate / 1000u + 1u) * frame * CONFIG_UAC_NUM_PACKETS_PER_URB;
    audio.carry_len = 0;
    decimator_reset(&audio.dec);
    memset(&audio.dc_block, 0, sizeof(audio.dc_block));
    hum_setup();
    audio.dc_sum = 0;
    audio.dc_count = 0;
    memset(&audio.acc, 0, sizeof(audio.acc));
    audio.acc.window = audio.rate / audio.factor / 10u;    /* 100 ms of the output */
    if (audio.acc.window == 0u) {
        audio.acc.window = 1;
    }
    memset(&audio.work, 0, sizeof(audio.work));
    audio.work.uac_version = audio.version;
    audio.work.rate = audio.rate;
    audio.work.channels = audio.channels;
    audio.work.out_rate = audio.rate / audio.factor;
    audio.work.buffer_bytes = RX_BUFFER_BYTES;
    audio.work.start_us = esp_timer_get_time();
    audio.wakeups = 0;
    audio.transfer_errors = 0;
    (void) tx_mutex_get(&audio.data_lock, TX_WAIT_FOREVER);
    memset(&audio.level, 0, sizeof(audio.level));
    audio.stats = audio.work;
    audio.written = 0;
    audio.consumed = 0;
    (void) tx_mutex_put(&audio.data_lock);

    ret = mic_start();
    if (ret != 0) {
        return ret;
    }
    audio.streaming = true;
    publish();
    apply_settings();
    LOG_INF("stream started: %u Hz, %u channels, 16 bits", (unsigned int) audio.rate, audio.channels);
    return 0;
}

static int serve_stop(void)
{
    int ret;

    if (audio.version == 0u) {
        return -ENODEV;
    }
    if (!audio.streaming) {
        return -ESRCH;
    }
    ret = mic_stop();
    stream_end();
    if (ret == 0) {
        LOG_INF("stream stopped");
    }
    return ret;
}

static int serve_info(void)
{
    struct usbaudio_info *info = audio.arg.info;

    *info = audio.info;
    info->streaming = audio.streaming;
    if (audio.version != 0u && audio.streaming) {
        struct usbaudio_volume v = {0};

        mic_volume_get(&v);
        info->volume_valid = v.db_valid;
        info->volume = v.db;
        info->mute_valid = v.mute_valid;
        info->mute = v.mute;
    }
    return 0;
}

/* The stream stopped and the microphone closed, before the drivers go. */
static int serve_quit(void)
{
    esp_err_t err;
    int ret;

    if (audio.version != 0u) {
        if (audio.streaming) {
            (void) serve_stop();
        }
        if (mic_close() != 0) {
            return -EIO;
        }
    }
    audio.pending = 0;
    ret = uac2_uninstall();
    if (ret != 0) {
        return ret == -ETIMEDOUT ? -EIO : ret;
    }
    err = uac_host_uninstall();
    if (err != ESP_OK) {
        LOG_ERR("uac_host_uninstall: error 0x%x", (unsigned int) err);
        return -EIO;
    }
    return 0;
}

static void answer(int result)
{
    audio.result = result;
    (void) tx_semaphore_put(&audio.done);
}

static void audio_task(void *arg)
{
    const uac_host_driver_config_t config = {
        .create_background_task = true,
        .task_priority = APP_PRIO_UAC_DRIVER,
        .stack_size = APP_STACK_UAC_DRIVER,
        .core_id = tskNO_AFFINITY,
        .callback = driver_event,
        .callback_arg = NULL,
    };
    esp_err_t err;
    int ret;

    (void) arg;

    err = uac_host_install(&config);
    if (err != ESP_OK) {
        LOG_ERR("uac_host_install: error 0x%x", (unsigned int) err);
        answer(err == ESP_ERR_NO_MEM ? -ENOMEM : -EIO);
        vTaskDelete(NULL);
        return;
    }
    ret = uac2_install(&uac2_cb);
    if (ret != 0) {
        (void) uac_host_uninstall();
        answer(ret);
        vTaskDelete(NULL);
        return;
    }
    answer(0);

    for (;;) {
        ULONG events = 0;

        (void) tx_event_flags_get(&audio.events, EV_ALL, TX_OR_CLEAR, &events, TX_WAIT_FOREVER);
        if ((events & EV_DISCONNECTED) != 0u) {
            interface_gone();
        }
        if ((events & (EV_CONNECTED | EV_DISCONNECTED)) != 0u) {
            interface_open();       /* also one that waited for the open one to go */
        }
        if ((events & EV_DATA) != 0u) {
            drain();
        }
        if ((events & EV_REQUEST) == 0u) {
            continue;
        }
        switch (audio.request) {
        case REQUEST_INFO:
            answer(serve_info());
            break;
        case REQUEST_START:
            answer(serve_start());
            break;
        case REQUEST_STOP:
            answer(serve_stop());
            break;
        case REQUEST_VOLUME_GET:
            answer(serve_volume_get());
            break;
        case REQUEST_VOLUME_SET:
            answer(serve_volume_set());
            break;
        case REQUEST_MUTE_SET:
            answer(serve_mute_set());
            break;
        case REQUEST_HUM_SET:
            /* From the next sample on, the notches' state cleared. */
            audio.hum_hz = audio.arg.hum_hz;
            if (audio.streaming) {
                hum_setup();
            }
            answer(0);
            break;
        case REQUEST_QUIT:
            ret = serve_quit();

            if (ret == 0) {
                answer(0);
                vTaskDelete(NULL);
                return;
            }
            answer(ret);
            break;
        default:
            break;
        }
        audio.request = REQUEST_NONE;
    }
}

/* ---- API ---------------------------------------------------------------- */

/* With request_lock held.  The task's answer, or -ETIMEDOUT. */
static int wait_answer(void)
{
    if (tx_semaphore_get(&audio.done, ms_to_ticks(AUDIO_ANSWER_TIMEOUT_MS)) != TX_SUCCESS) {
        LOG_ERR("no answer from uac in %u ms", (unsigned int) AUDIO_ANSWER_TIMEOUT_MS);
        audio.state = AUDIO_BROKEN;
        return -ETIMEDOUT;
    }
    return audio.result;
}

/* A request to the task, with its arguments; its answer. */
static int request(enum audio_request what, const struct request_args *args)
{
    int ret;

    if (!audio.created) {
        return -EALREADY;
    }
    (void) tx_mutex_get(&audio.request_lock, TX_WAIT_FOREVER);
    if (audio.state != AUDIO_RUNNING) {
        ret = audio.state == AUDIO_BROKEN ? -EIO : -EALREADY;
    } else {
        audio.arg = *args;
        audio.request = what;
        (void) tx_event_flags_set(&audio.events, EV_REQUEST, TX_OR);
        ret = wait_answer();
    }
    (void) tx_mutex_put(&audio.request_lock);
    return ret;
}

static int objects_create(void)
{
    if (audio.created) {
        return 0;
    }
    audio.hum_hz = 50;
    if (tx_mutex_create(&audio.request_lock, "uac_req", TX_INHERIT) != TX_SUCCESS) {
        return -ENOMEM;
    }
    if (tx_mutex_create(&audio.data_lock, "uac_data", TX_INHERIT) != TX_SUCCESS) {
        (void) tx_mutex_delete(&audio.request_lock);
        return -ENOMEM;
    }
    if (tx_semaphore_create(&audio.done, "uac", 0) != TX_SUCCESS) {
        (void) tx_mutex_delete(&audio.data_lock);
        (void) tx_mutex_delete(&audio.request_lock);
        return -ENOMEM;
    }
    if (tx_event_flags_create(&audio.events, "uac") != TX_SUCCESS) {
        (void) tx_semaphore_delete(&audio.done);
        (void) tx_mutex_delete(&audio.data_lock);
        (void) tx_mutex_delete(&audio.request_lock);
        return -ENOMEM;
    }
    if (tx_event_flags_create(&audio.ring_events, "uac_ring") != TX_SUCCESS) {
        (void) tx_event_flags_delete(&audio.events);
        (void) tx_semaphore_delete(&audio.done);
        (void) tx_mutex_delete(&audio.data_lock);
        (void) tx_mutex_delete(&audio.request_lock);
        return -ENOMEM;
    }
    audio.created = true;
    return 0;
}

int usbaudio_start(void)
{
    int16_t *ring;
    ULONG stale;
    int ret = objects_create();

    if (ret != 0) {
        return ret;
    }
    (void) tx_mutex_get(&audio.request_lock, TX_WAIT_FOREVER);
    if (audio.state != AUDIO_STOPPED) {
        ret = audio.state == AUDIO_BROKEN ? -EIO : -EALREADY;
        goto out;
    }
    ring = malloc(USBAUDIO_RING_SAMPLES * sizeof(*ring));
    if (ring == NULL) {
        ret = -ENOMEM;
        goto out;
    }
    (void) tx_mutex_get(&audio.data_lock, TX_WAIT_FOREVER);
    audio.ring = ring;
    audio.written = 0;
    audio.consumed = 0;
    memset(&audio.stats, 0, sizeof(audio.stats));
    memset(&audio.level, 0, sizeof(audio.level));
    (void) tx_mutex_put(&audio.data_lock);
    audio.dev = NULL;
    audio.version = 0;
    audio.open_address = 0;
    audio.streaming = false;
    audio.pending = 0;
    audio.request = REQUEST_NONE;
    memset(&audio.info, 0, sizeof(audio.info));
    (void) tx_event_flags_get(&audio.events, EV_ALL, TX_OR_CLEAR, &stale, TX_NO_WAIT);

    if (xTaskCreate(audio_task, "uac", APP_STACK_UAC_READER, NULL, APP_PRIO_UAC_READER, NULL) != pdPASS) {
        ret = -ENOMEM;
    } else {
        ret = wait_answer();
        if (ret == 0) {
            audio.state = AUDIO_RUNNING;
            goto out;
        }
        if (ret == -ETIMEDOUT) {
            goto out;               /* the ring stays: the task may still use it */
        }
    }
    (void) tx_mutex_get(&audio.data_lock, TX_WAIT_FOREVER);
    audio.ring = NULL;
    (void) tx_mutex_put(&audio.data_lock);
    free(ring);
out:
    (void) tx_mutex_put(&audio.request_lock);
    return ret;
}

int usbaudio_stop(void)
{
    int16_t *ring;
    int ret = request(REQUEST_QUIT, &(const struct request_args) { 0 });

    if (ret != 0) {
        return ret;
    }
    (void) tx_mutex_get(&audio.request_lock, TX_WAIT_FOREVER);
    audio.state = AUDIO_STOPPED;
    (void) tx_mutex_get(&audio.data_lock, TX_WAIT_FOREVER);
    ring = audio.ring;
    audio.ring = NULL;
    audio.stats.streaming = false;
    (void) tx_mutex_put(&audio.data_lock);
    (void) tx_mutex_put(&audio.request_lock);
    free(ring);
    /* A consumer waiting for samples sees that no stream runs. */
    (void) tx_event_flags_set(&audio.ring_events, RING_EV_DATA, TX_OR);
    return 0;
}

int usbaudio_info(struct usbaudio_info *info)
{
    memset(info, 0, sizeof(*info));
    return request(REQUEST_INFO, &(const struct request_args) { .info = info });
}

int usbaudio_stream_start(uint32_t rate, uint8_t channels)
{
    return request(REQUEST_START, &(const struct request_args) { .rate = rate, .channels = channels });
}

int usbaudio_stream_stop(void)
{
    int ret = request(REQUEST_STOP, &(const struct request_args) { 0 });

    /* A consumer waiting for samples sees that the stream stopped. */
    if (audio.created) {
        (void) tx_event_flags_set(&audio.ring_events, RING_EV_DATA, TX_OR);
    }
    return ret;
}

int usbaudio_volume_get(struct usbaudio_volume *volume)
{
    const struct request_args args = { .volume = volume };

    memset(volume, 0, sizeof(*volume));
    return request(REQUEST_VOLUME_GET, &args);
}

int usbaudio_volume_set(uint8_t percent)
{
    const struct request_args args = { .percent = percent };

    return request(REQUEST_VOLUME_SET, &args);
}

int usbaudio_mute_set(bool mute)
{
    const struct request_args args = { .mute = mute };

    return request(REQUEST_MUTE_SET, &args);
}

int usbaudio_hum_set(unsigned int hz)
{
    const struct request_args args = { .hum_hz = (uint8_t) hz };
    int ret;

    if (hz != 0u && hz != 50u && hz != 60u) {
        return -EINVAL;
    }
    ret = objects_create();
    if (ret != 0) {
        return ret;
    }
    ret = request(REQUEST_HUM_SET, &args);
    if (ret == -EALREADY) {
        /* No task: taken at the next stream start. */
        (void) tx_mutex_get(&audio.request_lock, TX_WAIT_FOREVER);
        audio.hum_hz = (uint8_t) hz;
        (void) tx_mutex_put(&audio.request_lock);
        ret = 0;
    }
    return ret;
}

unsigned int usbaudio_hum_get(void)
{
    return audio.created ? audio.hum_hz : 50u;
}

int usbaudio_stats(struct usbaudio_stats *stats)
{
    if (!audio.created) {
        memset(stats, 0, sizeof(*stats));
        return 0;
    }
    (void) tx_mutex_get(&audio.data_lock, TX_WAIT_FOREVER);
    *stats = audio.stats;
    (void) tx_mutex_put(&audio.data_lock);
    return 0;
}

int usbaudio_level(struct usbaudio_level *level)
{
    if (!audio.created) {
        memset(level, 0, sizeof(*level));
        return 0;
    }
    (void) tx_mutex_get(&audio.data_lock, TX_WAIT_FOREVER);
    *level = audio.level;
    (void) tx_mutex_put(&audio.data_lock);
    return 0;
}

int usbaudio_read_16k(int16_t *dst, size_t n, uint32_t timeout_ms)
{
    ULONG start = tx_time_get();
    ULONG timeout = ms_to_ticks(timeout_ms);
    size_t copied = 0;

    if (!audio.created) {
        return -ENODATA;
    }
    while (copied < n) {
        uint32_t avail;
        ULONG elapsed;
        ULONG actual;

        (void) tx_mutex_get(&audio.data_lock, TX_WAIT_FOREVER);
        if (audio.ring == NULL || !audio.stats.streaming) {
            (void) tx_mutex_put(&audio.data_lock);
            return copied > 0u ? (int) copied : -ENODATA;
        }
        if (audio.stats.out_rate != 16000u) {
            (void) tx_mutex_put(&audio.data_lock);
            return -ENOTSUP;
        }
        avail = audio.written - audio.consumed;
        if (avail > USBAUDIO_RING_SAMPLES) {
            audio.stats.ring_lost += avail - USBAUDIO_RING_SAMPLES;
            audio.consumed = audio.written - USBAUDIO_RING_SAMPLES;
            avail = USBAUDIO_RING_SAMPLES;
        }
        while (avail > 0u && copied < n) {
            dst[copied++] = audio.ring[audio.consumed & RING_MASK];
            audio.consumed++;
            avail--;
        }
        if (copied < n) {
            /* Cleared under the lock: a sample written after this sets it again. */
            (void) tx_event_flags_set(&audio.ring_events, ~RING_EV_DATA, TX_AND);
        }
        (void) tx_mutex_put(&audio.data_lock);
        if (copied >= n) {
            break;
        }
        elapsed = tx_time_get() - start;
        if (elapsed >= timeout
            || tx_event_flags_get(&audio.ring_events, RING_EV_DATA, TX_OR, &actual, timeout - elapsed)
                   != TX_SUCCESS) {
            break;
        }
    }
    return (int) copied;
}

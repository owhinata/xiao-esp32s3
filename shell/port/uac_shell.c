/* SPDX-License-Identifier: MIT */
/*
 * "uac": the USB audio input of port/usb/audio.h (a USB Audio Class 1.0
 * or 2.0 microphone on the USB host of "usb"), written as a Zephyr shell
 * command.
 *
 *   uac info                      the open microphone interface: its device,
 *                                 its formats (channels, bits, sampling
 *                                 rates) and, while streaming, its volume
 *                                 and mute
 *   uac start [<rate> [<ch>]]     start the stream (default 48000 Hz, 1
 *                                 channel, 16 bits)
 *   uac stop                      stop it
 *   uac volume [<0..100>]         the volume (percent of the device's range
 *                                 and dB) and mute, or set the volume; while
 *                                 the stream runs, set again at every start
 *   uac mute on|off               set the mute, the same way
 *   uac level [<seconds>]         the peak and RMS level of the decimated
 *                                 samples, in dBFS, four times a second for
 *                                 the time given (default 5 s)
 *   uac dump [<samples>]          that many consecutive 16 kHz samples
 *                                 (default 1600, at most 16000), read with
 *                                 usbaudio_read_16k() into a buffer first,
 *                                 then printed 16 per line and "END <count>"
 *   uac stats                     what the stream has delivered: counters,
 *                                 the measured rate against the nominal one
 *
 * dBFS: relative to a full-scale square wave (a full-scale sine is
 * -3.0 dBFS RMS).  The arithmetic is integer.
 */
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

#include "usb/audio.h"

#define LEVEL_PERIOD_MS 250
#define LEVEL_SECONDS_MAX 3600u

static int report(const struct shell *sh, int ret)
{
    switch (ret) {
    case 0:
        break;
    case -EALREADY:
        shell_error(sh, "The USB host is not running (\"usb host start\")");
        break;
    case -ENODEV:
        shell_error(sh, "No microphone");
        break;
    case -EBUSY:
        shell_error(sh, "The stream is already running");
        break;
    case -ESRCH:
        shell_error(sh, "The stream is not running");
        break;
    case -ENOTSUP:
        shell_error(sh, "The microphone has no such format (\"uac info\")");
        break;
    case -EINVAL:
        shell_error(sh, "Not a format this input takes (1 or 2 channels, up to 48000 Hz stereo)");
        break;
    case -ENOMEM:
        shell_error(sh, "Out of memory");
        break;
    case -ETIMEDOUT:
        shell_error(sh, "The audio task did not answer; it stays as it is until a reboot");
        break;
    default:
        shell_error(sh, "Failed (%d); \"dmesg\" shows why", ret);
        break;
    }
    return ret;
}

/* ---- dBFS ---------------------------------------------------------------- */

/* log2(x) in Q16, x > 0. */
static int32_t log2_q16(uint64_t x)
{
    int n = 63 - __builtin_clzll(x);
    uint64_t y = n >= 30 ? x >> (n - 30) : x << (30 - n);  /* [1, 2) in Q30 */
    int32_t r = n * 65536;

    for (int32_t bit = 1 << 15; bit != 0; bit >>= 1) {
        y = (y * y) >> 30;
        if (y >= (UINT64_C(1) << 31)) {
            y >>= 1;
            r += bit;
        }
    }
    return r;
}

/* 10 log10(2^(l / 65536)) in tenths of a dB. */
static int32_t tenths_of_db(int32_t l)
{
    int64_t t = (int64_t) l * 30103;

    return (int32_t) ((t + (t < 0 ? -32768000 : 32768000)) / 65536000);
}

static void print_db(char *buf, size_t size, int32_t tenths)
{
    int32_t a = tenths < 0 ? -tenths : tenths;

    (void) snprintf(buf, size, "%s%d.%d", tenths < 0 ? "-" : "", (int) (a / 10), (int) (a % 10));
}

/* The window's peak and RMS in dBFS, "-inf" for silence. */
static void level_text(const struct usbaudio_level *lv, char *peak, char *rms, size_t size)
{
    if (lv->peak == 0u) {
        (void) snprintf(peak, size, "-inf");
    } else {
        print_db(peak, size, tenths_of_db(2 * (log2_q16(lv->peak) - 15 * 65536)));
    }
    if (lv->sum_squares == 0u || lv->samples == 0u) {
        (void) snprintf(rms, size, "-inf");
    } else {
        print_db(rms, size, tenths_of_db(log2_q16(lv->sum_squares) - log2_q16(lv->samples)
                                         - 30 * 65536));
    }
}

/* A volume in 1/256 dB, as "-1.50 dB". */
static void print_volume_db(char *buf, size_t size, int16_t volume)
{
    int32_t hundredths = ((int32_t) volume * 100) / 256;
    int32_t a = hundredths < 0 ? -hundredths : hundredths;

    (void) snprintf(buf, size, "%s%d.%02d dB", hundredths < 0 ? "-" : "", (int) (a / 100),
                    (int) (a % 100));
}

/* ---- uac info ------------------------------------------------------------ */

static int cmd_uac_info(const struct shell *sh, size_t argc, char **argv)
{
    struct usbaudio_info *info = malloc(sizeof(*info));
    int ret;

    (void) argc;
    (void) argv;

    if (info == NULL) {
        return report(sh, -ENOMEM);
    }
    ret = usbaudio_info(info);
    if (ret != 0) {
        free(info);
        return report(sh, ret);
    }
    if (!info->open) {
        free(info);
        shell_print(sh, "No microphone");
        return 0;
    }
    shell_print(sh, "microphone: UAC %u.0, device %u interface %u, %04x:%04x, \"%s\", \"%s\"",
                info->uac_version, info->address, info->interface, info->vid, info->pid,
                info->manufacturer, info->product);
    for (uint8_t i = 0; i < info->alt_count; i++) {
        const struct usbaudio_alt *a = &info->alts[i];
        char rates[64];
        int n = 0;

        if (info->uac_version == 2u) {
            (void) snprintf(rates, sizeof(rates), "rates of clock %u", info->clock_id);
        } else if (a->rate_count == 0u) {
            (void) snprintf(rates, sizeof(rates), "%u .. %u Hz", (unsigned int) a->rate_min,
                            (unsigned int) a->rate_max);
        } else {
            rates[0] = '\0';
            for (uint8_t j = 0; j < a->rate_count && n >= 0 && (size_t) n < sizeof(rates); j++) {
                n += snprintf(rates + n, sizeof(rates) - (size_t) n, "%s%u", j == 0u ? "" : " ",
                              (unsigned int) a->rates[j]);
            }
            if (n >= 0 && (size_t) n < sizeof(rates)) {
                (void) snprintf(rates + n, sizeof(rates) - (size_t) n, " Hz");
            }
        }
        shell_print(sh, "alternate %u: %u channel%s, %u bits in %u bytes, %s", i + 1u, a->channels,
                    a->channels == 1u ? "" : "s", a->bits, a->subframe, rates);
    }
    if (info->uac_version == 2u) {
        shell_print(sh, "clock %u: %u Hz now, %u range%s%s", info->clock_id,
                    (unsigned int) info->clock_rate, info->clock_range_count,
                    info->clock_range_count == 1u ? "" : "s",
                    info->clock_range_count == 0u ? " (not read)" : ":");
        for (uint8_t i = 0; i < info->clock_range_count; i++) {
            const struct usbaudio_rate_range *rr = &info->clock_ranges[i];

            shell_print(sh, "  %u .. %u Hz, step %u", (unsigned int) rr->min, (unsigned int) rr->max,
                        (unsigned int) rr->res);
        }
        if (info->volume_range_valid) {
            char lo[16];
            char hi[16];
            char step[16];

            print_volume_db(lo, sizeof(lo), info->volume_min);
            print_volume_db(hi, sizeof(hi), info->volume_max);
            print_volume_db(step, sizeof(step), info->volume_res);
            shell_print(sh, "feature unit %u: volume %s .. %s, step %s", info->feature_unit, lo, hi, step);
        } else {
            shell_print(sh, "feature unit %u: no volume range%s", info->feature_unit,
                        info->feature_unit == 0u ? " (no feature unit)" : "");
        }
    }
    if (!info->streaming) {
        shell_print(sh, "stream: stopped (volume and mute are read while it runs)");
    } else {
        char volume[16];

        if (info->volume_valid) {
            print_volume_db(volume, sizeof(volume), info->volume);
        } else {
            (void) snprintf(volume, sizeof(volume), "no control");
        }
        shell_print(sh, "stream: running; volume %s, mute %s", volume,
                    info->mute_valid ? (info->mute ? "on" : "off") : "no control");
    }
    free(info);
    return 0;
}

/* ---- uac start / stop ---------------------------------------------------- */

static bool parse_u32(const char *s, uint32_t *out)
{
    char *end;
    unsigned long v = strtoul(s, &end, 10);

    if (*s == '\0' || *end != '\0' || v > UINT32_MAX) {
        return false;
    }
    *out = (uint32_t) v;
    return true;
}

static int cmd_uac_start(const struct shell *sh, size_t argc, char **argv)
{
    uint32_t rate = 48000;
    uint32_t channels = 1;
    int ret;

    if (argc > 1 && !parse_u32(argv[1], &rate)) {
        shell_error(sh, "Not a sampling rate: %s", argv[1]);
        return -EINVAL;
    }
    if (argc > 2 && (!parse_u32(argv[2], &channels) || channels < 1u || channels > 2u)) {
        shell_error(sh, "Not a channel count (1 or 2): %s", argv[2]);
        return -EINVAL;
    }
    ret = usbaudio_stream_start(rate, (uint8_t) channels);
    if (ret == 0) {
        shell_print(sh, "Streaming: %u Hz, %u channel%s, 16 bits; decimated to %u Hz",
                    (unsigned int) rate, (unsigned int) channels, channels == 1u ? "" : "s",
                    (unsigned int) (rate == 16000u ? rate : rate / 3u));
    }
    return report(sh, ret);
}

static int cmd_uac_stop(const struct shell *sh, size_t argc, char **argv)
{
    int ret;

    (void) argc;
    (void) argv;

    ret = usbaudio_stream_stop();
    if (ret == 0) {
        shell_print(sh, "Stopped");
    }
    return report(sh, ret);
}

/* ---- uac volume / mute --------------------------------------------------- */

/* The errors the controls add to report()'s. */
static int report_control(const struct shell *sh, int ret)
{
    switch (ret) {
    case -ESRCH:
        shell_error(sh, "Volume and mute are read and set while the stream runs (\"uac start\")");
        return ret;
    case -ENOTSUP:
        shell_error(sh, "The microphone has no such control");
        return ret;
    default:
        return report(sh, ret);
    }
}

static int cmd_uac_volume(const struct shell *sh, size_t argc, char **argv)
{
    struct usbaudio_volume v;
    char percent[8];
    char db[16];
    char set[24];
    uint32_t value;
    int ret;

    if (argc > 1) {
        if (!parse_u32(argv[1], &value) || value > 100u) {
            shell_error(sh, "Not a volume (0 .. 100): %s", argv[1]);
            return -EINVAL;
        }
        ret = usbaudio_volume_set((uint8_t) value);
        if (ret != 0) {
            return report_control(sh, ret);
        }
    }
    ret = usbaudio_volume_get(&v);
    if (ret != 0) {
        return report_control(sh, ret);
    }
    if (v.percent_valid) {
        (void) snprintf(percent, sizeof(percent), "%u%%", v.percent);
    } else {
        (void) snprintf(percent, sizeof(percent), "?");
    }
    if (v.db_valid) {
        print_volume_db(db, sizeof(db), v.db);
    } else {
        (void) snprintf(db, sizeof(db), "? dB");
    }
    if (v.set) {
        (void) snprintf(set, sizeof(set), "set to %u%%", v.set_percent);
    } else {
        (void) snprintf(set, sizeof(set), "device default");
    }
    shell_print(sh, "volume %s (%s), %s; mute %s", percent, db, set,
                v.mute_valid ? (v.mute ? "on" : "off") : "no control");
    return 0;
}

static int cmd_uac_mute(const struct shell *sh, size_t argc, char **argv)
{
    bool mute;
    int ret;

    (void) argc;
    if (strcmp(argv[1], "on") == 0) {
        mute = true;
    } else if (strcmp(argv[1], "off") == 0) {
        mute = false;
    } else {
        shell_error(sh, "Not on or off: %s", argv[1]);
        return -EINVAL;
    }
    ret = usbaudio_mute_set(mute);
    if (ret != 0) {
        return report_control(sh, ret);
    }
    shell_print(sh, "Mute %s", mute ? "on" : "off");
    return 0;
}

/* ---- uac level ----------------------------------------------------------- */

static int cmd_uac_level(const struct shell *sh, size_t argc, char **argv)
{
    uint32_t seconds = 5;
    uint32_t last_seq = 0;
    struct usbaudio_stats st;

    if (argc > 1 && (!parse_u32(argv[1], &seconds) || seconds < 1u || seconds > LEVEL_SECONDS_MAX)) {
        shell_error(sh, "Not a number of seconds (1 .. %u): %s", LEVEL_SECONDS_MAX, argv[1]);
        return -EINVAL;
    }
    (void) usbaudio_stats(&st);
    if (!st.streaming) {
        return report(sh, -ESRCH);
    }
    for (uint32_t i = 0; i < seconds * (1000u / LEVEL_PERIOD_MS); i++) {
        struct usbaudio_level lv;
        char peak[16];
        char rms[16];

        k_msleep(LEVEL_PERIOD_MS);
        (void) usbaudio_stats(&st);
        if (!st.streaming) {
            return report(sh, -ESRCH);
        }
        (void) usbaudio_level(&lv);
        if (lv.seq == last_seq) {
            shell_print(sh, "no new samples");
            continue;
        }
        last_seq = lv.seq;
        level_text(&lv, peak, rms, sizeof(peak));
        shell_print(sh, "peak %6s dBFS  RMS %6s dBFS", peak, rms);
    }
    return 0;
}

/* ---- uac dump ------------------------------------------------------------ */

#define DUMP_DEFAULT    1600u
#define DUMP_MAX        16000u
#define DUMP_CHUNK      1600u
#define DUMP_WAIT_MS    1000u

/*
 * The capture goes into the buffer first and is printed after, so that the
 * output's pace cannot make the reader fall behind the ring.  The first
 * read may skip what the ring no longer holds (a reader that has not read
 * for a while); a skip after it is a gap in the capture, which the END
 * line reports.
 */
static int cmd_uac_dump(const struct shell *sh, size_t argc, char **argv)
{
    uint32_t want = DUMP_DEFAULT;
    uint32_t got = 0;
    uint32_t lost_after_first = 0;
    struct usbaudio_stats st;
    const char *why = "";
    int16_t *buf;
    char line[16 * 7 + 1];

    if (argc > 1 && (!parse_u32(argv[1], &want) || want < 1u || want > DUMP_MAX)) {
        shell_error(sh, "Not a number of samples (1 .. %u): %s", DUMP_MAX, argv[1]);
        return -EINVAL;
    }
    buf = malloc(want * sizeof(*buf));
    if (buf == NULL) {
        return report(sh, -ENOMEM);
    }
    while (got < want) {
        uint32_t n = want - got < DUMP_CHUNK ? want - got : DUMP_CHUNK;
        int ret = usbaudio_read_16k(&buf[got], n, DUMP_WAIT_MS);

        if (ret < 0) {
            if (got == 0u) {
                free(buf);
                if (ret == -ENOTSUP) {
                    shell_error(sh, "The stream does not give 16 kHz samples (it runs at neither 16 nor 48 kHz)");
                    return ret;
                }
                return report(sh, -ESRCH);
            }
            why = ", the stream stopped";
            break;
        }
        if (got == 0u) {
            (void) usbaudio_stats(&st);
            lost_after_first = st.ring_lost;
        }
        got += (uint32_t) ret;
        if ((uint32_t) ret < n) {
            why = ", timed out waiting for samples";
            break;
        }
    }
    (void) usbaudio_stats(&st);

    for (uint32_t i = 0; i < got; i += 16u) {
        size_t len = 0;

        for (uint32_t j = i; j < got && j < i + 16u; j++) {
            len += (size_t) snprintf(line + len, sizeof(line) - len, "%s%d", j == i ? "" : " ", buf[j]);
        }
        shell_print(sh, "%s", line);
    }
    free(buf);
    if (st.ring_lost != lost_after_first) {
        shell_print(sh, "END %u (gap: %u samples overwritten during the capture%s)", (unsigned int) got,
                    (unsigned int) (st.ring_lost - lost_after_first), why);
    } else {
        shell_print(sh, "END %u%s", (unsigned int) got, why);
    }
    return 0;
}

/* ---- uac stats ----------------------------------------------------------- */

static void print_seconds(char *buf, size_t size, int64_t us)
{
    if (us < 0) {
        us = 0;
    }
    (void) snprintf(buf, size, "%u.%03u", (unsigned int) (us / 1000000),
                    (unsigned int) ((us / 1000) % 1000));
}

static int cmd_uac_stats(const struct shell *sh, size_t argc, char **argv)
{
    struct usbaudio_stats st;
    char a[24];
    char b[24];

    (void) argc;
    (void) argv;

    (void) usbaudio_stats(&st);
    if (st.rate == 0u) {
        shell_print(sh, "No stream since the USB host started");
        return 0;
    }
    shell_print(sh, "stream: %s, %u Hz, %u channel%s, decimated to %u Hz",
                st.streaming ? "running" : "stopped", (unsigned int) st.rate, st.channels,
                st.channels == 1u ? "" : "s", (unsigned int) st.out_rate);
    if (st.first_us == 0) {
        shell_print(sh, "data: none yet");
    } else {
        print_seconds(a, sizeof(a), st.first_us - st.start_us);
        print_seconds(b, sizeof(b), st.last_us - st.start_us);
        shell_print(sh, "data: from %s s to %s s after the start", a, b);
    }
    shell_print(sh, "bytes: %" PRIu64 ", frames: %" PRIu64 ", decimated samples: %" PRIu64,
                st.bytes, st.frames, st.out_samples);
    if (st.dc_valid) {
        shell_print(sh, "input DC: %d LSB (mean over the last second, before the 30 Hz high-pass)",
                    (int) st.dc);
    } else {
        shell_print(sh, "input DC: not yet (a second of input)");
    }
    shell_print(sh, "clipped: %" PRIu64 " input samples at full scale", st.clipped);
    if (st.last_us > st.first_us && st.frames > st.first_frames) {
        /* The frames after the first read, over the time between the first and the last. */
        uint64_t mrate = (st.frames - st.first_frames) * UINT64_C(1000000000)
                         / (uint64_t) (st.last_us - st.first_us);
        int64_t ppm = ((int64_t) mrate - (int64_t) st.rate * 1000) * 1000 / (int64_t) st.rate;

        shell_print(sh, "rate: %u.%03u frames/s measured over %u ms, nominal %u (%+d ppm)",
                    (unsigned int) (mrate / 1000u), (unsigned int) (mrate % 1000u),
                    (unsigned int) ((st.last_us - st.first_us) / 1000), (unsigned int) st.rate,
                    (int) ppm);
    }
    shell_print(sh, "reads: %u (short %u, empty %u, errors %u); RX_DONE events: %u",
                (unsigned int) st.reads, (unsigned int) st.short_reads,
                (unsigned int) st.empty_reads, (unsigned int) st.read_errors,
                (unsigned int) st.wakeups);
    shell_print(sh, "driver buffer: %u bytes, near full %u times; transfer errors: %u",
                (unsigned int) st.buffer_bytes, (unsigned int) st.near_full,
                (unsigned int) st.transfer_errors);
    if (st.uac_version == 2u) {
        shell_print(sh, "UAC 2.0: packets dropped (buffer full) %u, packets with errors %u",
                    (unsigned int) st.overruns, (unsigned int) st.packet_errors);
    }
    shell_print(sh, "16 kHz ring: %u samples, %u lost by the consumer",
                (unsigned int) USBAUDIO_RING_SAMPLES, (unsigned int) st.ring_lost);
    return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_uac,
    SHELL_CMD_ARG(dump, NULL, "Print consecutive 16 kHz samples: dump [<samples>] (default 1600)",
                  cmd_uac_dump, 1, 1),
    SHELL_CMD(info, NULL, "Show the microphone and its formats", cmd_uac_info),
    SHELL_CMD_ARG(level, NULL, "Show the level in dBFS: level [<seconds>] (default 5)",
                  cmd_uac_level, 1, 1),
    SHELL_CMD_ARG(start, NULL, "Start the stream: start [<rate> [<channels>]] (default 48000 1)",
                  cmd_uac_start, 1, 2),
    SHELL_CMD_ARG(mute, NULL, "Set the mute while streaming: mute on|off", cmd_uac_mute, 2, 0),
    SHELL_CMD(stats, NULL, "Show what the stream has delivered", cmd_uac_stats),
    SHELL_CMD(stop, NULL, "Stop the stream", cmd_uac_stop),
    SHELL_CMD_ARG(volume, NULL, "Show the volume and mute, or set the volume while streaming: "
                  "volume [<0..100>]", cmd_uac_volume, 1, 1),
    SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(uac, &sub_uac, "USB audio input (UAC 1.0 or 2.0 microphone)", NULL);

// SPDX-License-Identifier: MIT
using AudioStudio.Core.Audio;
using AudioStudio.Core.Dsp;
using FluentAssertions;
using Xunit;

namespace AudioStudio.Core.Tests;

public class DspTests
{
    internal static FileSource Sine(double hz, double amplitude, double seconds, int rate = 16000, double hum = 0)
    {
        short[] samples = new short[(int)(seconds * rate)];
        for (int i = 0; i < samples.Length; i++)
        {
            double t = (double)i / rate;
            double v = amplitude * Math.Sin(2 * Math.PI * hz * t) + hum * Math.Sin(2 * Math.PI * 50 * t);
            samples[i] = (short)Math.Round(v * 32767);
        }
        return new FileSource("sine", rate, samples);
    }

    [Fact]
    public void Spectrum_reads_a_sine_in_dbfs_at_its_frequency()
    {
        // 1 kHz is bin 128 of 2048 at 16 kHz: exactly on a bin.
        FileSource source = Sine(1000, 0.5, 1.0);
        Spectrogram s = new(16000, 2048, 256, 64, 8000, logFrequency: false);
        float[] bins = s.Bins(source, 8000);
        bins.Should().HaveCount(1024);
        int peak = Array.IndexOf(bins, bins.Max());
        (peak * s.BinHz).Should().BeApproximately(1000, 0.01);
        bins[peak].Should().BeApproximately(-6.02f, 0.1f);
        bins[peak + 10].Should().BeLessThan(-80f);
        SpectrumColumn column = s.Column(source, 8000 / 256)!;
        column.PeakHz.Should().BeApproximately(1000f, 0.01f);
        column.PeakDb.Should().BeApproximately(-6.02f, 0.1f);
        // Row 8 of 64 on a linear axis to 8 kHz covers 1000..1125 Hz.
        column.Rows[8].Should().BeApproximately(-6.02f, 0.1f);
        column.Rows[40].Should().BeLessThan(-80f);
    }

    [Fact]
    public void Fft_4096_resolves_mains_hum_next_to_a_low_tone()
    {
        // 50 Hz hum and a tone at 78.125 Hz (bin 20), 3.9 Hz bins: two separate peaks.
        FileSource source = Sine(78.125, 0.1, 1.0, hum: 0.01);
        Spectrogram s = new(16000, 4096, 512, 256, 8000, logFrequency: true);
        float[] bins = s.Bins(source, 16000);
        double binHz = s.BinHz;
        float at50 = bins[(int)Math.Round(50 / binHz)], at60 = bins[(int)Math.Round(60 / binHz)];
        float atTone = bins[20];
        at50.Should().BeApproximately(-40f, 1.5f);
        atTone.Should().BeApproximately(-20f, 1.5f);
        at60.Should().BeLessThan(at50 - 15, "the two lines are resolved");
    }

    [Fact]
    public void Logarithmic_axis_spreads_the_low_end()
    {
        Spectrogram s = new(16000, 4096, 512, 256, 8000, logFrequency: true);
        s.Frequency(0).Should().BeApproximately(Spectrogram.LogFloorHz, 1e-9);
        s.Frequency(1).Should().BeApproximately(8000, 1e-6);
        s.HeightOf(s.Frequency(0.3)).Should().BeApproximately(0.3, 1e-9);
        // 50 Hz sits a quarter of the way up: the band below 200 Hz gets 45 % of the rows.
        s.HeightOf(50).Should().BeApproximately(0.24, 0.01);
        s.HeightOf(200).Should().BeApproximately(0.45, 0.01);
    }

    [Fact]
    public void Columns_are_kept_and_null_past_the_end()
    {
        FileSource source = Sine(440, 0.25, 0.5);
        Spectrogram s = new(16000, 1024, 160, 32, 8000, logFrequency: false);
        s.Cached(10).Should().BeNull();
        SpectrumColumn a = s.Column(source, 10)!;
        s.Cached(10).Should().BeSameAs(a);
        s.Column(source, 10).Should().BeSameAs(a);
        s.Column(source, 8000 / 160 + 1).Should().BeNull();
        s.Column(source, 0).Should().BeNull();
    }

    [Fact]
    public void A_column_stands_for_the_centre_of_its_window()
    {
        Spectrogram s = new(16000, 2048, 256, 32, 8000, logFrequency: false);
        s.CentreOf(8).Should().Be(1024);
        // Centres from 1000 up to 16000 (excluded): k = 8 (1024) .. 66 (15872).
        (long first, long last) = s.ColumnsCentredIn(1000, 16000);
        first.Should().Be(8);
        last.Should().Be(66);
        s.CentreOf(last).Should().BeLessThan(16000);
        s.CentreOf(last + 1).Should().BeGreaterThanOrEqualTo(16000);
    }

    [Fact]
    public void Level_is_in_dbfs_as_the_board_prints_it()
    {
        float[] square = Enumerable.Range(0, 1000).Select(i => i % 2 == 0 ? 1f : -1f).ToArray();
        Level.Measure(square).Should().Be((0.0, 0.0));
        float[] sine = Enumerable.Range(0, 16000).Select(i => 0.5f * MathF.Sin(2 * MathF.PI * 1000 * i / 16000)).ToArray();
        (double peak, double rms) = Level.Measure(sine);
        peak.Should().BeApproximately(-6.02, 0.01);
        rms.Should().BeApproximately(-9.03, 0.01);
        Level.Measure(new float[10]).PeakDb.Should().Be(double.NegativeInfinity);
    }
}

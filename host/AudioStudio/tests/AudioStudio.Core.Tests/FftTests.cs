// SPDX-License-Identifier: MIT
using AudioStudio.Core.Dsp;
using FluentAssertions;
using Xunit;

namespace AudioStudio.Core.Tests;

public class FftTests
{
    [Fact]
    public void Fft_finds_a_sine()
    {
        const int n = 256;
        float[] re = new float[n], im = new float[n];
        for (int i = 0; i < n; i++)
        {
            re[i] = 3f * MathF.Cos(2 * MathF.PI * 10 * i / n) + 1f;
        }
        new Fft(n).Transform(re, im);
        MathF.Sqrt(re[10] * re[10] + im[10] * im[10]).Should().BeApproximately(3f * n / 2, 1e-2f);
        re[0].Should().BeApproximately(n, 1e-2f);
        MathF.Sqrt(re[11] * re[11] + im[11] * im[11]).Should().BeLessThan(1e-2f);
    }

    [Fact]
    public void Fft_needs_a_power_of_two()
    {
        Action make = () => _ = new Fft(1000);
        make.Should().Throw<ArgumentException>();
    }
}

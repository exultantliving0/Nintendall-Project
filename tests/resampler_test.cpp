// Feeds a 440 Hz tone through the resampler at the input rates real cores use
// and checks the output is the same tone, at the right length, without noise.
// Build: g++ -O2 -std=c++17 -Isrc tests/resampler_test.cpp src/audio.cpp $(pkg-config --cflags --libs sdl2)
#include <cmath>
#include <cstdio>
#include <vector>

#include "audio.hpp"

int main() {
    const double outRate = 48000, tone = 440;
    int failures = 0;
    for (double inRate : {2097152.0, 65536.0, 48000.0, 44100.0, 32040.0}) {
        Resampler r;
        std::vector<int16_t> out;
        const int perFrame = int(inRate / 60);
        std::vector<int16_t> in(size_t(perFrame) * 2);
        long n = 0;
        for (int f = 0; f < 120; ++f) {  // 2 seconds in 60 fps chunks, like a core
            for (int i = 0; i < perFrame; ++i, ++n) {
                const int16_t v = int16_t(12000 * std::sin(2 * M_PI * tone * n / inRate));
                in[size_t(i) * 2] = in[size_t(i) * 2 + 1] = v;
            }
            r.process(in.data(), size_t(perFrame), outRate / inRate, out);
        }
        // Fit the expected sine to the output (skip the first 0.1 s) and measure what's left over.
        const size_t frames = out.size() / 2, start = 4800;
        double c = 0, s = 0;
        for (size_t i = start; i < frames; ++i) {
            const double t = 2 * M_PI * tone * double(i) / outRate;
            c += out[i * 2] * std::cos(t);
            s += out[i * 2] * std::sin(t);
        }
        c *= 2.0 / double(frames - start);
        s *= 2.0 / double(frames - start);
        double sig = 0, noise = 0;
        for (size_t i = start; i < frames; ++i) {
            const double t = 2 * M_PI * tone * double(i) / outRate;
            const double fit = c * std::cos(t) + s * std::sin(t);
            sig += fit * fit;
            noise += (out[i * 2] - fit) * (out[i * 2] - fit);
        }
        const double snr = 10 * std::log10(sig / noise);
        const double expected = double(n) * outRate / inRate;
        const double lengthErr = std::fabs(double(frames) - expected) / expected * 100;
        const bool ok = snr > 30 && lengthErr < 0.1 && std::hypot(c, s) > 11000;
        failures += !ok;
        std::printf("%-4s in %8.0f Hz: %zu frames out (%.3f%% off), amplitude %.0f, signal/noise %.1f dB\n",
                    ok ? "ok" : "FAIL", inRate, frames, lengthErr, std::hypot(c, s), snr);
    }
    return failures;
}

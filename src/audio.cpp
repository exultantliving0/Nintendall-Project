#include "audio.hpp"

#include <algorithm>
#include <cstdio>

namespace {
constexpr double kTargetLatency = 0.064;  // seconds of audio we try to keep queued
constexpr double kMaxLatency = 0.25;      // beyond this we drop the queue and resync
constexpr double kMaxRateDelta = 0.005;   // +-0.5% pitch adjustment, inaudible
}  // namespace

bool Audio::init() {
    SDL_AudioSpec want{}, have{};
    want.freq = 48000;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 512;
    dev_ = SDL_OpenAudioDevice(nullptr, 0, &want, &have, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    if (!dev_) {
        std::fprintf(stderr, "Audio disabled: %s\n", SDL_GetError());
        return false;
    }
    outRate_ = have.freq;
    SDL_PauseAudioDevice(dev_, 0);
    return true;
}

void Audio::shutdown() {
    if (dev_) SDL_CloseAudioDevice(dev_);
    dev_ = 0;
}

void Audio::setInputRate(double hz) {
    if (hz <= 0 || hz == inRate_) return;
    inRate_ = hz;
    resampler_.reset();
}

double Audio::queuedSeconds() const {
    return dev_ ? double(SDL_GetQueuedAudioSize(dev_)) / (4.0 * outRate_) : 0.0;
}

void Audio::clear() {
    if (dev_) SDL_ClearQueuedAudio(dev_);
    resampler_.reset();
}

void Audio::setPaused(bool paused) {
    if (dev_) SDL_PauseAudioDevice(dev_, paused ? 1 : 0);
}

void Audio::push(const int16_t* in, size_t frames) {
    if (!dev_ || frames == 0) return;
    double queued = queuedSeconds();
    if (queued > kMaxLatency) {
        SDL_ClearQueuedAudio(dev_);
        queued = 0;
    }
    if (queued == 0) {
        // Starting (or recovering from) an empty buffer: pre-fill with silence so we don't underrun.
        std::vector<int16_t> silence(size_t(kTargetLatency * outRate_ * 0.5) * 2, 0);
        SDL_QueueAudio(dev_, silence.data(), Uint32(silence.size() * sizeof(int16_t)));
        queued = queuedSeconds();
    }

    // Below target -> produce slightly more samples, above -> slightly fewer.
    const double err = std::clamp((kTargetLatency - queued) / kTargetLatency, -1.0, 1.0);
    const double ratio = (outRate_ / inRate_) * (1.0 + kMaxRateDelta * err);
    out_.clear();
    resampler_.process(in, frames, ratio, out_);
    SDL_QueueAudio(dev_, out_.data(), Uint32(out_.size() * sizeof(int16_t)));
}

// ---------------------------------------------------------------------------

void Resampler::reset() {
    phase_ = 0;
    prevL_ = prevR_ = 0;
    accL_ = accR_ = 0;
    accN_ = 0;
}

void Resampler::process(const int16_t* in, size_t frames, double ratio, std::vector<int16_t>& out) {
    const double step = 1.0 / ratio;  // input samples consumed per output sample
    out.reserve(out.size() + size_t(frames * ratio * 2) + 8);
    if (step > 1.5) {
        // Downsampling a lot (e.g. Game Boy cores output ~2 MHz): average every input sample
        // that falls into each output sample. This filters out the high frequencies that
        // would otherwise alias into harsh noise.
        for (size_t i = 0; i < frames; ++i) {
            accL_ += in[i * 2];
            accR_ += in[i * 2 + 1];
            ++accN_;
            phase_ += 1.0;
            if (phase_ >= step) {
                out.push_back(int16_t(accL_ / accN_));
                out.push_back(int16_t(accR_ / accN_));
                accL_ = accR_ = 0;
                accN_ = 0;
                phase_ -= step;
            }
        }
        return;
    }
    // Similar rates or upsampling: linear interpolation between neighbouring samples.
    for (size_t i = 0; i < frames; ++i) {
        const int16_t l = in[i * 2], r = in[i * 2 + 1];
        while (phase_ < 1.0) {
            out.push_back(int16_t(prevL_ + (l - prevL_) * phase_));
            out.push_back(int16_t(prevR_ + (r - prevR_) * phase_));
            phase_ += step;
        }
        phase_ -= 1.0;
        prevL_ = l;
        prevR_ = r;
    }
}

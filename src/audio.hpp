#pragma once
#include <SDL.h>

#include <cstdint>
#include <vector>

// Converts stereo audio from one sample rate to another.
class Resampler {
public:
    void reset();
    // ratio = output rate / input rate. Appends converted samples to `out`.
    void process(const int16_t* in, size_t frames, double ratio, std::vector<int16_t>& out);

private:
    double phase_ = 0;
    int16_t prevL_ = 0, prevR_ = 0;
    int64_t accL_ = 0, accR_ = 0;  // running sums for averaging when downsampling
    int accN_ = 0;
};

// Plays core audio through SDL. Resamples from the core's rate to the device
// rate, nudging the ratio by up to 0.5% to keep the buffer level steady
// ("dynamic rate control"), so there's no crackle and no drift.
class Audio {
public:
    bool init();
    void shutdown();
    void setInputRate(double hz);
    void push(const int16_t* stereo, size_t frames);
    void clear();
    void setPaused(bool paused);
    double queuedSeconds() const;
    bool ok() const { return dev_ != 0; }

private:
    SDL_AudioDeviceID dev_ = 0;
    int outRate_ = 48000;
    double inRate_ = 48000;
    Resampler resampler_;
    std::vector<int16_t> out_;
};

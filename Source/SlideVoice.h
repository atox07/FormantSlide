#pragma once
#include "WaveLoader.h"
#include <signalsmith-stretch/signalsmith-stretch.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>

// One stereo shifter per sounding note. The sampler knob precedes the shifter.
class SlideVoice {
public:
    static constexpr int latencySamples = 1024;
    SlideVoice(std::shared_ptr<const WaveData>, int first, int last, double rate, double nominalHz);
    int render(float* left, float* right, int count, double scale);
    void jumpPitch() { snapPitch = true; }
    void setSlideActive(bool value) { slideActive = value; }
    void setPlaybackScale(double value) { playbackTarget = std::clamp(value, 1./64., 64.); }
    void release() { if (releaseAt < 0) releaseAt = clock; }
    bool hasSource() const { return sourceEnd < 0 && position < end; }

private:
    using Stereo = std::array<float, 2>;
    static constexpr int quantum = 32, maxInterval = 256, history = latencySamples + maxInterval;
    static constexpr int ringSize = 4096;
    struct Frame {
        Stereo dry{}, wet{};
        float pitch = 0, mix = 0, gain = 1, baseFrequency = 0;
    };
    Frame& frame(int64_t t) { return ring[static_cast<uint64_t>(t) & (ringSize - 1)]; }
    Stereo read(double where) const noexcept;
    void prime(int64_t start);
    void process(int count, int64_t start);
    struct ToneProbe {
        double previous = 0., older = 0., lastCrossing = -1., period = 0.;
        double tuned = 0., drive = 0., feedback = 0., decay = 0.;
        int consecutive = 0;
        void push(double sample, double frequency, int64_t time);
    };
    ToneProbe sourceTone{}, shiftedTone{};
    double correction = 0., trackedPitch = 0., trackedBase = 0.;
    Stereo mix(const Stereo& dry, const Stereo& wet, float amount);

    std::shared_ptr<const WaveData> wav;
    int begin, end;
    double step, position, playbackFollow, baseFrequency;
    double playbackTarget = 1., playback = 1., previousPitch = 0.;
    double dryPower = 0., wetPower = 0., crossPower = 0.;
    int64_t clock = 0, sourceEnd = -1, releaseAt = -1, wetUntil = -1;
    int fadeSamples, mixStep = 0, inputDelay = 0, wetPadding = 0;
    bool slideActive = false, snapPitch = false, running = false;
    signalsmith::stretch::SignalsmithStretch<float> stretch{314159L};
    std::array<Frame, ringSize> ring{};
    std::array<std::array<float, history>, 2> past{};
    std::array<std::array<float, quantum>, 2> in{}, out{};
};

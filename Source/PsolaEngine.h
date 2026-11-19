#pragma once
#include "WaveLoader.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>

class PsolaVoice {
public:
    static constexpr int latencySamples = 1024;

    PsolaVoice(std::shared_ptr<const WaveData>, int first, int last, double rate);
    int render(float* left, float* right, int count, double scale) noexcept;
    void jumpPitch() { snapPitch = true; }
    void setSlideActive(bool value) { slideActive = value; }
    void setPlaybackScale(double value) { playbackTarget = std::clamp(value, 1./64., 64.); }
    void release() { if (releaseAt < 0) releaseAt = clock; }
    bool hasSource() const { return sourceEnd < 0 && position < end; }

private:
    using Stereo = std::array<float, 2>;
    static constexpr int ringSize = 2 * latencySamples;
    static constexpr int tableSize = 1024;
    struct Input {
        Stereo dry{};
        double position = 0.;
        float weight = 0.f, mix = 0.f, gain = 1.f;
    };
    struct Grain {
        double center, end, inverseRadius, sourceOffset;
        int next;
    };

    static size_t ringIndex(int64_t frame) { return static_cast<uint64_t>(frame) & (ringSize - 1); }
    float voiceWeight(const VocalFrame&) const noexcept;
    Stereo read(double where) const noexcept;
    double epochCenter(double where, double period, bool half) const noexcept;
    void addGrain(double center, double source, double speed, const VocalFrame&, double ratio) noexcept;
    void activateGrains() noexcept;
    Stereo synthesise(const Input&) noexcept;

    std::shared_ptr<const WaveData> wav;
    int begin, end;
    double step, position, fixedLength, playbackFollow;
    float levelFollow, dryPower = 0.f, wetPower = 0.f, wetGain = 1.f;
    double playbackTarget = 1., playback = 1., previousPitch = 0., nextGrain = 0.;
    int64_t clock = 0, sourceEnd = -1, releaseAt = -1;
    int fadeSamples, mixStep = 0, freeGrain = 0, activeGrain = -1;
    bool slideActive = false, snapPitch = false, primed = false, halfPhase = false;

    std::array<Input, ringSize> input{};
    // At a minimum hop of two samples, the latency plus the longest grain
    // can leave at most approximately latencySamples grains queued or active.
    std::array<Grain, latencySamples + 32> grains{};
    std::array<int, ringSize> grainStarts{};
    std::array<float, tableSize + 1> hann{};
    std::array<Stereo, tableSize + 1> crossfade{};
};

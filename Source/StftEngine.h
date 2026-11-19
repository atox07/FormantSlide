#pragma once
#include "WaveLoader.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <kiss_fftr.h>

class StftVoice {
public:
    static constexpr int latencySamples = 2048;

    StftVoice(std::shared_ptr<const WaveData>, int, int, double, double);
    int render(float*, float*, int, double scale) noexcept;
    void jumpPitch() { snapPitch = true; }
    void setSlideActive(bool value) { slideActive = value; }
    void setPlaybackScale(double value) { playbackTarget = std::clamp(value, 1./64., 64.); }
    void setFormantScale(double v) { formant = std::clamp(v, 1./64., 64.); }
    void setFormantGuard(float v) { guard = v; }
    void setHighNoteLimit(float v) { limit = v; }
    void setHighNoteRelaxMax(float v) { relax = v; }
    void release() { if (releaseAt < 0) releaseAt = clock; }
    bool hasSource() const { return sourceEnd < 0 && position < end; }

private:
    using Stereo = std::array<float, 2>;
    static constexpr int maxBins = latencySamples / 2 + 1;
    static constexpr int ringSize = 2 * latencySamples;

    struct Input {
        Stereo sample{};
        float pitch = 0, formantScale = 1, mix = 0, gain = 1;
    };
    struct FreePlan {
        void operator()(kiss_fftr_state* plan) const noexcept { kiss_fftr_free(plan); }
    };

    void initStretch(double rate, double sourceF0);
    Stereo read(double where) const noexcept;
    void processFrame() noexcept;
    bool detectTransient(float flux, float magnitudeSum) noexcept;
    void estimateEnvelope() noexcept;
    static size_t ringIndex(int64_t frame) { return static_cast<uint64_t>(frame) & (ringSize - 1); }

    std::shared_ptr<const WaveData> wav;
    int begin, end;
    double step, startNote, position;
    double playbackTarget = 1, playback = 1, playbackFollow = 0;
    double previousPitch = 0, previousRatio = 1, formant = 1;
    float guard = 0, limit = 2000, relax = 0.75f;
    int64_t clock = 0, sourceEnd = -1, releaseAt = -1, nextFrame = 0, processThrough = -1;
    int fftSize = 0, hop = 0, bins = 0, lifter = 0, fadeSamples = 0, mixStep = 0;
    int fluxIndex = 0, fluxCount = 0, transientHold = 0;
    float fluxSum = 0;
    bool slideActive = false, snapPitch = false, haveSpectrum = false;
    std::unique_ptr<kiss_fftr_state, FreePlan> forward, inverse;
    std::array<Input, ringSize> input{};
    std::array<Stereo, ringSize> ola{};
    std::array<Stereo, latencySamples + 1> crossfade{};
    std::array<float, latencySamples> window{}, time{};
    std::array<float, maxBins> magnitude{}, previousMagnitude{}, envelope{};
    std::array<float, 8> fluxHistory{};
    std::array<int, maxBins> peaks{};
    std::array<float, maxBins> rotation{}, nextRotation{};
    std::array<std::array<kiss_fft_cpx, maxBins>, 2> spectrum{}, previous{}, shifted{};
    std::array<kiss_fft_cpx, maxBins> cepstrumSpectrum{};
};

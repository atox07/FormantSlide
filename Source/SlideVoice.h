#pragma once
#include "WaveLoader.h"
#include "VocalSpectrum.h"
#include <cstdint>
#include <memory>

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
    using Stereo = std::array<float,2>;
    static constexpr int fftSize = 1024, maxHop = 384;
    struct Frame {
        double position = 0., samplerStep = 1., wetStep = 1.;
        Stereo dry{};
        float mix = 0., gain = 1.;
    };
    std::shared_ptr<const WaveData> wav;
    int begin, end, hop, fadeSamples, mixStep = 0;
    double position, step, playbackFollow, nominalPeriod;
    double playback = 1., playbackTarget = 1., previousPitch = 0.;
    int64_t clock = 0, sourceEnd = -1, releaseAt = -1;
    bool slideActive = false, snapPitch = false;
    std::array<Frame,2*latencySamples> ring{};
    Frame& frame(int64_t t) { return ring[size_t(t) % ring.size()]; }
    Stereo read(double where) const noexcept;
    Stereo resample(double where, double speed) const;
    double align(double nominal, double previous, double speed, double period) const;
    void makeGrain(int64_t t);
    Stereo wet(int64_t t);
    Stereo mix(const Stereo& dry, const Stereo& wet, float amount);
    double dryPower = 0., wetPower = 0., crossPower = 0.;
    int grainOffset = -1;
    double nextAnchor = 0.;
    std::array<Stereo,2*maxHop> grain{};
    std::array<Stereo,maxHop> overlap{};
    std::array<float,maxHop> fade{};
    std::array<float,fftSize> guard{};
    std::array<std::complex<float>,fftSize> spectrum{};
};

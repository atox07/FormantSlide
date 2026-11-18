#pragma once
#include "WaveLoader.h"
#include <juce_dsp/juce_dsp.h>
#include <array>
#include <complex>

// A single synthesis spectrum for both analysis resolutions. Packed stereo
// gives both channels identical peak tracks and pitch trajectories.
class ResidualSpectrum {
    static constexpr int order = 11, size = 1<<order, half = size/2;
    static constexpr int shortSize = size/4, hop = size/8;
public:
    using Stereo = std::complex<float>;
    // Voices of one plugin instance are rendered serially. Plans belong to the
    // instance, while phase tracks, overlaps and all signal buffers are per voice.
    struct Plans {
        juce::dsp::FFT full{order}, shortWindow{order-2};
        Plans();
    };
    explicit ResidualSpectrum(Plans& shared) : plans(shared) {}
    Stereo next(const WaveData&, int first, int end, int at, double ratio);
    void reset() { active = false; }

private:
    Plans& plans;
    std::array<Stereo,size> input{}, bins{}, ring{};
    std::array<float,size> windowPower{};
    std::array<Stereo,shortSize> shortBins{};
    std::array<Stereo,half+1> left{}, right{}, shiftedL{}, shiftedR{};
    std::array<double,half+1> previousL{}, previousR{}, frequency{}, rotation{}, previousRotation{}, previousPeak{};
    std::array<double,half+2> sourceEnergy{}, shiftedEnergy{};
    std::array<double,shortSize/2+2> shortEnergy{};
    std::array<double,shortSize/2+1> previousBandPower{};
    std::array<float,shortSize/2+1> attack{};
    std::array<float,half+1> power{};
    std::array<int,half+1> peaks{};
    int cursor = 0;
    bool active = false;
    void frame(const WaveData&, int first, int end, int at, int offset, double ratio, int advance);
};

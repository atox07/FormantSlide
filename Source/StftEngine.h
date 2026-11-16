#pragma once
#include "WaveLoader.h"
#include <algorithm>
#include <array>
#include <memory>
#include <juce_dsp/juce_dsp.h>

class StftVoice {
public:
    StftVoice(std::shared_ptr<const WaveData>, int, int, double, double, double sourceStep);
    int render(float*, float*, int, double);
    int spectralPercent() const { return lastSpectralPercent; }
    bool retryingSpectrum() const { return spectralRetry > 0; }
    void jumpPitch() { snap = true; }
    void setPsolaPercent(int percent) { psolaLimit = std::clamp(percent,0,100)*0.01; }
    void setFormantScale(double v) { formant = std::clamp(v,1./64,64.); }
    void setFormantGuard(float v) { guard = v; }
    void setHighNoteLimit(float v) { limit = v; }
    void setHighNoteRelaxMax(float v) { relax = v; }
private:
    struct Grain { double source, speed, phase, increment; };
    std::shared_ptr<const WaveData> wav;
    int begin,end;
    double step,startNote,smoothing,position,pitch=0,formant=1,color=1,untilPulse=0,mix=0;
    bool snap=true;
    float guard=0,limit=2000,relax=0.75f;
    std::array<Grain,32> grains{};
    static constexpr int order = 10, size = 1 << order, hop = size / 4;
    using Complex = std::complex<float>;
    juce::dsp::FFT fft{order};
    std::array<float,size> window{};
    std::array<Complex,size> input{}, bins{}, output{};
    std::array<float,size/2+1> envelope{}, magnitude{};
    std::array<int,size/2+1> peaks{};
    struct SpectralChannel {
        std::array<float,size> overlap{};
        std::array<double,size/2+1> phase{}, rotation{};
        std::array<float,size/2+1> peakFrequency{}, stableTone{};
    };
    std::array<SpectralChannel,2> spectral{};
    int spectralCursor = 0;
    bool spectralActive = false;
    int spectralRetry = 0, silentSpectrum = 0, silentGrains = 0;
    double spectralReturn = 1;
    void recoverSpectrum();
    int lastSpectralPercent = -1;
    double psolaLimit = 1;
    double effectMix = 0;
    double spectralPower = 0, grainPower = 0, crossPower = 0, blendGain = 1;
    void spectralFrame(int offset, double ratio, double formantScale, bool first);
    int used=0;
    float read(double where,int channel) const;
};

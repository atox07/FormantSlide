#pragma once
#include "WaveLoader.h"
#include <world/synthesisrealtime.h>
#include <memory>
#include <vector>

// Immutable analysis shared by voices. All analysis is done during Load WAV.
struct WorldAnalysis {
    static constexpr double framePeriod = 5.0;
    int sampleRate = 44100, fftSize = 0;
    std::vector<double> f0;
    std::vector<std::vector<double>> spectrum, aperiodicity;
    static std::shared_ptr<WorldAnalysis> analyse(const WaveData& wav);
};

// WORLD is a speech vocoder: stereo input is analysed as a mono downmix.
// Rendering resamples the synthesised mono signal to the host sample rate.
class WorldVoice {
public:
    WorldVoice(std::shared_ptr<const WorldAnalysis> analysis, int begin, int end, double hostRate);
    ~WorldVoice();
    WorldVoice(const WorldVoice&) = delete;
    WorldVoice& operator=(const WorldVoice&) = delete;
    int render(float* output, int count, double pitchScale);
private:
    double nextSample(double pitchScale);
    bool fill(double pitchScale);
    std::shared_ptr<const WorldAnalysis> analysis_;
    WorldSynthesizer synth_{};
    bool initialized_ = false;
    int firstFrame_ = 0, queued_ = 0, cursor_ = 64;
    std::vector<double> pitch_;
    std::vector<double*> spectrum_, aperiodicity_;
    double ratio_ = 1, phase_ = 0, left_ = 0, right_ = 0;
    bool primed_ = false;
    long long remaining_ = 0;
};

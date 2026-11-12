#pragma once

#include "WaveLoader.h"
#include <algorithm>
#include <array>
#include <complex>
#include <memory>
#include <vector>
#include <StftPitchShift/RFFT.h>
#include <StftPitchShift/Cepster.h>

// Multi-resolution STFT for a preloaded WAV. Both public entry points and
// the formant-control method retain their original signatures.
class StftVoice {
public:
    StftVoice(std::shared_ptr<const WaveData>, int begin, int end, double hostRate);
    int render(float* left, float* right, int count, double scale);
    void setFormantScale(double scale);

private:
    static constexpr double kQuefrency          = 0.002;
    static constexpr double kSmoothMs           = 6.0;
    static constexpr double kFadeMs             = 8.0;
    static constexpr float  kPeakFloor          = 1e-4f;
    static constexpr float  kEnvSmoothCoef      = 0.30f;
    static constexpr float  kEnvSmoothCoefSlide = 0.05f;
    static constexpr double kCrossLowHz         = 2200.0;
    static constexpr double kCrossHighHz        = 2800.0;

    struct Track {
        float freq = 0.f;           // Unshifted, fractional-bin frequency
        float phase = 0.f;          // Synthesis phase at the window centre
        float analysisPhase = 0.f;  // Current analysis phase at that centre
        float magnitude = 0.f;
        int age = 0;                // Number of consecutive matched frames
    };

    struct Channel {
        std::vector<float> overlap;
        std::vector<float> prevPhase;
        std::vector<float> prevMagnitude;
        std::vector<float> envSmooth;
        std::vector<float> tonalHistory;
        std::vector<unsigned char> trackMatched;
        std::vector<Track> tracks;
        int trackCount = 0;
        double prevAlpha = 1.0;
        float prevFluxEnergy = 0.f;
        float fluxBaseline = 0.f;
        float voicedConfidence = 0.f; // Low-band voicing shared with high-band
        int transientHold = 0;
        bool havePrev = false;
    };

    struct Band {
        Band(int fftSize, double sampleRate, bool upperBand);

        const int N;
        const int H;
        const int K;
        const double rate;
        const bool high;
        std::shared_ptr<stftpitchshift::FFT> fftp;
        stftpitchshift::Cepster<float> cepster;
        std::array<Channel, 2> channels;

        // Fixed-size scratch: no resizing, reserving or heap use in the DSP.
        std::vector<float> window, crossover, time;
        std::vector<std::complex<float>> bins, spec;
        std::vector<float> mag, ph, env, freqBin;
        std::vector<float> tonalMask, overlapWeights;
        std::vector<int> peakBin, regionLo, regionHi, peakTrackIndex;
        std::vector<Track> newTracks;
        float outNorm = 2.f / 3.f;
        double hopPitchSum = 0.0;
        int cursor = 0;
    };

    float read(double sourcePosition, int channel) const;
    void frame(Band& band, long long start, double alpha, double phaseAlpha);
    void shift(Band& band, Channel& channel, int channelIndex,
               double alpha, double phaseAlpha);
    void advanceBand(Band& band, double alpha);
    void resetState();

    std::shared_ptr<const WaveData> wav;
    int begin, end;
    double step;
    double smoothCoef = 0.0;
    float fadeStep = 0.f;
    long long position = 0, length = 0;
    bool active = false;
    bool mono = false;
    double smoothLog = 0.0;
    bool smoothInit = false;
    float wet = 0.f;
    double formantScaleTarget_ = 1.0;
    double formantScaleSmoothed_ = 1.0;
    double formantSmoothCoef_ = 0.0;

    Band lowBand;
    Band highBand;
};

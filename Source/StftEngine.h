#pragma once
#include "WaveLoader.h"
#include <algorithm>
#include <array>
#include <complex>
#include <memory>
#include <vector>
#include <StftPitchShift/RFFT.h>
#include <StftPitchShift/Cepster.h>

// Pitch shifter with formant preservation used by one sample voice.
class StftVoice {
public:
    StftVoice(std::shared_ptr<const WaveData>, int begin, int end, double hostRate);
    int render(float* left, float* right, int count, double scale);

private:
    // ---- tuning (safe to experiment with) --------------------------------
    static constexpr double kQuefrency          = 0.002;   // formant envelope detail
    static constexpr double kSmoothMs           = 6.0;     // pitch factor smoothing time
    static constexpr double kFadeMs             = 8.0;     // dry/wet crossfade time
    static constexpr float  kMaxGain            = 6.0f;    // max formant correction per bin
    static constexpr float  kPeakFloor          = 1e-4f;   // peaks below this * frame max are ignored
    static constexpr float  kEnvSmoothCoef      = 0.3f;    // envelope smoothing when pitch is stable
    static constexpr float  kEnvSmoothCoefSlide = 0.05f;   // [FIX EH→I] very slow during slides

    struct Track {            // one spectral peak of the previous frame
        float freq;
        float phase;
    };

    struct Channel {
        std::vector<float> overlap;
        std::vector<float> prevPhase;
        std::vector<float> envSmooth;
        std::vector<Track> tracks;
        double prevAlpha = 1.0;   // [FIX EH→I] last pitch factor seen, to detect slides
        bool havePrev = false;
    };

    float read(double position, int channel) const;
    void frame(long long start, double scale);
    void shift(Channel& ch, double scale);
    void resetState();

    std::shared_ptr<const WaveData> wav;
    int begin, end;
    int N, H, K;
    double step;
    double smoothCoef = 0.0;
    float fadeStep = 0.f;
    long long position = 0, length;
    bool active = false;
    int cursor = 0;
    bool mono = false;

    double smoothLog = 0.0;
    bool smoothInit = false;
    float wet = 0.f;

    std::shared_ptr<stftpitchshift::FFT> fftp;
    stftpitchshift::Cepster<float> cepster;
    std::array<Channel, 2> chan;

    std::vector<float> window, time;
    std::vector<std::complex<float>> bins, spec;
    std::vector<float> mag, ph, env, freqBin;
    std::vector<int> peakBin, regionLo, regionHi;
    std::vector<float> synMag;
    std::vector<Track> newTracks;
    float outNorm = 1.f;
};

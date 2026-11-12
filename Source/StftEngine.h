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
//
// Public interface is unchanged (constructor + render), so the slide / MIDI /
// voice logic in FormantSlideNative does not need to be touched.
//
// Differences from the previous engine (stftpitchshift core):
//  * identity phase locking: every spectral peak is moved together with its
//    whole region of influence and keeps ONE continuous phase track.
//    The old core moved every bin independently, which is what produced the
//    hiss and the "ghost harmonic" while the pitch was gliding.
//  * larger window, hop = N/4 (was N=1024, H=64).
//  * pitch factor is smoothed per sample (log domain) between blocks.
//  * dry <-> wet crossfade instead of a hard switch at scale == 1.
//  * cubic interpolation when the WAV rate differs from the host rate.
//  * mono WAVs are processed once instead of twice.
class StftVoice {
public:
    StftVoice(std::shared_ptr<const WaveData>, int begin, int end, double hostRate);
    int render(float* left, float* right, int count, double scale);

private:
    // ---- tuning (safe to experiment with) --------------------------------
    static constexpr double kQuefrency   = 0.001;  // s, formant envelope smoothness (as before)
    static constexpr double kSmoothMs    = 6.0;    // pitch factor smoothing time
    static constexpr double kFadeMs      = 8.0;    // dry/wet crossfade time
    static constexpr float  kMaxGain     = 3.0f;   // max formant correction per bin (~ +9.5 dB)
    static constexpr float  kPeakFloor   = 1e-4f;  // peaks below this * frame max are ignored

    struct Track {            // one spectral peak of the previous frame
        float freq;           // analysis frequency, in bins
        float phase;          // synthesis phase (wet) of this sinusoid
    };

    struct Channel {
        std::vector<float> overlap;                 // output accumulator (N)
        std::vector<float> prevPhase;               // analysis phase of previous frame
        std::vector<Track> tracks;                  // peaks of previous frame
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
    double smoothCoef = 0.0;     // one-pole coefficient for the pitch factor
    float fadeStep = 0.f;        // dry/wet crossfade increment per sample
    long long position = 0, length;
    bool active = false;
    int cursor = 0;
    bool mono = false;

    // smoothing / crossfade state
    double smoothLog = 0.0;      // log2 of the smoothed factor
    bool smoothInit = false;
    float wet = 0.f;

    std::shared_ptr<stftpitchshift::FFT> fftp;
    stftpitchshift::Cepster<float> cepster;
    std::array<Channel, 2> chan;

    std::vector<float> window, time;
    std::vector<std::complex<float>> bins, spec;     // analysis / synthesis spectrum
    std::vector<float> mag, ph, env, freqBin;
    std::vector<int> peakBin, regionLo, regionHi;
    std::vector<float> synMag;
    std::vector<Track> newTracks;
    float outNorm = 1.f;
};

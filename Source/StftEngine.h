#pragma once

#include "WaveLoader.h"
#include "StftDevParams.h"
#include <algorithm>
#include <array>
#include <complex>
#include <memory>
#include <vector>
#include <StftPitchShift/RFFT.h>
#include <StftPitchShift/Cepster.h>

// Two independent, frequency-complementary STFTs.  Public API is unchanged.
// This class renders a preloaded WAV: future samples can be inspected when a
// MIDI note starts, so the synthesis is aligned to the supplied sample position.
class StftVoice {
public:
    StftVoice(std::shared_ptr<const WaveData>, int begin, int end, double hostRate);
    int render(float* left, float* right, int count, double scale);
    void setDevParams(float envSmooth, float envSmoothSlide, float maxGain, double quefrency);
    void setFormantScale(double scale); // 1.0 = preserve; 2.0 = +12 semitones

private:
    double kQuefrency          = 0.002;
    static constexpr double kSmoothMs           = 6.0;
    static constexpr double kFadeMs             = 8.0;
    float kMaxGain            = 6.0f;
    static constexpr float  kPeakFloor          = 1e-4f;
    float kEnvSmoothCoef      = 0.3f;
    float kEnvSmoothCoefSlide = 0.05f;

    struct Track {
        float freq = 0.f;  // Analysis frequency in (possibly fractional) FFT bins
        float phase = 0.f; // Synthesis phase referenced to the frame centre
    };

    struct Channel {
        std::vector<float> overlap;       // Preallocated overlap-add accumulator
        std::vector<float> prevPhase;     // Analysis phase from previous frame
        std::vector<float> prevMagnitude; // Magnitude from previous frame (flux)
        std::vector<float> envSmooth;     // Log-spectral envelope smoother
        std::vector<Track> tracks;        // Fixed-size peak track array
        int trackCount = 0;
        double prevAlpha = 1.0;
        float prevFluxEnergy = 0.f;
        float fluxBaseline = 0.f;
        int transientHold = 0;
        bool havePrev = false;
    };

    struct Band {
        Band(int fftSize, double sampleRate, bool upperBand, double quefrency,
             double crossLowHz, double crossHighHz);

        const int N;
        const int H;
        const int K;
        const double rate;
        const bool high;

        // Every band has its own FFT / cepstral processing state, so sizes and
        // any library-local FFT plans never interfere with the other band.
        std::shared_ptr<stftpitchshift::FFT> fftp;
        stftpitchshift::Cepster<float> cepster;
        std::array<Channel, 2> channels;

        std::vector<float> window;
        std::vector<float> crossover;
        std::vector<float> time;
        std::vector<std::complex<float>> bins;
        std::vector<std::complex<float>> spec;
        std::vector<float> mag;
        std::vector<float> ph;
        std::vector<float> env;
        std::vector<float> freqBin;
        std::vector<int> peakBin;
        std::vector<int> regionLo;
        std::vector<int> regionHi;
        std::vector<Track> newTracks;
        float outNorm = 2.f / 3.f;
        double hopPitchSum = 0.0; // Average MIDI pitch ratio over last H samples
        int cursor = 0;
    };

    float read(double sourcePosition, int channel) const;
    void frame(Band& band, long long start, double alpha, double phaseAlpha);
    void shift(Band& band, Channel& channel, double alpha, double phaseAlpha);
    void advanceBand(Band& band, double alpha);
    void resetState();
    void syncDevParams();                 // legge StftDevParams (slider VST)
    void applyQuefrency(double seconds);  // senza limiti utente, protegge solo la FFT
    unsigned devVersion_ = 0xFFFFFFFFu;

    std::shared_ptr<const WaveData> wav;
    int begin;
    int end;
    double step;
    double smoothCoef = 0.0;
    float fadeStep = 0.f;
    long long position = 0;
    long long length = 0;
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

#include "WorldEngine.h"
#include <world/harvest.h>
#include <world/cheaptrick.h>
#include <world/d4c.h>
#include <algorithm>
#include <cmath>
#include <stdexcept>

std::shared_ptr<WorldAnalysis> WorldAnalysis::analyse(const WaveData& wav) {
    if (wav.frames() < 2) throw std::runtime_error("Empty WAV");
    auto result = std::make_shared<WorldAnalysis>();
    result->sampleRate = wav.sampleRate;
    // Pad very short files for stable speech analysis, without extending playback.
    const int length = std::max(wav.frames(), wav.sampleRate / 10);
    std::vector<double> input(size_t(length), 0.0);
    for (int i = 0; i < wav.frames(); ++i)
        input[size_t(i)] = (double(wav.stereo[size_t(i) * 2]) + wav.stereo[size_t(i) * 2 + 1]) * 0.5;
    HarvestOption harvest{};
    InitializeHarvestOption(&harvest);
    harvest.frame_period = framePeriod;
    const int frames = GetSamplesForHarvest(wav.sampleRate, length, framePeriod);
    CheapTrickOption cheap{};
    InitializeCheapTrickOption(wav.sampleRate, &cheap);
    result->fftSize = GetFFTSizeForCheapTrick(wav.sampleRate, &cheap);
    const int bins = result->fftSize / 2 + 1;
    // Reject excessive analysis memory before allocating both spectral matrices.
    if (double(frames) * bins * sizeof(double) * 2 > 1024.0 * 1024 * 1024)
        throw std::runtime_error("WAV too large for WORLD analysis (1 GiB limit)");
    result->f0.resize(size_t(frames));
    std::vector<double> time(size_t(frames), 0.0);
    Harvest(input.data(), length, wav.sampleRate, &harvest, time.data(), result->f0.data());
    result->spectrum.assign(size_t(frames), std::vector<double>(size_t(bins)));
    result->aperiodicity.assign(size_t(frames), std::vector<double>(size_t(bins)));
    std::vector<double*> spectrum(size_t(frames), nullptr), aperiodicity(size_t(frames), nullptr);
    for (int i = 0; i < frames; ++i) {
        spectrum[size_t(i)] = result->spectrum[size_t(i)].data();
        aperiodicity[size_t(i)] = result->aperiodicity[size_t(i)].data();
    }
    CheapTrick(input.data(), length, wav.sampleRate, time.data(), result->f0.data(),
        frames, &cheap, spectrum.data());
    D4COption d4c{};
    InitializeD4COption(&d4c);
    D4C(input.data(), length, wav.sampleRate, time.data(), result->f0.data(),
        frames, result->fftSize, &d4c, aperiodicity.data());
    return result;
}

WorldVoice::WorldVoice(std::shared_ptr<const WorldAnalysis> analysis, int begin, int end, double hostRate)
    : analysis_(std::move(analysis)) {
    if (!analysis_ || end <= begin || hostRate <= 0) throw std::runtime_error("Invalid WORLD voice");
    ratio_ = analysis_->sampleRate / hostRate;
    remaining_ = static_cast<long long>(std::ceil((end - begin) / ratio_));
    const double frameSamples = analysis_->sampleRate * WorldAnalysis::framePeriod / 1000.0;
    firstFrame_ = int(std::floor(begin / frameSamples));
    const int frames = int(std::ceil((end - begin) / frameSamples)) + 32;
    pitch_.resize(size_t(frames));
    spectrum_.resize(size_t(frames));
    aperiodicity_.resize(size_t(frames));
    for (int i = 0; i < frames; ++i) {
        const int source = std::clamp(firstFrame_ + i, 0, int(analysis_->f0.size()) - 1);
        // WORLD does not modify these input matrices; keep their owner alive.
        spectrum_[size_t(i)] = const_cast<double*>(analysis_->spectrum[size_t(source)].data());
        aperiodicity_[size_t(i)] = const_cast<double*>(analysis_->aperiodicity[size_t(source)].data());
    }
    InitializeSynthesizer(analysis_->sampleRate, WorldAnalysis::framePeriod,
        analysis_->fftSize, 64, 64, &synth_);
    initialized_ = true;
    // Align slice start to the WAV sample, not just the 5 ms analysis grid.
    phase_ = begin - firstFrame_ * frameSamples;
}
WorldVoice::~WorldVoice() {
    if (initialized_) DestroySynthesizer(&synth_);
}
bool WorldVoice::fill(double pitchScale) {
    // Feed only enough lookahead to synthesize a block. Retain all parameter
    // storage for the voice lifetime because WORLD keeps pointers into it.
    for (int attempts = 0; attempts < 64; ++attempts) {
        if (Synthesis2(&synth_)) { cursor_ = 0; return true; }
        if (queued_ >= int(pitch_.size()) || IsLocked(&synth_)) return false;
        const int source = std::clamp(firstFrame_ + queued_, 0, int(analysis_->f0.size()) - 1);
        const double f0 = analysis_->f0[size_t(source)];
        pitch_[size_t(queued_)] = f0 > 0 ? std::clamp(f0 * pitchScale, 40.0, analysis_->sampleRate / 4.0) : 0;
        if (!AddParameters(&pitch_[size_t(queued_)], 1, &spectrum_[size_t(queued_)],
            &aperiodicity_[size_t(queued_)], &synth_)) return false;
        ++queued_;
    }
    return false;
}
double WorldVoice::nextSample(double pitchScale) {
    if (cursor_ >= 64 && !fill(pitchScale)) return 0;
    return synth_.buffer[cursor_++];
}
int WorldVoice::render(float* output, int count, double pitchScale) {
    if (!output || count <= 0 || !std::isfinite(pitchScale) || pitchScale <= 0) return 0;
    const int frames = int(std::min<long long>(count, remaining_));
    if (!primed_) {
        left_ = nextSample(pitchScale);
        right_ = nextSample(pitchScale);
        primed_ = true;
    }
    for (int i = 0; i < frames; ++i) {
        while (phase_ >= 1) {
            left_ = right_;
            right_ = nextSample(pitchScale);
            phase_ -= 1;
        }
        output[i] = float(left_ + (right_ - left_) * phase_);
        phase_ += ratio_;
    }
    remaining_ -= frames;
    return frames;
}

#include "StftEngine.h"
#include <cmath>
#include <utility>

namespace {
constexpr double kPi = 3.141592653589793238462643383279502884;
constexpr double kTwoPi = 2.0 * kPi;

int pickSize(double rate, int base) {
    if (rate <= 50000.0) return base;
    if (rate <= 100000.0) return base * 2;
    return base * 4;
}

double wrapPhase(double value) {
    return value - kTwoPi * std::floor(value / kTwoPi + 0.5);
}

float saturate(float value) { return std::clamp(value, 0.f, 1.f); }

// Four-tap Lagrange interpolation for fractional-bin relocation. Unlike a
// simple two-bin blend it preserves more of the sinusoid's spectral shape.
void fractionalWeights(float t, float (&w)[4]) {
    w[0] = -t * (t - 1.f) * (t - 2.f) / 6.f;
    w[1] = (t + 1.f) * (t - 1.f) * (t - 2.f) / 2.f;
    w[2] = -(t + 1.f) * t * (t - 2.f) / 2.f;
    w[3] = (t + 1.f) * t * (t - 1.f) / 6.f;
}
} // namespace

StftVoice::Band::Band(int fftSize, double sampleRate, bool upperBand)
    : N(fftSize), H(fftSize / 4), K(fftSize / 2 + 1),
      rate(sampleRate), high(upperBand),
      fftp(std::make_shared<stftpitchshift::RFFT>()),
      cepster(fftp, size_t(fftSize), sampleRate),
      window(size_t(N), 0.f), crossover(size_t(K), 0.f),
      time(size_t(N), 0.f), bins(size_t(K)), spec(size_t(K)),
      mag(size_t(K), 0.f), ph(size_t(K), 0.f),
      env(size_t(K), 0.f), freqBin(size_t(K), 0.f),
      tonalMask(size_t(K), 0.f), overlapWeights(size_t(K), 0.f),
      peakBin(size_t(K), 0), regionLo(size_t(K), 0),
      regionHi(size_t(K), 0), peakTrackIndex(size_t(K), -1),
      newTracks(size_t(K)) {
    cepster.quefrency(kQuefrency);
    for (int i = 0; i < N; ++i)
        window[size_t(i)] = float(0.5 - 0.5 * std::cos(kTwoPi * i / N));

    for (int k = 0; k < K; ++k) {
        const double hz = double(k) * rate / N;
        const float upperWeight = float(std::clamp(
            (hz - kCrossLowHz) / (kCrossHighHz - kCrossLowHz), 0.0, 1.0));
        crossover[size_t(k)] = high ? upperWeight : 1.f - upperWeight;
    }
    // 4x overlap / periodic Hann analysis and synthesis window.
    outNorm = 2.f / 3.f;
    for (auto& c : channels) {
        c.overlap.assign(size_t(N), 0.f);
        c.prevPhase.assign(size_t(K), 0.f);
        c.prevMagnitude.assign(size_t(K), 0.f);
        c.envSmooth.assign(size_t(K), 0.f);
        c.tonalHistory.assign(size_t(K), 0.f);
        c.trackMatched.assign(size_t(K), 0);
        c.tracks.resize(size_t(K));
    }

    // The libraries can lazily create plans/caches. Prime FFT and cepstral
    // paths at construction so the first MIDI trigger does not create a plan.
    fftp->fft(time, bins);
    fftp->ifft(bins, time);
    std::fill(env.begin(), env.end(), 1.f);
    cepster.lifter(env);
    std::fill(env.begin(), env.end(), 0.f);
}

StftVoice::StftVoice(std::shared_ptr<const WaveData> data,
                     int first, int last, double hostRate)
    : wav(std::move(data)),
      begin(std::clamp(first, 0, wav->frames())),
      end(std::clamp(last, begin, wav->frames())),
      step(wav->sampleRate / hostRate),
      length(static_cast<long long>(std::ceil((end - begin) / step))),
      lowBand(pickSize(hostRate, 2048), hostRate, false),
      highBand(pickSize(hostRate, 512), hostRate, true) {
    smoothCoef = 1.0 - std::exp(-1.0 / (kSmoothMs * 0.001 * hostRate));
    formantSmoothCoef_ = smoothCoef;
    fadeStep = float(1.0 / (kFadeMs * 0.001 * hostRate));
    mono = true;
    const auto& samples = wav->stereo;
    for (int i = begin; i < end; ++i) {
        if (samples[size_t(i) * 2] != samples[size_t(i) * 2 + 1]) {
            mono = false;
            break;
        }
    }
}

void StftVoice::setFormantScale(double scale) {
    formantScaleTarget_ = std::isfinite(scale)
        ? std::clamp(scale, 1.0 / 64.0, 64.0) : 1.0;
}

void StftVoice::resetState() {
    auto resetBand = [](Band& b) {
        b.cursor = 0;
        b.hopPitchSum = 0.0;
        for (auto& c : b.channels) {
            std::fill(c.overlap.begin(), c.overlap.end(), 0.f);
            std::fill(c.prevPhase.begin(), c.prevPhase.end(), 0.f);
            std::fill(c.prevMagnitude.begin(), c.prevMagnitude.end(), 0.f);
            std::fill(c.envSmooth.begin(), c.envSmooth.end(), 0.f);
            std::fill(c.tonalHistory.begin(), c.tonalHistory.end(), 0.f);
            c.trackCount = 0;
            c.prevAlpha = 1.0;
            c.prevFluxEnergy = 0.f;
            c.fluxBaseline = 0.f;
            c.transientHold = 0;
            c.havePrev = false;
        }
    };
    resetBand(lowBand);
    resetBand(highBand);
}

float StftVoice::read(double where, int channel) const {
    if (where < begin || where >= end) return 0.f;
    const float* s = wav->stereo.data();
    const int a = int(where);
    const float t = float(where - a);
    auto at = [&](int idx) {
        return s[size_t(std::clamp(idx, begin, end - 1)) * 2 + size_t(channel)];
    };
    if (t == 0.f) return at(a);
    const float y0 = at(a - 1), y1 = at(a), y2 = at(a + 1), y3 = at(a + 2);
    const float c1 = 0.5f * (y2 - y0);
    const float c2 = y0 - 2.5f * y1 + 2.f * y2 - 0.5f * y3;
    const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
    return ((c3 * t + c2) * t + c1) * t + y1;
}

void StftVoice::shift(Band& b, Channel& c, int channelIndex,
                      double alpha, double phaseAlpha) {
    const bool hadPrev = c.havePrev;
    const double omega = kTwoPi * b.H / b.N;
    float maxMag = 0.f;
    float fluxPositive = 0.f, fluxTotal = 0.f, fluxEnergy = 0.f;
    const int fluxFirst = std::max(1, int(std::floor(
        (b.high ? 1800.0 : 80.0) * b.N / b.rate)));
    const int fluxLast = std::min(b.K - 2, int(std::ceil(
        (b.high ? b.rate * 0.48 : 3200.0) * b.N / b.rate)));

    for (int k = 0; k < b.K; ++k) {
        const size_t ix = size_t(k);
        const float m = std::abs(b.bins[ix]);
        const float p = std::arg(b.bins[ix]);
        b.mag[ix] = m;
        b.ph[ix] = p;
        maxMag = std::max(maxMag, m);
        if (k >= fluxFirst && k <= fluxLast) {
            fluxEnergy += m;
            if (hadPrev)
                fluxPositive += std::max(0.f, m - c.prevMagnitude[ix]);
            fluxTotal += m;
        }
        c.prevMagnitude[ix] = m;
        double f = k;
        if (hadPrev) {
            const double error = wrapPhase(double(p) -
                double(c.prevPhase[ix]) - k * omega);
            f += error / omega;
        }
        b.freqBin[ix] = float(f);
        c.prevPhase[ix] = p;
    }
    const float flux = fluxPositive / std::max(fluxTotal, 1e-9f);
    const float threshold = b.high ? 0.17f : 0.22f;
    const bool onset = hadPrev && c.transientHold == 0 &&
        flux > std::max(threshold, c.fluxBaseline * 1.75f) &&
        (fluxEnergy > 1.08f * c.prevFluxEnergy || flux > 0.42f);
    c.fluxBaseline = hadPrev
        ? 0.94f * c.fluxBaseline + 0.06f * flux : 0.25f * flux;
    c.prevFluxEnergy = fluxEnergy;
    if (c.transientHold > 0) --c.transientHold;
    if (onset) c.transientHold = b.high ? 3 : 2;

    std::fill(b.spec.begin(), b.spec.end(), std::complex<float>{});
    if (maxMag < 1e-9f) {
        c.trackCount = 0;
        c.havePrev = false;
        c.fluxBaseline = 0.f;
        c.prevAlpha = alpha;
        std::fill(c.tonalHistory.begin(), c.tonalHistory.end(), 0.f);
        return;
    }

    // A sibilant is spectrally flat rather than a stable collection of
    // partials.  During noisy consonants suppress spurious peak tracking,
    // especially when an upward slide would whistle in the top octaves.
    float noiseDominance = 0.f;
    if (b.high) {
        const int lo = std::max(1, int(2500.0 * b.N / b.rate));
        const int hi = std::min(b.K - 2, int(12000.0 * b.N / b.rate));
        if (hi > lo) {
            double linear = 0.0, logarithmic = 0.0;
            for (int k = lo; k <= hi; ++k) {
                const double value = std::max(1e-8, double(b.mag[size_t(k)]));
                linear += value;
                logarithmic += std::log(value);
            }
            const double count = hi - lo + 1;
            const double flatness = std::exp(logarithmic / count) /
                std::max(1e-9, linear / count);
            noiseDominance = saturate(float((flatness - 0.48) / 0.30));
        }
    }

    const float peakThreshold = maxMag * kPeakFloor;
    int peakCount = 0;
    for (int k = 2; k < b.K - 2; ++k) {
        const float m = b.mag[size_t(k)];
        if (m > peakThreshold &&
            m > b.mag[size_t(k - 1)] && m >= b.mag[size_t(k + 1)] &&
            m > b.mag[size_t(k - 2)] && m >= b.mag[size_t(k + 2)]) {
            if (peakCount && k - b.peakBin[size_t(peakCount - 1)] < 4) {
                const int prev = b.peakBin[size_t(peakCount - 1)];
                if (m > b.mag[size_t(prev)])
                    b.peakBin[size_t(peakCount - 1)] = k;
            } else {
                b.peakBin[size_t(peakCount++)] = k;
            }
        }
    }
    if (!peakCount) {
        int best = 1;
        for (int k = 2; k < b.K - 1; ++k)
            if (b.mag[size_t(k)] > b.mag[size_t(best)]) best = k;
        b.peakBin[0] = best;
        peakCount = 1;
    }
    for (int i = 0; i < peakCount; ++i) {
        b.regionLo[size_t(i)] = 1;
        b.regionHi[size_t(i)] = b.K - 2;
    }
    for (int i = 0; i + 1 < peakCount; ++i) {
        int valley = b.peakBin[size_t(i)] + 1;
        for (int k = valley; k < b.peakBin[size_t(i + 1)]; ++k)
            if (b.mag[size_t(k)] < b.mag[size_t(valley)]) valley = k;
        b.regionHi[size_t(i)] = valley;
        b.regionLo[size_t(i + 1)] = valley + 1;
    }

    const double slideSpeed = std::abs(alpha - c.prevAlpha);
    c.prevAlpha = alpha;
    const float envCoef = slideSpeed > 1e-4
        ? kEnvSmoothCoefSlide : kEnvSmoothCoef;
    for (int k = 0; k < b.K; ++k)
        b.env[size_t(k)] = std::max(b.mag[size_t(k)], 1e-9f);
    b.cepster.lifter(b.env);
    for (int k = 0; k < b.K; ++k) {
        const size_t ix = size_t(k);
        const float logNow = std::log(std::max(b.env[ix], 1e-9f));
        c.envSmooth[ix] = hadPrev
            ? envCoef * logNow + (1.f - envCoef) * c.envSmooth[ix]
            : logNow;
        b.env[ix] = std::exp(c.envSmooth[ix]);
    }
    const float beta = float(std::clamp(
        formantScaleSmoothed_, 1.0 / 64.0, 64.0));
    auto sampleEnv = [&](float index) {
        index = std::clamp(index, 1.f, float(b.K - 2));
        const int lo = int(std::floor(index));
        const int hi = std::min(lo + 1, b.K - 2);
        const float f = index - float(lo);
        return b.env[size_t(lo)] * (1.f - f) + b.env[size_t(hi)] * f;
    };

    // Greedy one-to-one sinusoidal association. Avoids two new peaks inheriting
    // the same previous track (a frequent cause of a "second note" on slides).
    std::fill(c.trackMatched.begin(), c.trackMatched.end(), 0);
    std::fill(b.tonalMask.begin(), b.tonalMask.end(), 0.f);
    int nextTrackCount = 0;
    for (int i = 0; i < peakCount; ++i) {
        b.peakTrackIndex[size_t(i)] = -1;
        const int kp = b.peakBin[size_t(i)];
        const float m = b.mag[size_t(kp)];
        const double fp = std::clamp(double(b.freqBin[size_t(kp)]),
                                      kp - 1.5, kp + 1.5);
        if (alpha * fp <= 0.0) continue;
        const double centreAna = wrapPhase(
            double(b.ph[size_t(kp)]) + kPi * kp);

        int match = -1;
        float matchScore = 1e10f;
        for (int j = 0; j < c.trackCount; ++j) {
            if (c.trackMatched[size_t(j)]) continue;
            const Track& p = c.tracks[size_t(j)];
            const float distance = float(std::abs(fp - p.freq));
            if (distance > (b.high ? 1.20f : 1.45f)) continue;
            const float magPenalty = 0.15f * std::abs(std::log(
                (m + 1e-7f) / (p.magnitude + 1e-7f)));
            const float score = distance + magPenalty -
                0.02f * float(std::min(p.age, 8));
            if (score < matchScore) { matchScore = score; match = j; }
        }
        Track track;
        track.freq = float(fp);
        track.analysisPhase = float(centreAna);
        track.magnitude = m;
        track.age = match >= 0 ? std::min(c.tracks[size_t(match)].age + 1, 255) : 1;
        double centreSyn = centreAna;
        if (match >= 0) {
            c.trackMatched[size_t(match)] = 1;
            if (!onset) {
                const Track& old = c.tracks[size_t(match)];
                const double avgFreq = 0.5 * (double(old.freq) + fp);
                centreSyn = wrapPhase(double(old.phase) +
                                      avgFreq * phaseAlpha * omega);
            }
        }

        // Stereo-link only a confidently shared peak. Apply the same phase
        // correction as left, retaining the original L-R analysis phase gap.
        // Do not force this on independent/panned right-only components.
        if (channelIndex == 1 && !onset && track.age >= 2) {
            const Channel& reference = b.channels[0];
            int common = -1;
            float nearest = b.high ? 0.75f : 0.90f;
            for (int j = 0; j < reference.trackCount; ++j) {
                const Track& r = reference.tracks[size_t(j)];
                const float distance = float(std::abs(fp - r.freq));
                const float ratio = m / std::max(r.magnitude, 1e-7f);
                if (distance < nearest && ratio > 0.35f && ratio < 2.8f &&
                    r.age >= 2) {
                    nearest = distance;
                    common = j;
                }
            }
            if (common >= 0) {
                const Track& r = reference.tracks[size_t(common)];
                const double correction = wrapPhase(
                    double(r.phase) - double(r.analysisPhase));
                const double desired = wrapPhase(centreAna + correction);
                const double mix = b.high ? 0.45 : 0.70;
                centreSyn = wrapPhase(centreSyn +
                            mix * wrapPhase(desired - centreSyn));
            }
        }
        track.phase = float(wrapPhase(centreSyn));
        b.peakTrackIndex[size_t(i)] = nextTrackCount;
        b.newTracks[size_t(nextTrackCount++)] = track;

        if (b.high) {
            // Tonal/noise separation: high-frequency *stable, prominent*
            // partials shift; hiss, breath, S/SH/F and random peaks stay at
            // their original frequencies. No artificial high-pitch noise note.
            float neighbours = 0.f;
            int neighboursCount = 0;
            for (int offset = -6; offset <= 6; ++offset) {
                if (std::abs(offset) < 3) continue;
                const int q = kp + offset;
                if (q >= 1 && q < b.K - 1) {
                    neighbours += b.mag[size_t(q)];
                    ++neighboursCount;
                }
            }
            const float surroundings = neighbours /
                float(std::max(1, neighboursCount));
            const float prominence = m / std::max(surroundings, 1e-8f);
            const float sharpness = saturate((prominence - 1.35f) / 1.45f);
            const float maturity = saturate(float(track.age - 1) / 3.f);
            float confidence = sharpness * (0.40f + 0.60f * maturity);
            confidence *= (1.f - 0.80f * noiseDominance);
            if (onset) confidence *= 0.55f;
            for (int k = std::max(1, kp - 4);
                 k <= std::min(b.K - 2, kp + 4); ++k) {
                const int distance = std::abs(k - kp);
                const float lobe = distance <= 1 ? 1.f :
                                   distance == 2 ? 0.87f :
                                   distance == 3 ? 0.40f : 0.12f;
                b.tonalMask[size_t(k)] = std::max(
                    b.tonalMask[size_t(k)], confidence * lobe);
            }
        }
    }

    for (int i = 0; i < nextTrackCount; ++i)
        c.tracks[size_t(i)] = b.newTracks[size_t(i)];
    c.trackCount = nextTrackCount;

    // Smooth the separation along time in the high-band. The low band retains
    // full pitched energy: its low/mid vocal harmonics must not be doubled.
    if (b.high) {
        for (int k = 1; k < b.K - 1; ++k) {
            const size_t ix = size_t(k);
            const float raw = b.tonalMask[ix];
            const float stable = (hadPrev && !onset)
                ? 0.55f * c.tonalHistory[ix] + 0.45f * raw : raw;
            c.tonalHistory[ix] = stable;
            b.tonalMask[ix] = stable;
            b.spec[ix] = b.bins[ix] * (1.f - stable); // unpitched residual
        }
    } else {
        std::fill(b.tonalMask.begin(), b.tonalMask.end(), 1.f);
    }

    // Shift the coherent harmonic region around each track. Fractional-bin
    // interpolation takes place in window-centred complex spectra (the pi
    // index correction below), and phase is integrated on sub-bin frequency.
    std::fill(b.overlapWeights.begin(), b.overlapWeights.end(), 0.f);
    for (int i = 0; i < peakCount; ++i) {
        const int kp = b.peakBin[size_t(i)];
        const int trackIndex = b.peakTrackIndex[size_t(i)];
        if (trackIndex < 0) continue;
        const Track& track = b.newTracks[size_t(trackIndex)];
        const double exactShift = (alpha - 1.0) * track.freq;
        const int deltaFloor = int(std::floor(exactShift));
        const float fraction = float(exactShift - deltaFloor);
        float weights[4];
        fractionalWeights(fraction, weights);
        std::complex<float> rotation[4];
        for (int tap = 0; tap < 4; ++tap) {
            const int delta = deltaFloor + tap - 1;
            const double theta = wrapPhase(double(track.phase) -
                double(b.ph[size_t(kp)]) - kPi * (kp + delta));
            rotation[tap] = {float(std::cos(theta)), float(std::sin(theta))};
        }
        for (int k = b.regionLo[size_t(i)];
             k <= b.regionHi[size_t(i)]; ++k) {
            const float tonal = b.tonalMask[size_t(k)];
            if (tonal <= 1e-5f) continue;
            const float inputEnvelope = std::max(b.env[size_t(k)], 1e-9f);
            const std::complex<float> base = b.bins[size_t(k)] * tonal;
            for (int tap = 0; tap < 4; ++tap) {
                const float weight = weights[tap];
                if (std::abs(weight) < 1e-6f) continue;
                const int dst = k + deltaFloor + tap - 1;
                if (dst < 1 || dst >= b.K - 1) continue;
                const float targetEnvelope = sampleEnv(float(dst) / beta);
                // Old +/-12dB boosts were amplifying noise as metallic hiss,
                // especially for upward slides and the short FFT.
                const float maxGain = b.high ? 2.4f : 4.f;
                const float gain = std::clamp(targetEnvelope /
                    inputEnvelope, b.high ? 0.50f : 0.35f, maxGain);
                float trebleSafety = 1.f;
                if (b.high && alpha > 1.0) {
                    const float destinationHz = float(dst * b.rate / b.N);
                    const float amount = saturate(float((alpha - 1.0) / 2.0));
                    const float brightness = saturate(
                        (destinationHz - 6500.f) / 8500.f);
                    trebleSafety -= 0.55f * amount * brightness;
                }
                b.spec[size_t(dst)] += base * rotation[tap] *
                    (gain * weight * trebleSafety);
                b.overlapWeights[size_t(dst)] += tonal * std::abs(weight);
            }
        }
    }

    // Prevent stacked/split peaks from generating large high-frequency spikes.
    // This does not boost sparse bins and never touches the unpitched noise.
    for (int k = 1; k < b.K - 1; ++k) {
        const size_t ix = size_t(k);
        const float weight = b.overlapWeights[ix];
        if (weight > 1.35f) {
            const float scale = 1.35f / weight;
            if (b.high) {
                const std::complex<float> noise = b.bins[ix] *
                    (1.f - b.tonalMask[ix]);
                b.spec[ix] = noise + (b.spec[ix] - noise) * scale;
            } else {
                b.spec[ix] *= scale;
            }
        }
        b.spec[ix] *= b.crossover[ix];
    }
    b.spec.front() = b.spec.back() = {};
    c.havePrev = true;
}

void StftVoice::frame(Band& b, long long start, double alpha,
                      double phaseAlpha) {
    const int count = mono ? 1 : 2;
    for (int ch = 0; ch < count; ++ch) {
        // Identical source/output sample timebase for both STFT sizes.
        for (int i = 0; i < b.N; ++i)
            b.time[size_t(i)] = read(
                begin + double(start + i) * step, ch) * b.window[size_t(i)];
        b.fftp->fft(b.time, b.bins);
        shift(b, b.channels[size_t(ch)], ch, alpha, phaseAlpha);
        b.fftp->ifft(b.spec, b.time);
        auto& overlap = b.channels[size_t(ch)].overlap;
        for (int i = 0; i < b.N; ++i) {
            const long long offset = start + i - position;
            if (offset >= 0 && offset < b.N)
                overlap[size_t(offset)] += b.time[size_t(i)] *
                    b.window[size_t(i)] * b.outNorm;
        }
    }
}

void StftVoice::advanceBand(Band& b, double alpha) {
    if (b.cursor != b.H) return;
    for (int ch = 0; ch < (mono ? 1 : 2); ++ch) {
        auto& o = b.channels[size_t(ch)].overlap;
        std::move(o.begin() + b.H, o.end(), o.begin());
        std::fill(o.end() - b.H, o.end(), 0.f);
    }
    frame(b, position, alpha, b.hopPitchSum / b.H);
    b.cursor = 0;
    b.hopPitchSum = 0.0;
}

int StftVoice::render(float* left, float* right, int count, double scale) {
    if (count <= 0 || length <= position) return 0;
    const int frames = int(std::max<long long>(0,
        std::min<long long>(count, length - position)));
    const double safeScale = std::isfinite(scale)
        ? std::clamp(scale, 1.0 / 64.0, 64.0) : 1.0;
    const double target = std::log2(safeScale);
    if (!smoothInit) { smoothLog = target; smoothInit = true; }

    for (int i = 0; i < frames; ++i, ++position) {
        smoothLog += (target - smoothLog) * smoothCoef;
        if (target == 0.0 && std::abs(smoothLog) < 1e-4)
            smoothLog = 0.0;
        const bool shifted = std::abs(smoothLog) > 1e-5;
        const double factor = std::exp2(smoothLog);
        formantScaleSmoothed_ +=
            (formantScaleTarget_ - formantScaleSmoothed_) * formantSmoothCoef_;

        if (shifted && !active) {
            resetState();
            for (int offset = -lowBand.N + lowBand.H;
                 offset <= 0; offset += lowBand.H)
                frame(lowBand, position + offset, factor, factor);
            for (int offset = -highBand.N + highBand.H;
                 offset <= 0; offset += highBand.H)
                frame(highBand, position + offset, factor, factor);
            active = true;
            wet = position == 0 ? 1.f : 0.f;
        }
        if (active) {
            advanceBand(lowBand, factor);
            advanceBand(highBand, factor);
        }
        const float wetTarget = shifted ? 1.f : 0.f;
        wet += std::clamp(wetTarget - wet, -fadeStep, fadeStep);
        float dl = 0.f, dr = 0.f, wl = 0.f, wr = 0.f;
        if (wet < 1.f) {
            dl = read(begin + double(position) * step, 0);
            dr = mono ? dl : read(begin + double(position) * step, 1);
        }
        if (active) {
            wl = lowBand.channels[0].overlap[size_t(lowBand.cursor)] +
                 highBand.channels[0].overlap[size_t(highBand.cursor)];
            wr = mono ? wl :
                 lowBand.channels[1].overlap[size_t(lowBand.cursor)] +
                 highBand.channels[1].overlap[size_t(highBand.cursor)];
            ++lowBand.cursor;
            ++highBand.cursor;
            lowBand.hopPitchSum += factor;
            highBand.hopPitchSum += factor;
        }
        left[i] = dl * (1.f - wet) + wl * wet;
        right[i] = dr * (1.f - wet) + wr * wet;
        if (active && !shifted && wet == 0.f) active = false;
    }
    return frames;
}

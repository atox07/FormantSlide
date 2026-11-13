#include "StftEngine.h"
#include <cmath>
#include <utility>

namespace {
constexpr double kPi = 3.141592653589793238462643383279502884;
constexpr double kTwoPi = 2.0 * kPi;

int pickSize(double rate, int base) {
    // L'utente puo' scrivere qualsiasi numero: lo portiamo alla potenza di 2
    // piu' vicina (la FFT la richiede), tra 128 e 16384.
    int n = 128;
    const int want = std::clamp(base, 128, 16384);
    while (n < 16384 && std::abs(std::log2(double(n * 2) / want)) < std::abs(std::log2(double(n) / want)))
        n *= 2;
    if (rate > 100000.0) n *= 4;
    else if (rate > 50000.0) n *= 2;
    return n;
}

double smooth01(double x) {
    x = std::clamp(x, 0.0, 1.0);
    return x * x * (3.0 - 2.0 * x);
}

double wrapPhase(double p) {
    return p - kTwoPi * std::floor(p / kTwoPi + 0.5);
}
} // namespace

StftVoice::Band::Band(int fftSize, double sampleRate, bool upperBand, double quefrency,
                      double crossLowHz, double crossHighHz)
    : N(fftSize),
      H(fftSize / 4),
      K(fftSize / 2 + 1),
      rate(sampleRate),
      high(upperBand),
      fftp(std::make_shared<stftpitchshift::RFFT>()),
      cepster(fftp, size_t(fftSize), sampleRate),
      window(size_t(N), 0.f),
      crossover(size_t(K), 0.f),
      time(size_t(N), 0.f),
      bins(size_t(K)),
      spec(size_t(K)),
      mag(size_t(K), 0.f),
      ph(size_t(K), 0.f),
      env(size_t(K), 0.f),
      freqBin(size_t(K), 0.f),
      peakBin(size_t(K), 0),
      regionLo(size_t(K), 0),
      regionHi(size_t(K), 0),
      newTracks(size_t(K)) {
    // Con FFT piccole il valore iniziale potrebbe superare N/2 campioni.
    cepster.quefrency(std::clamp(quefrency, 1.0 / sampleRate,
                                 double(fftSize / 2 - 1) / sampleRate));
    for (int i = 0; i < N; ++i)
        window[size_t(i)] = float(0.5 - 0.5 * std::cos(kTwoPi * i / N));

    // Complementary frequency-domain crossover.  We filter each *synthesized*
    // spectrum, so an upward/downward shift is split by its OUTPUT frequency.
    for (int k = 0; k < K; ++k) {
        const double hz = double(k) * rate / N;
        const float highWeight = float(std::clamp(
            (hz - crossLowHz) / std::max(crossHighHz - crossLowHz, 1e-3), 0.0, 1.0));
        crossover[size_t(k)] = high ? highWeight : 1.f - highWeight;
    }

    // The periodic Hann window squared has sum 1.5 at 4x overlap.
    outNorm = 2.f / 3.f;
    for (auto& c : channels) {
        c.overlap.assign(size_t(N), 0.f);
        c.prevPhase.assign(size_t(K), 0.f);
        c.prevMagnitude.assign(size_t(K), 0.f);
        c.envSmooth.assign(size_t(K), 0.f);
        c.tracks.resize(size_t(K));
    }

    // RFFT lazily builds a per-transform-size cache on its first call.
    // Prime both directions HERE, not in render()/frame()/shift().  Cepster
    // uses this same FFT instance and the same N in its lifter() call.
    fftp->fft(time, bins);
    fftp->ifft(bins, time);
}

StftVoice::StftVoice(std::shared_ptr<const WaveData> data,
                     int first, int last, double hostRate)
    : wav(std::move(data)),
      begin(std::clamp(first, 0, wav->frames())),
      end(std::clamp(last, begin, wav->frames())),
      step(wav->sampleRate / hostRate),
      length(static_cast<long long>(std::ceil((end - begin) / step))),
      lowBand(pickSize(hostRate, StftDevParams::instance().fftLow.load()), hostRate, false,
              kQuefrency, StftDevParams::instance().crossLowHz.load(),
              StftDevParams::instance().crossHighHz.load()),
      highBand(pickSize(hostRate, StftDevParams::instance().fftHigh.load()), hostRate, true,
               kQuefrency, StftDevParams::instance().crossLowHz.load(),
               StftDevParams::instance().crossHighHz.load()) {
    smoothCoef = 1.0 - std::exp(-1.0 / (kSmoothMs * 0.001 * hostRate));
    formantSmoothCoef_ = smoothCoef;
    fadeStep = float(1.0 / (kFadeMs * 0.001 * hostRate));

    // No scanning or allocations are performed on the audio callback.
    mono = true;
    const auto& s = wav->stereo;
    for (int i = begin; i < end; ++i) {
        if (s[size_t(i) * 2] != s[size_t(i) * 2 + 1]) {
            mono = false;
            break;
        }
    }
}

void StftVoice::applyQuefrency(double q) {
    if (!std::isfinite(q)) return;
    kQuefrency = q;
    // Un cepstro ha solo N/2 campioni: oltre quel punto il risultato e'
    // identico ("nessun liftering"), quindi il valore passato alla libreria
    // viene tenuto nel range valido per evitare accessi fuori buffer.
    auto set = [q](Band& b) {
        const double lo = 1.0 / b.rate;
        const double hi = double(b.N / 2 - 1) / b.rate;
        b.cepster.quefrency(std::clamp(q, lo, hi));
    };
    set(lowBand);
    set(highBand);
}

// Nessun range imposto: qualsiasi valore finito viene applicato.
void StftVoice::setDevParams(float envSmooth, float envSmoothSlide, float maxGain, double quefrency) {
    if (std::isfinite(envSmooth)) kEnvSmoothCoef = envSmooth;
    if (std::isfinite(envSmoothSlide)) kEnvSmoothCoefSlide = envSmoothSlide;
    if (!std::isnan(maxGain)) kMaxGain = std::max(0.f, maxGain);
    if (quefrency != kQuefrency) applyQuefrency(quefrency);
}

void StftVoice::syncDevParams() {
    auto& p = StftDevParams::instance();
    const unsigned v = p.version.load(std::memory_order_acquire);
    if (v == devVersion_) return;
    devVersion_ = v;
    setDevParams(p.envSmooth.load(), p.envSmoothSlide.load(), p.maxGain.load(),
                 p.quefrencyMs.load() * 0.001);
}

void StftVoice::jumpPitch() { snapPending_ = true; }

void StftVoice::setFormantScale(double scale) {
    formantScaleTarget_ = std::isfinite(scale)
        ? std::clamp(scale, 1.0 / 64.0, 64.0) : 1.0;
    if (!formantInit_) { formantScaleSmoothed_ = formantScaleTarget_; formantInit_ = true; }
}

void StftVoice::resetState() {
    auto resetBand = [](Band& band) {
        band.cursor = 0;
        band.hopPitchSum = 0.0;
        for (auto& c : band.channels) {
            std::fill(c.overlap.begin(), c.overlap.end(), 0.f);
            std::fill(c.prevPhase.begin(), c.prevPhase.end(), 0.f);
            std::fill(c.prevMagnitude.begin(), c.prevMagnitude.end(), 0.f);
            std::fill(c.envSmooth.begin(), c.envSmooth.end(), 0.f);
            c.trackCount = 0;
            c.prevAlpha = 1.0;
            c.prevFluxEnergy = 0.f;
            c.fluxBaseline = 0.f;
            c.transientHold = 0;
            c.f0Hz = 0.f;
            c.relax = 0.f;
            c.havePrev = false;
        }
    };
    resetBand(lowBand);
    resetBand(highBand);
}

float StftVoice::read(double where, int ch) const {
    if (where < begin || where >= end) return 0.f;
    const float* s = wav->stereo.data();
    const int a = int(where);
    const float f = float(where - a);
    auto at = [&](int i) {
        return s[size_t(std::clamp(i, begin, end - 1)) * 2 + size_t(ch)];
    };
    if (f == 0.f) return at(a);
    const float y0 = at(a - 1), y1 = at(a), y2 = at(a + 1), y3 = at(a + 2);
    const float c1 = 0.5f * (y2 - y0);
    const float c2 = y0 - 2.5f * y1 + 2.f * y2 - 0.5f * y3;
    const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
    return ((c3 * f + c2) * f + c1) * f + y1;
}

void StftVoice::shift(Band& b, Channel& c, double alpha,
                      double phaseAlpha) {
    const double omega = kTwoPi * b.H / b.N;
    const bool hadPrev = c.havePrev;

    float maxMag = 0.f;
    float fluxPositive = 0.f;
    float fluxTotal = 0.f;
    float fluxEnergy = 0.f;

    // Analyze the whole frame, but detect transients primarily in the band
    // that this STFT contributes to the finished signal.
    const int fluxFirst = std::max(1, int(std::floor(
        (b.high ? 1800.0 : 80.0) * b.N / b.rate)));
    const int fluxLast = std::min(b.K - 2, int(std::ceil(
        (b.high ? b.rate * 0.48 : 3200.0) * b.N / b.rate)));

    for (int k = 0; k < b.K; ++k) {
        const size_t ix = size_t(k);
        const float magnitude = std::abs(b.bins[ix]);
        const float phase = std::arg(b.bins[ix]);
        b.mag[ix] = magnitude;
        b.ph[ix] = phase;
        maxMag = std::max(maxMag, magnitude);

        if (k >= fluxFirst && k <= fluxLast) {
            fluxEnergy += magnitude;
            if (hadPrev)
                fluxPositive += std::max(0.f, magnitude - c.prevMagnitude[ix]);
            fluxTotal += magnitude;
        }
        c.prevMagnitude[ix] = magnitude;

        double f = k;
        if (hadPrev) {
            const double err = wrapPhase(double(phase) -
                double(c.prevPhase[ix]) - k * omega);
            f += err / omega;
        }
        b.freqBin[ix] = float(f);
        c.prevPhase[ix] = phase;
    }

    const float flux = fluxPositive / std::max(fluxTotal, 1e-9f);
    const float threshold = b.high ? 0.17f : 0.22f;
    const bool onset = hadPrev && c.transientHold == 0 &&
        flux > std::max(threshold, c.fluxBaseline * 1.75f) &&
        (fluxEnergy > 1.08f * c.prevFluxEnergy || flux > 0.42f);

    c.fluxBaseline = hadPrev
        ? (0.94f * c.fluxBaseline + 0.06f * flux)
        : (0.25f * flux);
    c.prevFluxEnergy = fluxEnergy;
    if (c.transientHold > 0) --c.transientHold;
    if (onset) c.transientHold = b.high ? 3 : 2;

    std::fill(b.spec.begin(), b.spec.end(), std::complex<float>{0.f, 0.f});
    if (maxMag < 1e-9f) {
        c.trackCount = 0;
        c.havePrev = false;
        c.fluxBaseline = 0.f;
        c.prevAlpha = alpha;
        c.f0Hz = 0.f;
        c.relax = 0.f;
        return;
    }

    // Find spectral peaks.  All arrays are full-size and allocated at creation.
    const float peakThreshold = maxMag * kPeakFloor;
    int peakCount = 0;
    for (int k = 2; k < b.K - 2; ++k) {
        const float m = b.mag[size_t(k)];
        if (m > peakThreshold &&
            m > b.mag[size_t(k - 1)] && m >= b.mag[size_t(k + 1)] &&
            m > b.mag[size_t(k - 2)] && m >= b.mag[size_t(k + 2)]) {
            if (peakCount > 0 && k - b.peakBin[size_t(peakCount - 1)] < 4) {
                if (m > b.mag[size_t(b.peakBin[size_t(peakCount - 1)])])
                    b.peakBin[size_t(peakCount - 1)] = k;
            } else {
                b.peakBin[size_t(peakCount++)] = k;
            }
        }
    }
    if (peakCount == 0) {
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

    // Smoothed cepstral envelope for formant restoration / formant knob.
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
        // IMPORTANT: don't smooth a first frame against zero log-magnitude.
        float sm = hadPrev
            ? envCoef * logNow + (1.f - envCoef) * c.envSmooth[ix]
            : logNow;
        // Anti-NaN: con coefficienti >1 il filtro puo' divergere; senza questo
        // exp() andrebbe a inf e il suono morirebbe fino alla nota dopo.
        if (!std::isfinite(sm)) sm = logNow;
        c.envSmooth[ix] = std::clamp(sm, -80.f, 80.f);
        b.env[ix] = std::exp(c.envSmooth[ix]);
    }

    // --- Rilascio progressivo della preservazione formanti -------------------
    // La banda bassa stima la f0 sorgente (primo picco forte sopra 90 Hz); la banda
    // alta usa quella stima, perche' li' i picchi sono armoniche, non la f0.
    const int chIx = int(&c - b.channels.data());
    if (!b.high) {
        const int kMin = std::max(2, int(std::ceil(90.0 * b.N / b.rate)));
        for (int i = 0; i < peakCount; ++i) {
            const int kp = b.peakBin[size_t(i)];
            if (kp < kMin || b.mag[size_t(kp)] < 0.2f * maxMag) continue;
            const float f0 = float(double(b.freqBin[size_t(kp)]) * b.rate / b.N);
            c.f0Hz = c.f0Hz > 0.f ? 0.7f * c.f0Hz + 0.3f * f0 : f0;
            break;
        }
    }
    {
        using R = StftRelax;
        const double st = 12.0 * std::log2(std::max(alpha, 1e-3));
        const double outHz = double(lowBand.channels[size_t(chIx)].f0Hz) * alpha;
        // Quanto e' "alta" la nota: shift totale oppure altezza assoluta in uscita.
        const double onset = std::max(
            smooth01((st - R::kShiftOn0) / (R::kShiftOn1 - R::kShiftOn0)),
            smooth01((outHz - R::kHzOn0) / (R::kHzOn1 - R::kHzOn0)));
        const double extreme = std::max(
            smooth01((st - R::kShiftX0) / (R::kShiftX1 - R::kShiftX0)),
            smooth01((outHz - R::kHzX0) / (R::kHzX1 - R::kHzX0)));
        const double cap = R::kLow + (R::kHigh - R::kLow) * extreme;
        const float target = float(cap * onset);
        c.relax += (target - c.relax) * 0.25f; // niente scatti da un frame all'altro
        if (c.relax < 1e-4f) c.relax = 0.f;
    }
    const bool relaxOn = c.relax > 0.f;
    const float keep = 1.f - c.relax; // esponente: 1 = preservazione piena

    const float beta = float(std::clamp(
        formantScaleSmoothed_, 1.0 / 64.0, 64.0));
    auto sampleEnv = [&](float binPos) -> float {
        binPos = std::clamp(binPos, 1.f, float(b.K - 2));
        const int lo = int(std::floor(binPos));
        const int hi = std::min(lo + 1, b.K - 2);
        const float frac = binPos - lo;
        return b.env[size_t(lo)] * (1.f - frac) + b.env[size_t(hi)] * frac;
    };

    int nextTrackCount = 0;
    int trackCursor = 0;
    for (int i = 0; i < peakCount; ++i) {
        const int kp = b.peakBin[size_t(i)];
        const double fp = std::clamp(
            double(b.freqBin[size_t(kp)]), kp - 1.5, kp + 1.5);
        const double shiftedFrequency = alpha * fp;
        if (shiftedFrequency <= 0.0) continue;
        if (shiftedFrequency >= b.K - 1.0) break;

        // Integer bin relocation + *explicit* fractional remainder.
        // This fixes the phase progression at a continuous, sub-bin target,
        // instead of advancing it at a rounded destination frequency.
        const double exactShift = (alpha - 1.0) * fp;
        const int delta = int(std::lround(exactShift));
        const double fractional = exactShift - delta; // [-0.5, +0.5]
        const int neighbour = delta + (fractional >= 0.0 ? 1 : -1);
        const float primaryWeight = float(1.0 - std::abs(fractional));
        const float neighbourWeight = float(std::abs(fractional));

        const double centreAna = double(b.ph[size_t(kp)]) + kPi * kp;
        double centreSyn = centreAna;
        if (hadPrev && !onset && c.trackCount > 0) {
            while (trackCursor + 1 < c.trackCount &&
                   c.tracks[size_t(trackCursor + 1)].freq <= fp)
                ++trackCursor;
            int best = trackCursor;
            if (trackCursor + 1 < c.trackCount &&
                std::abs(c.tracks[size_t(trackCursor + 1)].freq - fp) <
                std::abs(c.tracks[size_t(best)].freq - fp))
                best = trackCursor + 1;
            if (std::abs(double(c.tracks[size_t(best)].freq) - fp) < 1.75) {
                // Advance at the *hop-averaged* MIDI pitch, not merely its
                // last sample.  This is essential on fast continuous slides.
                // The first terms explicitly retain the fractional offset;
                // the final term integrates the pitch ramp within this hop.
                const double advanceBins =
                    double(kp + delta) + (fp - kp) + fractional +
                    (phaseAlpha - alpha) * fp;
                centreSyn = double(c.tracks[size_t(best)].phase) +
                    advanceBins * omega;
            }
        }
        // On a transient, centreSyn deliberately resets to the current
        // analysis phase.  This prevents prolonged phase smearing on attacks.
        centreSyn = wrapPhase(centreSyn);
        b.newTracks[size_t(nextTrackCount++)] = {
            float(fp), float(centreSyn)
        };

        // Continuous, fractional-bin phase rotation.  Each of the two target
        // integer bins is corrected relative to the *same* non-integer peak
        // position; their phases therefore coincide at the frame centre.
        const double continuousRotation = centreSyn -
            double(b.ph[size_t(kp)]) - kPi * (kp + exactShift);
        const double rotationPrimary = wrapPhase(
            continuousRotation + kPi * (exactShift - delta));
        const double rotationNeighbour = wrapPhase(
            continuousRotation + kPi * (exactShift - neighbour));
        const std::complex<float> rot0(float(std::cos(rotationPrimary)),
                                        float(std::sin(rotationPrimary)));
        const std::complex<float> rot1(float(std::cos(rotationNeighbour)),
                                        float(std::sin(rotationNeighbour)));

        for (int k = b.regionLo[size_t(i)];
             k <= b.regionHi[size_t(i)]; ++k) {
            const float originalEnvelope = std::max(b.env[size_t(k)], 1e-9f);
            const std::complex<float> base = b.bins[size_t(k)];
            const int d0 = k + delta;
            const int d1 = k + neighbour;
            if (primaryWeight > 0.f && d0 >= 1 && d0 < b.K - 1) {
                const float targetEnvelope = sampleEnv(float(d0) / beta);
                float gain = std::clamp(
                    targetEnvelope / originalEnvelope, 0.f, kMaxGain);
                if (relaxOn) gain = std::pow(gain, keep);
                b.spec[size_t(d0)] += base * rot0 * (gain * primaryWeight);
            }
            if (neighbourWeight > 0.f && d1 >= 1 && d1 < b.K - 1) {
                const float targetEnvelope = sampleEnv(float(d1) / beta);
                float gain = std::clamp(
                    targetEnvelope / originalEnvelope, 0.f, kMaxGain);
                if (relaxOn) gain = std::pow(gain, keep);
                b.spec[size_t(d1)] += base * rot1 * (gain * neighbourWeight);
            }
        }
    }

    // Destination frequency, not source frequency, controls the crossover.
    for (int k = 1; k < b.K - 1; ++k)
        b.spec[size_t(k)] *= b.crossover[size_t(k)];
    b.spec.front() = b.spec.back() = {};

    // No resizing/swapping/push_back on the audio thread: fixed track arrays.
    for (int i = 0; i < nextTrackCount; ++i)
        c.tracks[size_t(i)] = b.newTracks[size_t(i)];
    c.trackCount = nextTrackCount;
    c.havePrev = true;
}

void StftVoice::frame(Band& b, long long start, double alpha,
                      double phaseAlpha) {
    const int channelCount = mono ? 1 : 2;
    for (int ch = 0; ch < channelCount; ++ch) {
        // start is an ABSOLUTE time index in the output/source timebase.
        // Both windows read and overlap-add at that same index.  The FFT
        // centre is start + N/2, but there is NO extra N/2 sample offset on
        // either read or write.  Consequently the 512/2048 window difference
        // introduces no relative latency, including on the pre-roll frames.
        for (int i = 0; i < b.N; ++i) {
            const long long sampleIndex = start + i;
            b.time[size_t(i)] = read(
                begin + double(sampleIndex) * step, ch) * b.window[size_t(i)];
        }
        b.fftp->fft(b.time, b.bins);
        shift(b, b.channels[size_t(ch)], alpha, phaseAlpha);
        b.fftp->ifft(b.spec, b.time);

        auto& overlap = b.channels[size_t(ch)].overlap;
        for (int i = 0; i < b.N; ++i) {
            const long long offset = start + i - position;
            if (offset >= 0 && offset < b.N) {
                overlap[size_t(offset)] += b.time[size_t(i)] *
                    b.window[size_t(i)] * b.outNorm;
            }
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
    syncDevParams();
    const int frames = int(std::max<long long>(0,
        std::min<long long>(count, length - position)));
    const double safeScale = std::isfinite(scale)
        ? std::clamp(scale, 1.0 / 64.0, 64.0) : 1.0;
    const double target = std::log2(safeScale);
    if (!smoothInit) { smoothLog = target; smoothInit = true; }
    if (snapPending_) { smoothLog = target; snapPending_ = false; }

    for (int i = 0; i < frames; ++i, ++position) {
        smoothLog += (target - smoothLog) * smoothCoef;
        if (target == 0.0 && std::abs(smoothLog) < 1e-4)
            smoothLog = 0.0;
        formantScaleSmoothed_ +=
            (formantScaleTarget_ - formantScaleSmoothed_) * formantSmoothCoef_;
        if (formantScaleTarget_ == 1.0 && std::abs(formantScaleSmoothed_ - 1.0) < 1e-5)
            formantScaleSmoothed_ = 1.0;
        // Il motore deve girare anche quando il pitch netto e' 0 ma il formante
        // del knob e' diverso da 1, altrimenti il formante sparirebbe in quel punto.
        const bool shifted = std::abs(smoothLog) > 1e-5 ||
                             std::abs(std::log2(formantScaleSmoothed_)) > 1e-5;
        const double factor = std::exp2(smoothLog);

        if (shifted && !active) {
            resetState();
            // Each band gets its OWN correctly centered warm-up frames; no
            // heap allocation, no waiting for a future audio callback.
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

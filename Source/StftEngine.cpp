#include "StftEngine.h"
#include <cmath>

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 2.0 * kPi;

int pickSize(double rate) {
    // ~46 ms window: good frequency resolution for voices, same at every host rate
    if (rate <= 50000.0) return 2048;
    if (rate <= 100000.0) return 4096;
    return 8192;
}
double wrapPhase(double p) { return p - kTwoPi * std::floor(p / kTwoPi + 0.5); }
}

StftVoice::StftVoice(std::shared_ptr<const WaveData> data, int first, int last, double rate)
    : wav(std::move(data)), begin(first), end(last),
      N(pickSize(rate)), H(N / 4), K(N / 2 + 1),
      step(wav->sampleRate / rate),
      length(static_cast<long long>(std::ceil((last - first) / step))),
      fftp(std::make_shared<stftpitchshift::RFFT>()),
      cepster(fftp, size_t(N), rate) {
    cepster.quefrency(kQuefrency);
    smoothCoef = 1.0 - std::exp(-1.0 / (kSmoothMs * 0.001 * rate));
    fadeStep = float(1.0 / (kFadeMs * 0.001 * rate));
    outNorm = float(H / (0.375 * N));       // Hann^2 overlap-add gain

    window.resize(size_t(N));
    for (int i = 0; i < N; ++i)
        window[size_t(i)] = float(0.5 - 0.5 * std::cos(kTwoPi * i / N));
    time.assign(size_t(N), 0.f);
    bins.assign(size_t(K), {});
    spec.assign(size_t(K), {});
    mag.assign(size_t(K), 0.f);
    ph.assign(size_t(K), 0.f);
    env.assign(size_t(K), 0.f);
    freqBin.assign(size_t(K), 0.f);
    synMag.assign(size_t(K), 0.f);
    peakBin.reserve(size_t(K));
    regionLo.reserve(size_t(K));
    regionHi.reserve(size_t(K));
    newTracks.reserve(size_t(K));
    for (auto& c : chan) {
        c.overlap.assign(size_t(N), 0.f);
        c.prevPhase.assign(size_t(K), 0.f);
        c.envSmooth.assign(size_t(K), 0.f);   // [FIX FORMANT]
        c.tracks.reserve(size_t(K));
    }

    // Mono files (L == R) are processed once and copied: half the CPU,
    // and no chance of the two channels drifting apart.
    mono = true;
    const auto& s = wav->stereo;
    for (int i = std::max(0, first); i < std::min(last, wav->frames()); ++i)
        if (s[size_t(i) * 2] != s[size_t(i) * 2 + 1]) { mono = false; break; }
}

void StftVoice::resetState() {
    for (auto& c : chan) {
        std::fill(c.overlap.begin(), c.overlap.end(), 0.f);
        std::fill(c.prevPhase.begin(), c.prevPhase.end(), 0.f);
        std::fill(c.envSmooth.begin(), c.envSmooth.end(), 0.f);
        c.tracks.clear();
        c.havePrev = false;
    }
    cursor = 0;
}

// Catmull-Rom interpolation: much cleaner than linear when the WAV rate
// differs from the host rate (linear interpolation adds high-frequency hiss).
float StftVoice::read(double where, int ch) const {
    if (where < begin || where >= end) return 0;
    const float* s = wav->stereo.data();
    const int a = int(where);
    const float f = float(where - a);
    auto at = [&](int i) { return s[size_t(std::clamp(i, begin, end - 1)) * 2 + size_t(ch)]; };
    if (f == 0.f) return at(a);
    const float y0 = at(a - 1), y1 = at(a), y2 = at(a + 1), y3 = at(a + 2);
    const float c1 = 0.5f * (y2 - y0);
    const float c2 = y0 - 2.5f * y1 + 2.f * y2 - 0.5f * y3;
    const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
    return ((c3 * f + c2) * f + c1) * f + y1;
}

// bins (analysis) -> spec (synthesis), pitch factor `alpha`, formants kept in place.
void StftVoice::shift(Channel& c, double alpha) {
    const double omega = kTwoPi * H / N;   // phase advance per hop of bin 1

    float maxMag = 0.f;
    for (int k = 0; k < K; ++k) {
        mag[size_t(k)] = std::abs(bins[size_t(k)]);
        ph[size_t(k)] = std::arg(bins[size_t(k)]);
        maxMag = std::max(maxMag, mag[size_t(k)]);
    }

    // Instantaneous frequency of every bin (in bins) from the phase advance.
    for (int k = 0; k < K; ++k) {
        double f = k;
        if (c.havePrev) {
            const double d = wrapPhase(double(ph[size_t(k)]) - double(c.prevPhase[size_t(k)]) - k * omega);
            f = k + d / omega;
        }
        freqBin[size_t(k)] = float(f);
        c.prevPhase[size_t(k)] = ph[size_t(k)];
    }
    c.havePrev = true;

    std::fill(spec.begin(), spec.end(), std::complex<float>{});
    if (maxMag < 1e-9f) { c.tracks.clear(); return; }

    // ---- spectral peaks (at least 4 bins apart: Hann side lobes are not peaks)
    const float thr = maxMag * kPeakFloor;
    peakBin.clear();
    for (int k = 2; k < K - 2; ++k) {
        const float m = mag[size_t(k)];
        if (m > thr && m > mag[size_t(k - 1)] && m >= mag[size_t(k + 1)] &&
            m > mag[size_t(k - 2)] && m >= mag[size_t(k + 2)]) {
            if (!peakBin.empty() && k - peakBin.back() < 4) {
                if (m > mag[size_t(peakBin.back())]) peakBin.back() = k;
            } else {
                peakBin.push_back(k);
            }
        }
    }
    if (peakBin.empty()) {
        int best = 1;
        for (int k = 1; k < K - 1; ++k) if (mag[size_t(k)] > mag[size_t(best)]) best = k;
        peakBin.push_back(best);
    }

    // ---- region of influence of every peak: split at the valley between peaks
    const int n = int(peakBin.size());
    regionLo.assign(size_t(n), 1);
    regionHi.assign(size_t(n), K - 2);
    for (int i = 0; i + 1 < n; ++i) {
        int valley = peakBin[size_t(i)] + 1;
        for (int k = valley; k < peakBin[size_t(i + 1)]; ++k)
            if (mag[size_t(k)] < mag[size_t(valley)]) valley = k;
        regionHi[size_t(i)] = valley;
        regionLo[size_t(i + 1)] = valley + 1;
    }

    // ---- formant envelope with temporal smoothing
    for (int k = 0; k < K; ++k) env[size_t(k)] = std::max(mag[size_t(k)], 1e-9f);
    cepster.lifter(env);
    for (int k = 0; k < K; ++k) {
        env[size_t(k)] = std::max(env[size_t(k)], 1e-9f);
        const float logNow = std::log(env[size_t(k)]);
        if (c.havePrev)
            c.envSmooth[size_t(k)] = kEnvSmoothCoef * logNow +
                                     (1.f - kEnvSmoothCoef) * c.envSmooth[size_t(k)];
        else
            c.envSmooth[size_t(k)] = logNow;
        env[size_t(k)] = std::exp(c.envSmooth[size_t(k)]);
    }

    // ---- move every region, keep one continuous phase track per peak
    std::fill(synMag.begin(), synMag.end(), 0.f);
    newTracks.clear();
    size_t t = 0;
    for (int i = 0; i < n; ++i) {
        const int kp = peakBin[size_t(i)];
        const double fp = std::clamp(double(freqBin[size_t(kp)]), kp - 1.5, kp + 1.5);
        const double fn = alpha * fp;                       // true new frequency (bins)
        if (fn >= K - 2) break;                              // above Nyquist: peaks are sorted

        // [FIX SCRATCH + GHOST] Fractional destination bin, but the phase
        // correction (-1)^k term must use INTEGER destination bins. Mixing
        // a fractional delta into the phase rotation adds a spurious pi/2
        // and produces a ghost harmonic while gliding. Splitting the energy
        // over the two adjacent INTEGER bins (fracLo / fracHi) keeps the
        // glide continuous without introducing spurious harmonics.
        const double deltaExact = (alpha - 1.0) * fp;
        const int    deltaLo    = int(std::floor(deltaExact));
        const int    deltaHi    = deltaLo + 1;
        const float  fracHi     = float(deltaExact - deltaLo);
        const float  fracLo     = 1.f - fracHi;

        // Phase of the sinusoid at the CENTRE of the window (the bin phase carries
        // a (-1)^k term that would otherwise break continuity when kn changes).
        const double centreAna = double(ph[size_t(kp)]) + kPi * kp;
        double centreSyn = centreAna;
        if (c.havePrev && !c.tracks.empty()) {
            while (t + 1 < c.tracks.size() && c.tracks[t + 1].freq <= fp) ++t;
            size_t best = t;
            if (t + 1 < c.tracks.size() &&
                std::abs(c.tracks[t + 1].freq - fp) < std::abs(c.tracks[t].freq - fp)) best = t + 1;
            if (std::abs(c.tracks[best].freq - fp) < 1.75)
                centreSyn = double(c.tracks[best].phase) + fn * omega;
        }
        centreSyn = wrapPhase(centreSyn);
        newTracks.push_back({float(fp), float(centreSyn)});

        // Two integer phase references. Since kkHi == kkLo + 1, thetaHi == thetaLo - pi,
        // which is the natural phase relation between adjacent FFT bins.
        const double thetaBase = centreSyn - double(ph[size_t(kp)]);
        const double thetaLo   = thetaBase - kPi * (kp + deltaLo);
        const double thetaHi   = thetaBase - kPi * (kp + deltaHi);
        const std::complex<float> rotLo(float(std::cos(thetaLo)), float(std::sin(thetaLo)));
        const std::complex<float> rotHi(float(std::cos(thetaHi)), float(std::sin(thetaHi)));

        for (int k = regionLo[size_t(i)]; k <= regionHi[size_t(i)]; ++k) {
            const int kkLo = k + deltaLo;
            const int kkHi = k + deltaHi;
            const std::complex<float> base = bins[size_t(k)];
            if (fracLo > 0.f && kkLo >= 1 && kkLo <= K - 2) {
                const float g = std::clamp(env[size_t(kkLo)] / env[size_t(k)], 0.f, kMaxGain);
                const std::complex<float> v = base * rotLo * g * fracLo;
                const float m2 = std::norm(v);
                if (m2 > synMag[size_t(kkLo)]) { spec[size_t(kkLo)] = v; synMag[size_t(kkLo)] = m2; }
            }
            if (fracHi > 0.f && kkHi >= 1 && kkHi <= K - 2) {
                const float g = std::clamp(env[size_t(kkHi)] / env[size_t(k)], 0.f, kMaxGain);
                const std::complex<float> v = base * rotHi * g * fracHi;
                const float m2 = std::norm(v);
                if (m2 > synMag[size_t(kkHi)]) { spec[size_t(kkHi)] = v; synMag[size_t(kkHi)] = m2; }
            }
        }
    }
    c.tracks.swap(newTracks);
}

void StftVoice::frame(long long start, double scale) {
    const int channels = mono ? 1 : 2;
    for (int ch = 0; ch < channels; ++ch) {
        for (int i = 0; i < N; ++i)
            time[size_t(i)] = read(begin + (start + i) * step, ch) * window[size_t(i)];
        fftp->fft(time, bins);
        shift(chan[size_t(ch)], scale);
        spec.front() = spec.back() = 0;
        fftp->ifft(spec, time);
        auto& overlap = chan[size_t(ch)].overlap;
        for (int i = 0; i < N; ++i) {
            const long long offset = start + i - position;
            if (offset >= 0 && offset < N)
                overlap[size_t(offset)] += time[size_t(i)] * window[size_t(i)] * outNorm;
        }
    }
}

int StftVoice::render(float* left, float* right, int count, double scale) {
    const int frames = int(std::max<long long>(0, std::min<long long>(count, length - position)));
    const double target = std::log2(std::clamp(scale, 1.0 / 64.0, 64.0));
    if (!smoothInit) { smoothLog = target; smoothInit = true; }
    const int channels = mono ? 1 : 2;

    for (int i = 0; i < frames; ++i, ++position) {
        // pitch factor glides smoothly between the (stepwise) values of the host
        smoothLog += (target - smoothLog) * smoothCoef;
        if (target == 0.0 && std::abs(smoothLog) < 1e-4) smoothLog = 0.0;
        const bool shifted = std::abs(smoothLog) > 1e-5;
        const double factor = std::exp2(smoothLog);

        if (shifted && !active) {
            resetState();
            for (int offset = -N + H; offset <= 0; offset += H) frame(position + offset, factor);
            active = true;
            cursor = 0;
            wet = position == 0 ? 1.f : 0.f;   // note starts shifted: no dry attack
        }
        if (active && cursor == H) {
            for (int ch = 0; ch < channels; ++ch) {
                auto& o = chan[size_t(ch)].overlap;
                std::move(o.begin() + H, o.end(), o.begin());
                std::fill(o.end() - H, o.end(), 0.f);
            }
            frame(position, factor);
            cursor = 0;
        }

        const float wetTarget = shifted ? 1.f : 0.f;
        wet += std::clamp(wetTarget - wet, -fadeStep, fadeStep);

        float dl = 0.f, dr = 0.f, wl = 0.f, wr = 0.f;
        if (wet < 1.f) {
            dl = read(begin + position * step, 0);
            dr = mono ? dl : read(begin + position * step, 1);
        }
        if (active) {
            wl = chan[0].overlap[size_t(cursor)];
            wr = mono ? wl : chan[1].overlap[size_t(cursor)];
            ++cursor;
        }
        left[i] = dl * (1.f - wet) + wl * wet;
        right[i] = dr * (1.f - wet) + wr * wet;

        // back to the original pitch and fully faded: stop spending CPU
        if (active && !shifted && wet == 0.f) { active = false; }
    }
    return frames;
}

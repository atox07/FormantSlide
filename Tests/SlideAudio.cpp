#include "SlideVoice.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <numbers>
#include <stdexcept>
#include <vector>

namespace {
constexpr double tau = 2. * std::numbers::pi;
constexpr int block = 47; // Deliberately misaligned with the DSP interval.

std::shared_ptr<WaveData> makeWave(int rate, bool rough) {
    auto wave = std::make_shared<WaveData>();
    wave->sampleRate = rate;
    wave->stereo.resize(2 * rate);
    uint32_t random = 42;
    for (int i = 0; i < rate; ++i) {
        const double phase = tau * 211. * i / rate;
        random = random * 1664525u + 1013904223u;
        const double noise = double(random) / 2147483648. - 1.;
        const double tone = std::sin(phase) + .25 * std::sin(3*phase);
        float v = float(rough ? .32*std::tanh(3.*tone + .4*noise) : .24*tone);
        wave->stereo[2*i] = v;
        wave->stereo[2*i+1] = -.75f*v;
    }
    return wave;
}

double bin(const std::vector<float>& audio, int start, int size, int rate, double hz) {
    double re = 0., im = 0.;
    for (int i = 0; i < size; ++i) {
        const double a = tau * hz * i / rate;
        const double w = .5 - .5*std::cos(tau*i/size);
        re += audio[start+i]*w*std::cos(a);
        im += audio[start+i]*w*std::sin(a);
    }
    return std::hypot(re, im);
}

void check(int rate, bool rough, double semitones) {
    auto wav = makeWave(rate, rough);
    SlideVoice voice(wav, 0, wav->frames(), rate, 211.);
    constexpr int delay = SlideVoice::latencySamples;
    const int length = rate + delay;
    std::vector<float> l(length), r(length);
    for (int t = 0; t < length;) {
        const int n = std::min(block, length - t);
        const double ramp = std::clamp((double(t+n)/rate - .25) / .20, 0., 1.);
        voice.setSlideActive(semitones != 0. && ramp > 0.);
        if (voice.render(l.data()+t, r.data()+t, n, std::exp2(semitones*ramp/12.)) != n)
            throw std::runtime_error("Truncated audio");
        t += n;
    }
    for (int t = 0; t < length; ++t) {
        if (!std::isfinite(l[t]) || !std::isfinite(r[t])) throw std::runtime_error("Non-finite sample");
        if (t < delay && (l[t] != 0.f || r[t] != 0.f)) throw std::runtime_error("Early output");
        if (t >= delay && (semitones == 0. || t < delay + rate/5)) {
            if (std::memcmp(&l[t], &wav->stereo[2*(t-delay)], sizeof(float)) ||
                std::memcmp(&r[t], &wav->stereo[2*(t-delay)+1], sizeof(float)))
                throw std::runtime_error("Dry null failed");
        }
    }
    if (semitones == 0.) return;
    const double target = 211.*std::exp2(semitones/12.);
    const int at = delay + 3*rate/5, size = rate/3;
    double bestPower = 0., bestHz = 0.;
    for (int cent = -240; cent <= 240; ++cent) {
        const double hz = target*std::exp2(cent/1200.);
        const double power = bin(l, at, size, rate, hz);
        if (power > bestPower) { bestPower = power; bestHz = hz; }
    }
    const double cents = 1200.*std::log2(bestHz/target);
    const double residueDb = 20.*std::log10((bin(l, at, size, rate, 211.)+1.e-9)/(bestPower+1.e-9));
    const int window = int(std::round(8.*rate/target));
    std::vector<double> levels;
    for (int i = at; i + window <= at + size; i += window) {
        double power = 0.;
        for (int k = 0; k < window; ++k) power += double(l[i+k])*l[i+k];
        levels.push_back(10.*std::log10(power/window + 1.e-20));
    }
    std::sort(levels.begin(), levels.end());
    const double wobble = levels[9*levels.size()/10] - levels[levels.size()/10];
    std::printf("%d Hz, %s, %+.0f st: pitch %+.1f cents, source %.1f dB relative to target, RMS p90-p10 %.2f dB\n",
        rate, rough ? "distorted + noise" : "clean", semitones, cents, residueDb, wobble);
    if (bestPower < .001*size) throw std::runtime_error("Held fundamental too weak");
    if (std::abs(cents) > 20.) throw std::runtime_error("Held pitch exceeds 20 cents");
    if (residueDb > -10.) throw std::runtime_error("Original pitch too prominent");
    if (wobble > 6.) throw std::runtime_error("Held level variation exceeds 6 dB");
}

void checkSamplerKnob(int rate) {
    auto wav = makeWave(rate, true);
    SlideVoice voice(wav, 0, wav->frames(), rate, 211.);
    voice.setPlaybackScale(2.);
    std::array<float, block> left{}, right{};
    constexpr int delay = SlideVoice::latencySamples;
    for (int t = 0; t + block < delay + rate/2; t += block) {
        if (voice.render(left.data(), right.data(), block, 1.) != block)
            throw std::runtime_error("Knob truncated the slice");
        for (int i = 0; i < block; ++i) {
            if (t+i < delay) continue;
            const size_t source = 4 * size_t(t+i-delay);
            if (left[i] != wav->stereo[source] || right[i] != wav->stereo[source+1])
                throw std::runtime_error("Pitch knob must resample the entire waveform");
        }
    }
}

void checkVowelFormants(int rate) {
    constexpr double f0 = 110.;
    const double ratio = std::exp2(7./12.);
    constexpr std::array<double, 2> centres{720., 2300.};
    constexpr std::array<double, 2> widths{160., 260.};
    auto wave = std::make_shared<WaveData>();
    wave->sampleRate = rate;
    wave->stereo.assign(2*rate, 0.f);
    for (int h = 1; h*f0 < 5000.; ++h) {
        double amplitude = .01/h;
        for (size_t j = 0; j < centres.size(); ++j) {
            const double d = (h*f0-centres[j])/widths[j];
            amplitude += .03*std::exp(-.5*d*d);
        }
        for (int i = 0; i < rate; ++i)
            wave->stereo[2*i] += float(amplitude*std::sin(tau*h*f0*i/rate));
    }
    for (int i = 0; i < rate; ++i) wave->stereo[2*i+1] = wave->stereo[2*i];
    SlideVoice voice(wave, 0, wave->frames(), rate, f0);
    voice.setSlideActive(true);
    std::vector<float> left(rate), right(rate);
    for (int t = 0; t < rate; t += block) {
        const int n = std::min(block, rate - t);
        const double ramp = std::min(1., 4.*(t+n)/rate);
        if (voice.render(left.data()+t, right.data()+t, n, std::pow(ratio, ramp)) != n)
            throw std::runtime_error("Short vowel render");
    }
    // Estimate each envelope maximum by sampling the output harmonics.
    // At the new F0 the estimate is quantised to one harmonic spacing.
    for (size_t j = 0; j < centres.size(); ++j) {
        double strongest = 0., measured = 0.;
        for (int h = 1; h*f0*ratio < 4500.; ++h) {
            const double hz = h*f0*ratio;
            if (hz < .7*centres[j] || hz > 1.65*centres[j]) continue;
            const double magnitude = bin(left, rate/2, rate/4, rate, hz);
            if (magnitude > strongest) { strongest = magnitude; measured = hz; }
        }
        std::printf("%d Hz vowel +7 st, F%zu: nominal %.0f Hz, strongest harmonic %.0f Hz\n",
                    rate, j+1, centres[j], measured);
        if (strongest < .001 || std::abs(measured-centres[j]) > std::max(f0*ratio, .12*centres[j]))
            throw std::runtime_error("Vowel resonance moved outside tolerance");
    }
}

void measureEightVoices(int rate) {
    auto wave = makeWave(rate, true);
    std::array<std::unique_ptr<SlideVoice>, 8> voices;
    for (auto& voice : voices) {
        voice = std::make_unique<SlideVoice>(wave, 0, rate, rate, 211.);
        voice->setSlideActive(true);
    }
    std::array<float, block> left{}, right{};
    const auto start = std::chrono::steady_clock::now();
    for (int t = 0; t < rate; t += block) {
        const int n = std::min(block, rate - t);
        const double ratio = std::exp2(.75 * (t+n) / rate);
        for (auto& voice : voices)
            if (voice->render(left.data(), right.data(), n, ratio) != n)
                throw std::runtime_error("Short polyphonic render");
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
    std::printf("%d Hz, 8 concurrent voices: %.1f ms per second of audio, %.2f%% of one core per voice (wall time)\n",
                rate, 1000.*seconds, 100.*seconds/voices.size());
}
}

int main() {
    try {
        for (int rate : {44100, 48000}) {
            check(rate, false, 0.);
            check(rate, false, 12.);
            check(rate, true, 7.);
            check(rate, true, -7.);
            checkSamplerKnob(rate);
            checkVowelFormants(rate);
            measureEightVoices(rate);
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
}

#include "StftEngine.h"
#include <cmath>
#include <numbers>
#include <new>
#include <utility>

namespace {
constexpr double pi = std::numbers::pi;
constexpr double tau = 2.0 * pi;

double wrap(double phase) {
    return phase - tau * std::round(phase / tau);
}

double boundedLogPitch(double ratio) {
    return std::isfinite(ratio) ? std::log2(std::clamp(ratio, 1./16., 16.)) : 0.;
}
}

StftVoice::StftVoice(std::shared_ptr<const WaveData> data, int first, int last,
                     double rate, double hz)
    : wav(std::move(data)), begin(std::clamp(first, 0, wav->frames())),
      end(std::clamp(last, begin, wav->frames())), step(wav->sampleRate / rate),
      startNote(hz), position(begin) {
    initStretch(rate, wav->estimateF0(begin, end, hz));
}

void StftVoice::initStretch(double rate, double sourceF0) {
    fftSize = sourceF0 < 150. ? 2048 : 1024;
    hop = fftSize / 4;
    bins = fftSize / 2 + 1;
    lifter = std::clamp(int(std::lround(35. * rate / 44100.)), 1, fftSize / 4);
    fadeSamples = std::max(1, int(std::lround(0.020 * rate)));
    playbackFollow = -std::expm1(-1. / (0.003 * rate));
    for (int i = 0; i < fftSize; ++i)
        window[i] = float(0.5 - 0.5 * std::cos(tau * i / fftSize));
    for (int i = 0; i <= latencySamples; ++i) {
        const double angle = 0.5 * pi * i / latencySamples;
        crossfade[i] = {float(std::cos(angle)), float(std::sin(angle))};
    }
    forward.reset(kiss_fftr_alloc(fftSize, 0, nullptr, nullptr));
    inverse.reset(kiss_fftr_alloc(fftSize, 1, nullptr, nullptr));
    if (!forward || !inverse) throw std::bad_alloc();
}

StftVoice::Stereo StftVoice::read(double where) const noexcept {
    const int frame = int(where);
    const float fraction = float(where - frame);
    const auto sample = [&](int offset, int channel) {
        return wav->stereo[2 * size_t(std::clamp(frame + offset, begin, end - 1)) + channel];
    };
    Stereo result{sample(0, 0), sample(0, 1)};
    if (fraction == 0.f) return result;
    for (int channel = 0; channel < 2; ++channel) {
        const float a = sample(-1, channel), b = result[channel];
        const float c = sample(1, channel), d = sample(2, channel);
        result[channel] = b + 0.5f * fraction *
            ((c - a) + fraction * ((2.f * a - 5.f * b + 4.f * c - d)
                                    + fraction * (3.f * (b - c) + d - a)));
    }
    return result;
}

bool StftVoice::detectTransient(float flux, float magnitudeSum) noexcept {
    const float mean = fluxCount > 0 ? std::max(0.f, fluxSum) / fluxCount : 0.f;
    // A relative floor stops tiny spectral changes in a steady vowel from
    // repeatedly resetting its phase when the flux history is near zero.
    const bool onset = haveSpectrum && !transientHold && fluxCount >= 2
                    && flux > std::max(2.f * mean, 0.04f * magnitudeSum) + 1.e-6f;
    if (onset || (transientHold > 0 && flux > std::max(mean, 0.02f * magnitudeSum) + 1.e-6f)) transientHold = 2;
    else if (transientHold > 0) --transientHold;
    if (haveSpectrum) {
        fluxSum += flux - fluxHistory[fluxIndex];
        fluxHistory[fluxIndex] = flux;
        fluxIndex = (fluxIndex + 1) % int(fluxHistory.size());
        fluxCount = std::min(fluxCount + 1, int(fluxHistory.size()));
    }
    return onset;
}

void StftVoice::estimateEnvelope() noexcept {
    const float floor = std::max(1.e-12f, *std::max_element(magnitude.begin(),
                                                           magnitude.begin() + bins) * 1.e-4f);
    for (int k = 0; k < bins; ++k)
        cepstrumSpectrum[k] = {std::log(std::max(floor, magnitude[k])), 0.f};
    kiss_fftri(inverse.get(), cepstrumSpectrum.data(), time.data());
    // Taper the last quarter of the lifter to avoid ripple in the envelope.
    const int taperStart = 3 * lifter / 4;
    for (int q = 0; q <= lifter; ++q) {
        const float weight = float(std::min(1., double(lifter + 1 - q) / (lifter + 1 - taperStart)));
        time[q] *= weight / fftSize;
        if (q != 0) time[fftSize - q] *= weight / fftSize;
    }
    std::fill(time.begin() + lifter + 1, time.begin() + fftSize - lifter, 0.f);
    kiss_fftr(forward.get(), time.data(), cepstrumSpectrum.data());
    for (int k = 0; k < bins; ++k)
        envelope[k] = cepstrumSpectrum[k].r;
}

void StftVoice::processFrame() noexcept {
    const int half = fftSize / 2;
    const int64_t first = clock - fftSize + 1;
    const auto& control = input[ringIndex(first + half)];
    const double ratio = std::exp2(control.pitch);
    for (int channel = 0; channel < 2; ++channel) {
        // Zero-phase FFT. The bin remap rotates lobes about the window center.
        for (int n = 0; n < fftSize; ++n)
            time[(n + half) & (fftSize - 1)] = window[n] *
                (first + n >= 0 ? std::clamp(input[ringIndex(first + n)].sample[channel], -8.f, 8.f) : 0.f);
        kiss_fftr(forward.get(), time.data(), spectrum[channel].data());
        std::fill_n(shifted[channel].begin(), bins, kiss_fft_cpx{});
    }
    float maxPeak = 0.f, flux = 0.f, magnitudeSum = 0.f, inputPower = 0.f;
    // Use mid for the peak map. Side energy supplies the reference in bins
    // where a wide or anti-phase source cancels in mid.
    const auto power = [](kiss_fft_cpx z) { return z.r*z.r + z.i*z.i; };
    for (int k = 0; k < bins; ++k) {
        const auto l = spectrum[0][k], r = spectrum[1][k];
        const float binPower = power(l) + power(r);
        inputPower += binPower;
        magnitude[k] = std::sqrt(binPower);
        magnitudeSum += magnitude[k];
        const float midPower = power({l.r + r.r, l.i + r.i});
        const float sidePower = power({l.r - r.r, l.i - r.i});
        cepstrumSpectrum[k].i = std::max(midPower, 0.0625f * sidePower);
        maxPeak = std::max(maxPeak, cepstrumSpectrum[k].i);
        flux += std::max(0.f, magnitude[k] - previousMagnitude[k]);
    }
    const bool reset = detectTransient(flux, magnitudeSum) || !haveSpectrum;
    int peakCount = 0, strongest = 0;
    for (int k = 1; k < half; ++k) {
        const float candidate = cepstrumSpectrum[k].i;
        if (candidate > cepstrumSpectrum[strongest].i) strongest = k;
        if (candidate > 0.0001f * maxPeak && candidate > cepstrumSpectrum[k-1].i &&
            candidate > cepstrumSpectrum[k+1].i)
            peaks[peakCount++] = k;
    }
    if (peakCount == 0) peaks[peakCount++] = strongest;
    const bool sameEnvelope = ratio == 1. && control.formantScale == 1.f;
    if (!sameEnvelope) estimateEnvelope();
    const double frameRatio = 0.5 * (ratio + previousRatio);
    const auto angularStep = [&](int k) { return tau * hop * k / fftSize; };
    int start = 0;
    for (int p = 0; p < peakCount; ++p) {
        const int peak = peaks[p];
        const int last = p + 1 == peakCount ? half : (peak + peaks[p+1]) / 2;
        double phase = 0.;
        if (!reset) {
            // Advance the shared phase with the phase difference summed over
            // L/R. This is defined even for fully anti-phase stereo material.
            float re = 0.f, im = 0.f;
            for (int ch = 0; ch < 2; ++ch) {
                const auto a = spectrum[ch][peak], b = previous[ch][peak];
                re += a.r*b.r + a.i*b.i;
                im += a.i*b.r - a.r*b.i;
            }
            const double expected = angularStep(peak);
            const double advance = expected + wrap(std::atan2(im, re) - expected);
            phase = wrap(rotation[peak] + (frameRatio - 1.) * advance);
            if (ratio == 1.) phase *= 0.5;
        }
        const float c = float(std::cos(phase)), s = float(std::sin(phase));
        for (int k = start; k <= last; ++k) {
            nextRotation[k] = float(phase);
            const int dest = int(std::lround(k * ratio));
            if (dest > half) continue;
            float gain = 1.f;
            if (!sameEnvelope) {
                const float envBin = std::min(float(half), dest / control.formantScale);
                const int low = int(envBin), high = std::min(low + 1, half);
                const float target = std::lerp(envelope[low], envelope[high], envBin - low);
                gain = std::exp(std::clamp(target - envelope[k], -1.6f, 1.6f));
            }
            for (int ch = 0; ch < 2; ++ch) {
                const auto src = spectrum[ch][k];
                auto& out = shifted[ch][dest];
                out.r += gain * (c * src.r - s * src.i);
                out.i += gain * (s * src.r + c * src.i);
            }
        }
        start = last + 1;
    }
    rotation.swap(nextRotation);
    spectrum.swap(previous);
    magnitude.swap(previousMagnitude);
    haveSpectrum = true;
    previousRatio = ratio;
    float outputPower = 0.f;
    for (int ch = 0; ch < 2; ++ch) {
        shifted[ch][0].i = shifted[ch][half].i = 0.f;
        for (int k = 0; k < bins; ++k) outputPower += power(shifted[ch][k]);
    }
    // A common gain preserves the stereo balance and softens level losses
    // when a shifted harmonic lands between two formants.
    const float level = std::clamp(std::sqrt(inputPower / std::max(outputPower, 1.e-20f)), 0.5f, 4.f);
    const float synthesisGain = level / (1.5f * fftSize);
    for (int ch = 0; ch < 2; ++ch) {
        kiss_fftri(inverse.get(), shifted[ch].data(), time.data());
        for (int n = 0; n < fftSize; ++n)
            ola[ringIndex(first + latencySamples + n)][ch] +=
                time[(n + half) & (fftSize - 1)] * window[n] * synthesisGain;
    }
}

int StftVoice::render(float* left, float* right, int count, double scale) noexcept {
    const double targetPitch = boundedLogPitch(scale);
    if (clock == 0) playback = playbackTarget;
    if (snapPitch) previousPitch = targetPitch;
    snapPitch = false;
    const double pitchStep = count > 0 ? (targetPitch - previousPitch) / count : 0.;
    const double formantFollow = startNote >= limit ? std::clamp(double(guard * relax), 0., 1.) : 0.;
    int i = 0;
    for (; i < count; ++i, ++clock) {
        const bool releasing = releaseAt >= 0;
        const int64_t releaseAge = releasing ? clock - releaseAt : 0;
        if (sourceEnd < 0 && (position >= end || (releasing && releaseAge >= fadeSamples)))
            sourceEnd = clock;
        if (sourceEnd >= 0 && clock >= sourceEnd + latencySamples) break;

        const float gain = releasing ? std::max(0.f, 1.f - float(releaseAge) / fadeSamples) : 1.f;
        const auto dry = sourceEnd < 0 ? read(position) : Stereo{};
        playback += playbackFollow * (playbackTarget - playback);
        if (std::abs(playbackTarget - playback) < 1.e-9) playback = playbackTarget;
        if (sourceEnd < 0) position += step * playback;

        const double pitch = previousPitch + pitchStep * (i + 1);
        const bool active = sourceEnd < 0 && (slideActive || pitch != 0.);
        mixStep = active ? std::min(fadeSamples, mixStep + 1) : std::max(0, mixStep - 1);
        const float envelopeScale = float(formantFollow == 0. ? formant : formant * std::exp2(formantFollow * pitch));
        input[ringIndex(clock)] = {dry, float(pitch), envelopeScale, float(mixStep) / fadeSamples, gain};
        if (mixStep > 0 && sourceEnd < 0) {
            if (clock > processThrough) {
                haveSpectrum = false;
                fluxSum = 0.f;
                fluxIndex = fluxCount = transientHold = 0;
                fluxHistory.fill(0.f);
                nextFrame = clock;
            }
            processThrough = clock + fftSize - 1;
        }
        if (clock == nextFrame && clock <= processThrough) {
            processFrame();
            nextFrame += hop;
        }

        auto& wet = ola[ringIndex(clock)];
        Stereo output{};
        if (clock >= latencySamples) {
            const auto& delayed = input[ringIndex(clock - latencySamples)];
            output = delayed.sample;
            if (delayed.mix > 0.f) {
                if (delayed.mix >= 1.f) output = wet;
                else {
                    const auto gains = crossfade[int(delayed.mix * latencySamples + 0.5f)];
                    // Near unity the two paths are correlated. Compensate the
                    // equal-power curve there to avoid a 3 dB level bump.
                    const float ratio = std::clamp(std::abs(delayed.pitch) * 24.f, 0.f, 1.f);
                    const float norm = 1.f + (1.f - ratio) * (gains[0] + gains[1] - 1.f);
                    for (int ch = 0; ch < 2; ++ch)
                        output[ch] = (output[ch] * gains[0] + wet[ch] * gains[1]) / norm;
                }
            }
            if (delayed.gain != 1.f)
                for (float& sample : output) sample *= delayed.gain;
        }
        left[i] = output[0];
        right[i] = output[1];
        wet = {};
    }
    previousPitch = targetPitch;
    return i;
}

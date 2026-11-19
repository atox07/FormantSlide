#include "SlideVoice.h"
#include <cmath>
#include <numbers>
#include <stdexcept>
#include <utility>

SlideVoice::SlideVoice(std::shared_ptr<const WaveData> data, int first, int last,
                       double rate, double nominalHz)
    : wav(std::move(data)), begin(std::clamp(first, 0, wav->frames())),
      end(std::clamp(last, begin, wav->frames())), step(wav->sampleRate / rate),
      position(begin), playbackFollow(-std::expm1(-1. / (0.003 * rate))),
      baseFrequency(wav->slicePitchHz(begin, end, nominalHz) / rate),
      fadeSamples(std::max(1, int(std::lround(0.020 * rate)))) {
    stretch.configure(2, latencySamples, interval, false);
    inputDelay = stretch.inputLatency();
    wetPadding = latencySamples - inputDelay - stretch.outputLatency();
    if (wetPadding < 0 || wetPadding >= ringSize)
        throw std::runtime_error("Unexpected Signalsmith processing delay");
    stretch.setFormantFactor(1.f, true);
    // Exercise the largest seek and process calls before the voice is published.
    stretch.seek(past, history, 1.);
    stretch.process(in, quantum, out, quantum);
    stretch.reset();
}

SlideVoice::Stereo SlideVoice::read(double where) const noexcept {
    const double bounded = std::clamp(where, double(begin), double(end - 1));
    const int frame = int(bounded);
    const float fraction = float(bounded - frame);
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

void SlideVoice::prime(int64_t start) {
    stretch.reset();
    for (int i = 0; i < history; ++i) {
        const int64_t t = start - history + i;
        const auto sample = t >= 0 ? frame(t).dry : Stereo{};
        for (int ch = 0; ch < 2; ++ch) past[ch][i] = sample[ch];
    }
    stretch.seek(past, history, 1.);
}

void SlideVoice::process(int count, int64_t start) {
    if (start > wetUntil) {
        running = false;
        return;
    }
    if (!running) {
        prime(start);
        running = true;
    }
    for (int i = 0; i < count; ++i) {
        const auto dry = frame(start + i).dry;
        for (int ch = 0; ch < 2; ++ch) in[ch][i] = dry[ch];
    }
    // The library centres its automation at its analysis time. Read the
    // slide control at that time, just as we delay the audio for the mixer.
    const auto& control = frame(std::max<int64_t>(0, start + count/2 - inputDelay));
    stretch.setTransposeFactor(std::exp2(control.pitch));
    stretch.setFormantBase(control.baseFrequency);
    stretch.process(in, count, out, count);
    for (int i = 0; i < count; ++i)
        frame(start + i).wet = {out[0][i], out[1][i]};
}

SlideVoice::Stereo SlideVoice::mix(const Stereo& dry, const Stereo& wet, float amount) {
    if (amount == 1.f) return wet;
    const auto dot = [](const Stereo& a, const Stereo& b) {
        return double(a[0])*b[0] + double(a[1])*b[1];
    };
    // Equal power for unrelated signals, with unity gain where the delayed
    // dry and wet still coincide near the beginning or end of a glide.
    dryPower += playbackFollow * (dot(dry, dry) - dryPower);
    wetPower += playbackFollow * (dot(wet, wet) - wetPower);
    crossPower += playbackFollow * (dot(dry, wet) - crossPower);
    const double correlation = std::clamp(crossPower / std::sqrt(dryPower*wetPower + 1.e-30), 0., 1.);
    const double angle = amount * (std::numbers::pi / 2.);
    const double a = std::cos(angle), b = std::sin(angle);
    const double gain = 1. / std::sqrt(1. + 2.*a*b*correlation);
    return {float(gain*(a*dry[0] + b*wet[0])), float(gain*(a*dry[1] + b*wet[1]))};
}

int SlideVoice::render(float* left, float* right, int count, double scale) {
    if (count <= 0) return 0;
    const double target = std::isfinite(scale) ? std::log2(std::clamp(scale, 1./16., 16.)) : 0.;
    if (clock == 0) playback = playbackTarget;
    if (snapPitch) previousPitch = target;
    snapPitch = false;
    const double increment = (target - previousPitch) / count;
    int written = 0;
    while (written < count) {
        const int64_t start = clock;
        int n = 0;
        for (; n < std::min(quantum, count - written); ++n, ++clock) {
            const bool releasing = releaseAt >= 0;
            const int64_t age = releasing ? clock - releaseAt : 0;
            if (sourceEnd < 0 && (position >= end || (releasing && age >= fadeSamples)))
                sourceEnd = clock;
            if (sourceEnd >= 0 && clock >= sourceEnd + latencySamples) break;
            const double pitch = previousPitch + (written + n + 1) * increment;
            const bool active = sourceEnd < 0 && (slideActive || std::abs(pitch) > 1.e-9);
            mixStep = std::clamp(mixStep + (active ? 1 : -1), 0, fadeSamples);
            const float x = float(mixStep) / fadeSamples;
            auto& f = frame(clock);
            f.dry = sourceEnd < 0 ? read(position) : Stereo{};
            f.wet = {};
            f.pitch = float(pitch);
            f.mix = x*x*(3.f - 2.f*x);
            f.gain = releasing ? std::max(0.f, 1.f - float(age) / fadeSamples) : 1.f;
            playback += playbackFollow * (playbackTarget - playback);
            f.baseFrequency = float(std::clamp(baseFrequency * playback, 0.0001, 0.16));
            if (sourceEnd < 0) {
                position += step * playback;
                if (mixStep > 0) wetUntil = clock + latencySamples;
            }
        }
        if (!n) break;
        process(n, start);
        for (int i = 0; i < n; ++i) {
            Stereo sample{};
            const int64_t t = start + i;
            if (t >= latencySamples) {
                const auto& delayed = frame(t - latencySamples);
                sample = delayed.dry;
                if (delayed.mix > 0.f) {
                    const auto& wet = frame(t - wetPadding).wet;
                    sample = mix(sample, wet, delayed.mix);
                }
                if (delayed.gain != 1.f)
                    for (float& v : sample) v *= delayed.gain;
            }
            left[written+i] = sample[0];
            right[written+i] = sample[1];
        }
        written += n;
    }
    previousPitch = target;
    return written;
}

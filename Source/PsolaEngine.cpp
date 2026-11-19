#include "PsolaEngine.h"
#include <cmath>
#include <cstddef>
#include <numbers>
#include <utility>

PsolaVoice::PsolaVoice(std::shared_ptr<const WaveData> data, int first, int last, double rate)
    : wav(std::move(data)), begin(std::clamp(first, 0, wav->frames())),
      end(std::clamp(last, begin, wav->frames())), step(wav->sampleRate / rate),
      position(begin), fixedLength(0.005 * rate), playbackFollow(-std::expm1(-1. / (0.003 * rate))),
      levelFollow(float(-std::expm1(-1. / (0.025 * rate)))),
      fadeSamples(std::max(1, int(std::lround(0.020 * rate)))) {
    grainStarts.fill(-1);
    for (int i = 0; i < int(grains.size()) - 1; ++i) grains[i].next = i + 1;
    grains.back().next = -1;
    for (int i = 0; i <= tableSize; ++i) {
        const double phase = std::numbers::pi * i / tableSize;
        hann[i] = float(0.5 + 0.5 * std::cos(phase));
        crossfade[i] = {float(std::cos(phase / 2.)), float(std::sin(phase / 2.))};
    }
}

float PsolaVoice::voiceWeight(const VocalFrame& frame) const noexcept {
    const float wPsola = std::clamp((frame.voiced - 0.5f) / 0.3f, 0.f, 1.f);
    return frame.transient ? 0.f : wPsola * frame.transientBlend;
}

PsolaVoice::Stereo PsolaVoice::read(double where) const noexcept {
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

double PsolaVoice::epochCenter(double where, double period, bool half) const noexcept {
    const auto& epochs = wav->epochs;
    const double probe = where - (half ? 0.5 * period : 0.);
    const auto first = std::lower_bound(epochs.begin(), epochs.end(), probe,
        [](const VocalEpoch& epoch, double target) { return epoch.position < target; });
    const auto index = first - epochs.begin();
    const auto count = epochs.end() - epochs.begin();
    double best = where, bestScore = 0.85 * period;
    for (auto i = std::max(std::ptrdiff_t{0}, index - 2);
         i < std::min(count, index + 2); ++i) {
        const auto& epoch = epochs[i];
        if (epoch.position < begin || epoch.position >= end) continue;
        double position = epoch.position;
        if (half) {
            double offset = 0.5 * period;
            if (i + 1 < count && epochs[i + 1].position < end) {
                const double spacing = epochs[i + 1].position - position;
                if (spacing > 0.65 * period && spacing < 1.45 * period) offset = 0.5 * spacing;
            }
            position += offset;
        }
        if (position >= end) continue;
        const double score = std::abs(position - where) + 0.08 * period * (1. - epoch.confidence);
        if (score < bestScore) {
            best = position;
            bestScore = score;
        }
    }
    return best;
}

void PsolaVoice::addGrain(double center, double source, double speed, const VocalFrame& frame,
                          double logRatio) noexcept {
    const double weight = voiceWeight(frame);
    const double period = frame.period > 0.f ? frame.period / speed : fixedLength;
    const double radius = std::clamp(std::lerp(0.5 * fixedLength, period, weight), 2., double(latencySamples - 2));
    const double ratio = std::exp2(logRatio * weight);
    // Alternate an epoch and its half-cycle phase at T_target/2. Reusing
    // the same source phase twice per cycle would double the output pitch.
    const double aligned = weight > 0. ? epochCenter(source, period * speed, halfPhase) : source;
    // Near unity, converge back to the dry phase as the slide returns to its root.
    const double phaseAmount = std::clamp(std::abs(logRatio) * 120., 0., 1.);
    if (freeGrain >= 0) {
        const int id = freeGrain;
        freeGrain = grains[id].next;
        const double outputCenter = center + latencySamples;
        const int64_t start = std::max(clock, int64_t(std::ceil(outputCenter - radius)));
        int& slot = grainStarts[ringIndex(start)];
        grains[id] = {outputCenter, outputCenter + radius, 1. / radius,
                      weight * phaseAmount * (aligned - source), slot};
        slot = id;
    }
    halfPhase = !halfPhase;
    // At 5 ms grain length, use a half-length hop so Hann windows overlap.
    const double hop = std::lerp(0.5 * fixedLength, 0.5 * period / ratio, weight);
    nextGrain = center + std::clamp(hop, 2., radius);
}

void PsolaVoice::activateGrains() noexcept {
    // Only grains touching the current output sample belong to the active list.
    for (int* link = &activeGrain; *link >= 0;) {
        const int id = *link;
        auto& grain = grains[id];
        if (grain.end <= clock) {
            *link = grain.next;
            grain.next = freeGrain;
            freeGrain = id;
        } else {
            link = &grain.next;
        }
    }
    int& slot = grainStarts[ringIndex(clock)];
    while (slot >= 0) {
        const int id = slot;
        slot = grains[id].next;
        grains[id].next = activeGrain;
        activeGrain = id;
    }
}

PsolaVoice::Stereo PsolaVoice::synthesise(const Input& delayed) noexcept {
    Stereo shift{};
    float total = 0.f;
    for (int i = activeGrain; i >= 0; i = grains[i].next) {
        const auto& grain = grains[i];
        const double phase = std::abs(clock - grain.center) * grain.inverseRadius;
        if (phase >= 1.) continue;
        const double point = phase * tableSize;
        const int index = int(point);
        const float window = std::lerp(hann[index], hann[index + 1], float(point - index));
        total += window;
        if (grain.sourceOffset == 0.) continue;
        const double source = delayed.position + grain.sourceOffset;
        const float edge = float(std::clamp(std::min(source - begin, end - source) / 16., 0., 1.));
        const auto wet = read(source);
        for (int ch = 0; ch < 2; ++ch)
            shift[ch] += edge * window * (wet[ch] - delayed.dry[ch]);
    }
    auto output = delayed.dry;
    if (total > 0.f) {
        // The aligned and fixed readings share one Hann weight and one OLA.
        const float factor = delayed.weight / std::max(0.1f, total);
        for (int ch = 0; ch < 2; ++ch) output[ch] += factor * shift[ch];
    }
    const auto power = [](const Stereo& s) {
        const float l = std::clamp(s[0], -8.f, 8.f), r = std::clamp(s[1], -8.f, 8.f);
        return l*l + r*r;
    };
    dryPower += levelFollow * (power(delayed.dry) - dryPower);
    wetPower += levelFollow * (power(output) - wetPower);
    const float targetGain = std::clamp(std::sqrt((dryPower + 1.e-6f) / (wetPower + 1.e-6f)), 0.7f, 2.5f);
    wetGain += levelFollow * (targetGain - wetGain);
    const float level = std::lerp(1.f, wetGain, delayed.weight);
    for (float& sample : output) sample *= level;
    return output;
}

int PsolaVoice::render(float* left, float* right, int count, double scale) noexcept {
    const double targetPitch = std::isfinite(scale) ? std::log2(std::clamp(scale, 1./16., 16.)) : 0.;
    if (clock == 0) playback = playbackTarget;
    if (snapPitch) previousPitch = targetPitch;
    snapPitch = false;
    const double pitchStep = count > 0 ? (targetPitch - previousPitch) / count : 0.;
    int i = 0;
    for (; i < count; ++i, ++clock) {
        const bool releasing = releaseAt >= 0;
        const int64_t releaseAge = releasing ? clock - releaseAt : 0;
        if (sourceEnd < 0 && (position >= end || (releasing && releaseAge >= fadeSamples)))
            sourceEnd = clock;
        if (sourceEnd >= 0 && clock >= sourceEnd + latencySamples) break;

        const double pitch = previousPitch + pitchStep * (i + 1);
        const bool active = sourceEnd < 0 && (slideActive || std::abs(pitch) > 1.e-9);
        mixStep = active ? std::min(fadeSamples, mixStep + 1) : std::max(0, mixStep - 1);
        const float gain = releasing ? std::max(0.f, 1.f - float(releaseAge) / fadeSamples) : 1.f;
        const auto dry = sourceEnd < 0 ? read(position) : Stereo{};
        playback += playbackFollow * (playbackTarget - playback);
        if (std::abs(playbackTarget - playback) < 1.e-9) playback = playbackTarget;
        const double speed = step * playback;

        VocalFrame frame;
        if (mixStep > 0 && sourceEnd < 0) {
            frame = wav->voiceAt(position);
            if (!primed) {
                const double aligned = voiceWeight(frame) > 0.f
                    ? epochCenter(position, frame.period, false) : position;
                nextGrain = clock + std::clamp((aligned - position) / speed,
                                               -0.5 * latencySamples, 0.5 * latencySamples);
                halfPhase = false;
                primed = true;
            }
            while (nextGrain <= clock) {
                const double source = std::max(double(begin), position + (nextGrain - clock) * speed);
                addGrain(nextGrain, source, speed, wav->voiceAt(source), pitch);
            }
        } else {
            primed = false;
        }
        input[ringIndex(clock)] = {dry, position, voiceWeight(frame),
                                   float(mixStep) / fadeSamples, gain};
        if (sourceEnd < 0) position += speed;

        activateGrains();

        Stereo output{};
        if (clock >= latencySamples) {
            const auto& delayed = input[ringIndex(clock - latencySamples)];
            output = delayed.dry;
            // Fixed grains read the same source samples as dry. Their
            // normalized OLA is exactly dry, including on the transient hold.
            if (delayed.mix > 0.f && delayed.weight > 0.f && activeGrain >= 0) {
                const auto wet = synthesise(delayed);
                if (delayed.mix >= 1.f) output = wet;
                else {
                    const auto gains = crossfade[int(delayed.mix * tableSize + 0.5f)];
                    const float normalise = 1.f + (1.f - delayed.weight) * (gains[0] + gains[1] - 1.f);
                    for (int ch = 0; ch < 2; ++ch)
                        output[ch] = (gains[0] * output[ch] + gains[1] * wet[ch]) / normalise;
                }
            }
            if (delayed.mix == 0.f) {
                dryPower = wetPower = 0.f;
                wetGain = 1.f;
            }
            if (delayed.gain != 1.f)
                for (float& sample : output) sample *= delayed.gain;
        }
        left[i] = output[0];
        right[i] = output[1];
    }
    previousPitch = targetPitch;
    return i;
}

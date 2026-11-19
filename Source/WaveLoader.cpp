#include "WaveLoader.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <numbers>

namespace {
// Pitch analysis runs once, while loading the WAV.
constexpr int pitchStates = 8;
constexpr float aperiodicLimit = 0.30f;
struct PitchCandidate {
    float period = 0;
    float cost = 0;
    float aperiodicity = 1;
};
struct PitchPath {
    std::array<PitchCandidate, pitchStates> candidates{};
    std::array<uint8_t, pitchStates> previous{};
    int count = 1; // State zero is unvoiced.
};

float pitchTransition(float before, float after) {
    if (before == 0.f || after == 0.f) return before == after ? 0.f : 0.16f;
    const float octaves = std::abs(std::log2(after / before));
    return 0.18f * octaves + 0.65f * std::max(0.f, octaves - 0.2f);
}

void markTransients(WaveData& wav, const std::vector<float>& signal, int stride) {
    const double rate = double(wav.sampleRate) / stride;
    // Positive multiband energy flux, equivalent to a coarse spectral flux.
    const std::array<double, 3> frequencies{250., 1000., 2700.};
    std::array<double, 3> low{}, coefficient{};
    for (int band = 0; band < 3; ++band)
        coefficient[band] = -std::expm1(-2. * std::numbers::pi * frequencies[band] / rate);
    std::array<double, 4> previous{};
    std::array<double, 8> history{};
    size_t next = 0;
    double fluxSum = 0.;
    int hold = 0;
    for (size_t frame = 0; frame < wav.vocal.size(); ++frame) {
        std::array<double, 4> power{};
        const size_t stop = std::min(signal.size(), (frame + 1) * wav.analysisHop / stride);
        for (; next < stop; ++next) {
            const double value = signal[next];
            for (int band = 0; band < 3; ++band)
                low[band] += coefficient[band] * (value - low[band]);
            const std::array<double, 4> bands{low[0], low[1] - low[0], low[2] - low[1], value - low[2]};
            for (int band = 0; band < 4; ++band) power[band] += bands[band] * bands[band];
        }
        double flux = 0., energy = 0.;
        for (int band = 0; band < 4; ++band) {
            const double level = std::sqrt(power[band]);
            flux += std::max(0., level - previous[band]);
            energy += level;
            previous[band] = level;
        }
        flux /= std::max(0.03, energy);
        const size_t slot = frame % history.size();
        if (flux > std::max(0.34, 2. * fluxSum / history.size()) && energy > 0.03)
            hold = std::max(1, int(std::lround(0.015 * wav.sampleRate / wav.analysisHop)));
        fluxSum += flux - history[slot];
        history[slot] = flux;
        wav.vocal[frame].transient = hold > 0;
        hold = std::max(0, hold - 1);
    }
}

void findEpochs(WaveData& wav, int channel) {
    const auto x = [&](int at) {
        return std::clamp(wav.stereo[2 * size_t(std::clamp(at, 0, wav.frames() - 1)) + channel], -8.f, 8.f);
    };
    const auto excitation = [&](int at) {
        // De-emphasize smooth low frequencies while keeping a consistent polarity.
        const int offset = std::max(1, wav.sampleRate / 8000);
        return x(at) - 0.80f * x(at - offset);
    };
    for (size_t frame = 0; frame < wav.vocal.size();) {
        if (wav.vocal[frame].voiced <= 0.5f) { ++frame; continue; }
        const int start = int(frame) * wav.analysisHop;
        do { ++frame; } while (frame < wav.vocal.size() && wav.vocal[frame].voiced > 0.5f);
        const int end = std::min(wav.frames(), int(frame) * wav.analysisHop);
        double predicted = start, previousEpoch = start - 1.;
        bool seeded = false;
        float polarity = 1.f;
        while (predicted < end) {
            const VocalFrame pitch = wav.voiceAt(predicted);
            if (pitch.period <= 0.f) break;
            const double radius = seeded ? 0.2 * pitch.period : pitch.period;
            const int low = std::max(int(std::floor(previousEpoch)) + 1, int(std::ceil(predicted - radius)));
            const int high = std::min(end - 1, int(std::floor(predicted + radius)));
            if (low > high) break;
            const auto score = [&](int at) {
                const float feature = excitation(at);
                return seeded ? polarity * feature : std::abs(feature);
            };
            int peak = low;
            for (int at = low + 1; at <= high; ++at)
                if (score(at) > score(peak)) peak = at;
            if (!seeded) polarity = excitation(peak) < 0.f ? -1.f : 1.f;
            const double left = polarity * excitation(peak - 1), mid = polarity * excitation(peak);
            const double right = polarity * excitation(peak + 1);
            const double curve = 2. * mid - left - right;
            const double delta = curve > 1.e-9 && peak > low && peak < high
                ? std::clamp(0.5 * (right - left) / curve, -0.5, 0.5) : 0.;
            const double position = peak + delta;
            const double error = seeded ? std::abs(position - predicted) / radius : 0.;
            wav.epochs.push_back({position, pitch.confidence * float(1. - 0.5 * std::min(1., error))});
            previousEpoch = position;
            const float localPeriod = wav.voiceAt(position).period;
            predicted = position + (localPeriod > 0.f ? localPeriod : pitch.period);
            seeded = true;
        }
    }
}

void analyzeVoice(WaveData& wav) {
    const int stride = std::max(1, wav.sampleRate / 8000);
    const double rate = double(wav.sampleRate) / stride;
    double power[2]{};
    for (size_t i = 0; i < wav.stereo.size(); ++i) {
        const double sample = std::clamp(wav.stereo[i], -8.f, 8.f);
        power[i % 2] += sample * sample;
    }
    const int channel = power[1] > power[0];
    std::vector<float> signal;
    signal.reserve((wav.frames() + stride - 1) / stride);
    for (int at = 0; at < wav.frames(); at += stride) {
        const int count = std::min(stride, wav.frames() - at);
        float sum = 0.f;
        for (int i = 0; i < count; ++i)
            sum += std::clamp(wav.stereo[2 * size_t(at + i) + channel], -8.f, 8.f);
        signal.push_back(sum / count);
    }

    wav.analysisHop = std::max(1, int(std::lround(wav.sampleRate * 0.005)));
    const int frames = (wav.frames() + wav.analysisHop - 1) / wav.analysisHop;
    wav.vocal.resize(frames);
    std::vector<PitchPath> path(frames);
    const int minLag = std::max(2, int(rate / 2200.));
    const int maxLag = int(std::ceil(rate / 55.));
    const int width = std::max(maxLag, int(rate * 0.022));
    std::vector<float> difference(maxLag + 2);
    std::array<float, pitchStates> last{}, next{};
    PitchPath empty;
    for (int frame = 0; frame < frames; ++frame) {
        auto& current = path[frame];
        const int mid = int(int64_t(frame) * wav.analysisHop / stride);
        const int start = std::clamp(mid - width / 2 - maxLag / 4, 0,
                                     std::max(0, int(signal.size()) - width - maxLag - 1));
        const int count = std::min(width, int(signal.size()) - start - maxLag - 1);
        if (count >= width / 2) {
            double rms = 0.;
            for (int i = 0; i < count; ++i) rms += signal[start + i] * signal[start + i];
            if (rms > count * 1.e-8) {
                double sum = 0.;
                difference[0] = 1.f;
                for (int lag = 1; lag <= maxLag + 1; ++lag) {
                    double delta = 0.;
                    for (int i = 0; i < count; ++i) {
                        const double d = signal[start + i] - signal[start + i + lag];
                        delta += d * d;
                    }
                    sum += delta;
                    difference[lag] = sum > 1.e-12 ? float(delta * lag / sum) : 1.f;
                }
                int firstValley = 0;
                for (int lag = minLag; lag <= maxLag; ++lag) {
                    const float a = difference[lag - 1], b = difference[lag], c = difference[lag + 1];
                    if (b >= aperiodicLimit || b >= a || b > c) continue;
                    const float fraction = std::clamp(0.5f * (a - c) / std::max(1.e-8f, a + c - 2.f*b), -0.5f, 0.5f);
                    const float period = (lag + fraction) * stride;
                    if (!firstValley) firstValley = lag;
                    const float cost = b + 0.055f * std::log2(float(lag) / firstValley);
                    PitchCandidate candidate{period, cost, b};
                    if (current.count < pitchStates) current.candidates[current.count++] = candidate;
                    else {
                        auto worst = std::max_element(current.candidates.begin() + 1, current.candidates.end(),
                                                      [](const auto& l, const auto& r) { return l.cost < r.cost; });
                        if (cost < worst->cost) *worst = candidate;
                    }
                }
                current.candidates[0].cost = current.count > 1 ? 0.33f : 0.f;
            }
        }
        const auto& prior = frame > 0 ? path[frame - 1] : empty;
        for (int j = 0; j < current.count; ++j) {
            float best = std::numeric_limits<float>::infinity();
            for (int i = 0; i < prior.count; ++i) {
                const float cost = last[i] + pitchTransition(prior.candidates[i].period, current.candidates[j].period);
                if (cost < best) {
                    best = cost;
                    current.previous[j] = uint8_t(i);
                }
            }
            next[j] = best + current.candidates[j].cost;
        }
        const float base = *std::min_element(next.begin(), next.begin() + current.count);
        for (int j = 0; j < current.count; ++j) last[j] = next[j] - base;
    }
    int selected = int(std::min_element(last.begin(), last.begin() + path.back().count) - last.begin());
    for (int frame = frames - 1; frame >= 0; --frame) {
        const auto& candidate = path[frame].candidates[selected];
        if (candidate.period > 0.f) {
            const float voiced = std::clamp(1.f - candidate.aperiodicity / aperiodicLimit, 0.f, 1.f);
            wav.vocal[frame] = {candidate.period, voiced, 1.f - candidate.aperiodicity, false};
        }
        selected = path[frame].previous[selected];
    }
    markTransients(wav, signal, stride);
    findEpochs(wav, channel);
}
uint16_t u16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t u32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
                                        (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }
float sampleAt(const uint8_t* p, int bits, int format) {
    if (format == 3 && bits == 32) {
        float result = 0.0f;
        std::memcpy(&result, p, 4);
        return std::isfinite(result) ? result : 0.0f;
    }
    if (format != 1) return 0;
    if (bits == 8) return (float(p[0]) - 128.f) / 128.f;
    if (bits == 16) return float(int16_t(u16(p))) / 32768.f;
    if (bits == 24) {
        int32_t n = int32_t(p[0]) | (int32_t(p[1]) << 8) | (int32_t(p[2]) << 16);
        if (n & 0x800000) n |= ~0xffffff;
        return float(n) / 8388608.f;
    }
    if (bits == 32) { int32_t n = int32_t(u32(p)); return double(n) / 2147483648.0; }
    return 0;
}
}

VocalFrame WaveData::voiceAt(double position) const noexcept {
    if (vocal.empty()) return {};
    const double frame = std::max(0., position / analysisHop);
    const size_t lo = std::min(size_t(frame), vocal.size() - 1);
    const auto& a = vocal[lo];
    const auto& b = vocal[std::min(lo + 1, vocal.size() - 1)];
    const float t = float(std::clamp(frame - lo, 0., 1.));
    float period = a.period > 0.f ? a.period : b.period;
    if (a.period > 0.f && b.period > 0.f)
        period = std::lerp(a.period, b.period, t);
    // Taper into and out of the hard transient hold. Analysis is offline,
    // so we can begin the taper before the consonant.
    float gate = 1.f;
    if (a.transient) gate = 0.f;
    else {
        if (lo > 0 && vocal[lo - 1].transient) gate = std::min(gate, t);
        if (b.transient) gate = std::min(gate, 1.f - t);
        gate *= gate * (3.f - 2.f * gate);
    }
    return {period, std::lerp(a.voiced, b.voiced, t),
            std::lerp(a.confidence, b.confidence, t), a.transient, gate};
}

bool readWav(const std::wstring& path, WaveData& result, std::string& error) {
    std::ifstream input(std::filesystem::path(path), std::ios::binary);
    if (!input) { error = "Cannot open WAV"; return false; }
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)), {});
    if (bytes.size() < 44 || bytes.size() > size_t(800) * 1024 * 1024 ||
        std::memcmp(bytes.data(), "RIFF", 4) || std::memcmp(bytes.data() + 8, "WAVE", 4)) {
        error = "Not a RIFF/WAVE file"; return false;
    }
    size_t pos = 12, dataStart = 0, dataSize = 0;
    int format = 0, channels = 0, sampleRate = 0, bits = 0, blockAlign = 0;
    std::vector<uint32_t> cues;
    while (pos + 8 <= bytes.size()) {
        const uint32_t chunkSize = u32(bytes.data() + pos + 4);
        const size_t begin = pos + 8;
        if (chunkSize > bytes.size() - begin) { error = "Broken WAV chunk"; return false; }
        if (!std::memcmp(bytes.data() + pos, "fmt ", 4) && chunkSize >= 16) {
            const uint8_t* p = bytes.data() + begin;
            format = u16(p); channels = u16(p + 2); sampleRate = int(u32(p + 4));
            blockAlign = u16(p + 12); bits = u16(p + 14);
            if (format == 0xfffe && chunkSize >= 40) format = u16(p + 24);
        }
        if (!std::memcmp(bytes.data() + pos, "data", 4)) { dataStart = begin; dataSize = chunkSize; }
        if (!std::memcmp(bytes.data() + pos, "cue ", 4) && chunkSize >= 4) {
            const auto* chunk = bytes.data() + begin;
            const auto count = u32(chunk);
            if (count <= (chunkSize - 4) / 24) {
                for (uint32_t i = 0; i < count; ++i) {
                    const auto* cue = chunk + 4 + size_t(i) * 24;
                    if (!std::memcmp(cue + 8, "data", 4))
                        cues.push_back(u32(cue + 20));
                }
            }
        }
        pos = begin + chunkSize + (chunkSize & 1);
    }
    if ((format != 1 && format != 3) || channels < 1 || channels > 8 ||
        sampleRate < 8000 || sampleRate > 384000 ||
        (bits != 8 && bits != 16 && bits != 24 && bits != 32) ||
        (format == 3 && bits != 32) || !blockAlign || !dataSize ||
        blockAlign < channels * (bits / 8)) {
        error = "Unsupported WAV (use PCM 16/24/32 or 32-bit float)"; return false;
    }
    const size_t frames = dataSize / size_t(blockAlign);
    if (frames < 2 || frames > size_t(sampleRate) * 600) {
        error = "WAV must be between 2 frames and 10 minutes"; return false;
    }
    WaveData tmp;
    for (const auto offset : cues)
        if (offset < frames - 1) tmp.cueStarts.push_back(int(offset));
    std::sort(tmp.cueStarts.begin(), tmp.cueStarts.end());
    tmp.cueStarts.erase(std::unique(tmp.cueStarts.begin(), tmp.cueStarts.end()), tmp.cueStarts.end());
    tmp.sampleRate = sampleRate;
    tmp.stereo.resize(frames * 2);
    for (size_t i = 0; i < frames; ++i) {
        const uint8_t* p = bytes.data() + dataStart + i * size_t(blockAlign);
        const float l = sampleAt(p, bits, format);
        const float r = channels > 1 ? sampleAt(p + bits / 8, bits, format) : l;
        tmp.stereo[i * 2] = l;
        tmp.stereo[i * 2 + 1] = r;
    }
    analyzeVoice(tmp);
    result = std::move(tmp);
    error.clear();
    return true;
}

std::vector<AudioSlice> sliceWav(const WaveData& wav, bool equal, int divisions) {
    std::vector<AudioSlice> slices;
    const int frames = wav.frames();
    if (frames < 2) return slices;
    if (equal) {
        const int n = std::clamp(divisions, 1, std::min(64, frames / 2));
        for (int i = 0; i < n; ++i)
            slices.push_back({int(int64_t(i) * frames / n), int(int64_t(i + 1) * frames / n)});
        return slices;
    }
    if (!wav.cueStarts.empty()) {
        for (size_t i = 0; i < wav.cueStarts.size(); ++i) {
            const int end = i + 1 < wav.cueStarts.size() ? wav.cueStarts[i + 1] : frames;
            if (end - wav.cueStarts[i] >= 2) slices.push_back({wav.cueStarts[i], end});
        }
        return slices;
    }
    float maxPeak = 0;
    for (float v : wav.stereo) maxPeak = std::max(maxPeak, std::abs(v));
    const float threshold = std::max(0.0007f, maxPeak * 0.017f);
    const int window = std::max(1, int(wav.sampleRate * 0.003));
    const int gap = std::max(1, int(wav.sampleRate * 0.028));
    const int minSlice = std::max(2, int(wav.sampleRate * 0.042));
    const int pad = std::max(0, int(wav.sampleRate * 0.003));
    int start = -1, last = 0;
    for (int at = 0; at < frames; at += window) {
        const int end = std::min(frames, at + window);
        float peak = 0;
        for (int j = at; j < end; ++j) {
            peak = std::max(peak, std::abs(wav.stereo[size_t(j) * 2]));
            peak = std::max(peak, std::abs(wav.stereo[size_t(j) * 2 + 1]));
        }
        if (peak >= threshold) {
            if (start < 0) start = at;
            last = end;
        } else if (start >= 0 && at - last >= gap) {
            if (last - start >= minSlice)
                slices.push_back({std::max(0, start - pad), std::min(frames, last + pad)});
            start = -1;
        }
    }
    if (start >= 0 && last - start >= minSlice)
        slices.push_back({std::max(0, start - pad), std::min(frames, last + pad)});
    if (slices.empty()) slices.push_back({0, frames});
    // A chromatic WAV may have no silent gaps at all. Never silently map
    // an entire multi-note chromatic onto a single slice: use configurable
    // equal divisions as an initial guess (drag the boundaries to correct).
    // There is no duration threshold: short chromatics need this too.
    if (slices.size() == 1 && divisions > 1 && frames >= divisions * 2) {
        return sliceWav(wav, true, divisions);
    }
    if (slices.size() > 64) slices.resize(64);
    return slices;
}

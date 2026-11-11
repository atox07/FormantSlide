#include "WaveLoader.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>

namespace {
uint16_t u16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t u32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
                                        (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }
float sampleAt(const uint8_t* p, int bits, int format) {
    if (format == 3 && bits == 32) {
        float result = 0.0f;
        std::memcpy(&result, p, 4);
        return std::isfinite(result) ? std::clamp(result, -8.0f, 8.0f) : 0.0f;
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
    tmp.sampleRate = sampleRate;
    tmp.stereo.resize(frames * 2);
    for (size_t i = 0; i < frames; ++i) {
        const uint8_t* p = bytes.data() + dataStart + i * size_t(blockAlign);
        const float l = sampleAt(p, bits, format);
        const float r = channels > 1 ? sampleAt(p + bits / 8, bits, format) : l;
        tmp.stereo[i * 2] = l;
        tmp.stereo[i * 2 + 1] = r;
    }
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

// Estimate the actual recorded pitch of a slice. Unlike the v0.5 'first
// pressed key' anchor, this depends only on WAV content. This runs ONLY
// during WAV loading / slice editing, never in the audio render callback.
PitchEstimate estimateSlicePitch(const WaveData& wav, AudioSlice slice) {
    PitchEstimate invalid;
    if (wav.sampleRate < 8000 || wav.frames() < 2) return invalid;
    slice.begin = std::clamp(slice.begin, 0, wav.frames());
    slice.end = std::clamp(slice.end, slice.begin, wav.frames());
    const int originalFrames = slice.end - slice.begin;
    if (originalFrames < int(0.040 * wav.sampleRate)) return invalid;

    // Decimate to ~12 kHz so YIN's lag calculations are fast even for 192k WAVs.
    const int stride = std::max(1, int(std::lround(wav.sampleRate / 12000.0)));
    const double rate = double(wav.sampleRate) / stride;
    const int minLag = std::max(2, int(std::floor(rate / 1550.0)));
    const int maxLag = std::min(240, int(std::ceil(rate / 75.0)));
    const int n = std::min(int(0.075 * rate), originalFrames / stride);
    if (n < maxLag * 2 || n < 350) return invalid;

    std::vector<double> estimates;
    std::vector<double> strengths;
    const double startFractions[] = {0.10, 0.29, 0.48, 0.66};
    for (double fraction : startFractions) {
        const int fullWindowFrames = n * stride;
        const int base = slice.begin + int((originalFrames - fullWindowFrames) * fraction);
        std::vector<float> signal(size_t(n), 0.f);
        double mean = 0, energyL = 0, energyR = 0;
        for (int i = 0; i < n; ++i) {
            const size_t at = size_t(base + i * stride) * 2;
            energyL += double(wav.stereo[at]) * wav.stereo[at];
            energyR += double(wav.stereo[at + 1]) * wav.stereo[at + 1];
        }
        const size_t channel = energyL >= energyR ? 0u : 1u;
        for (int i = 0; i < n; ++i) {
            signal[size_t(i)] = wav.stereo[size_t(base + i * stride) * 2 + channel];
            mean += signal[size_t(i)];
        }
        if (std::max(energyL, energyR) / n < 0.000001) continue;
        mean /= n;
        // DC removal + first order high pass to lessen slow envelope influence.
        float previous = 0, hp = 0;
        for (float& v : signal) {
            const float raw = v - float(mean);
            hp = raw - previous + 0.985f * hp;
            previous = raw;
            v = hp;
        }
        std::vector<double> difference(size_t(maxLag + 1), 0.0);
        double cumulative = 0.0;
        for (int lag = 1; lag <= maxLag; ++lag) {
            double delta = 0.0;
            for (int i = 0; i < n - maxLag; ++i) {
                const double d = double(signal[size_t(i)]) - signal[size_t(i + lag)];
                delta += d * d;
            }
            difference[size_t(lag)] = delta;
        }
        int best = -1;
        double bestVal = 1.0;
        for (int lag = 1; lag <= maxLag; ++lag) {
            cumulative += difference[size_t(lag)];
            if (lag < minLag || cumulative <= 1e-15) continue;
            const double val = difference[size_t(lag)] * lag / cumulative;
            if (val < 0.20) {
                int candidate = lag;
                double candVal = val;
                // First sufficiently deep local minimum is the fundamental period.
                while (candidate < maxLag) {
                    const int next = candidate + 1;
                    // For the next comparison the cumulative mean difference needs
                    // its own prefix sum; approximate by raw local valley search.
                    if (difference[size_t(next)] >= difference[size_t(candidate)]) break;
                    candidate = next;
                }
                best = candidate;
                bestVal = candVal;
                break;
            }
            if (val < bestVal) { bestVal = val; best = lag; }
        }
        if (best < minLag || bestVal >= 0.29) continue;
        double lag = best;
        if (best > minLag && best < maxLag) {
            const double l = difference[size_t(best-1)];
            const double c = difference[size_t(best)];
            const double r = difference[size_t(best+1)];
            const double denom = l - 2*c + r;
            if (std::abs(denom) > 1e-10) lag += std::clamp(0.5 * (l-r)/denom, -0.5, 0.5);
        }
        const double hz = rate / lag;
        if (hz < 70.0 || hz > 1600.0) continue;
        const double midi = 69.0 + 12.0 * std::log2(hz / 440.0);
        if (std::isfinite(midi)) { estimates.push_back(midi); strengths.push_back(1.0-bestVal); }
    }
    if (estimates.empty()) return invalid;
    // Choose the most consistent semitone cluster instead of an octave error
    // from a single noisy frame.
    size_t best = 0; int votes = -1; double score = -1;
    for (size_t i = 0; i < estimates.size(); ++i) {
        int count = 0; double strength = 0;
        for (size_t j = 0; j < estimates.size(); ++j) {
            if (std::abs(estimates[i] - estimates[j]) <= 0.7) { ++count; strength += strengths[j]; }
        }
        if (count > votes || (count == votes && strength > score)) {
            votes = count; score = strength; best = i;
        }
    }
    double sum = 0, weight = 0;
    for (size_t i = 0; i < estimates.size(); ++i) {
        if (std::abs(estimates[i] - estimates[best]) <= 0.7) {
            sum += estimates[i] * strengths[i]; weight += strengths[i];
        }
    }
    if (weight <= 0) return invalid;
    const float confidence = float(score / votes * (votes >= 2 ? 1.0 : 0.82));
    if (confidence < 0.65f) return invalid;
    return {sum / weight, confidence};
}

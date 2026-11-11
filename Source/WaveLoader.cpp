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
    // Continuous chromatic WAVs often have no silence between phonemes.
    // Offer immediately usable default divisions for long files; these are
    // initial estimates that can now be dragged individually in the editor.
    if (slices.size() == 1 && frames >= wav.sampleRate * 2 && divisions > 1) {
        return sliceWav(wav, true, divisions);
    }
    if (slices.size() > 64) slices.resize(64);
    return slices;
}

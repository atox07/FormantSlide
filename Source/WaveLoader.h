#pragma once
#include <array>
#include <string>
#include <vector>

constexpr int maxVocalOrder = 96;
using VocalFilter = std::array<float,maxVocalOrder>;
struct VocalFrame {
    float period = 0, voiced = 0;
};

struct WaveData {
    int analysisHop = 1;
    std::vector<VocalFrame> vocal;
    std::vector<double> pulses; // fractional positions in source samples
    int filterOrder = 0;
    std::vector<VocalFilter> filters;
    std::vector<float> excitation; // stereo, on the original WAV sample grid
    std::vector<int> cueStarts;
    std::vector<float> stereo; // interleaved stereo
    int sampleRate = 44100;
    int frames() const { return static_cast<int>(stereo.size() / 2); }
};
struct AudioSlice { int begin = 0; int end = 0; };

bool readWav(const std::wstring& path, WaveData& out, std::string& error);
std::vector<AudioSlice> sliceWav(const WaveData& wav, bool equal, int divisions);

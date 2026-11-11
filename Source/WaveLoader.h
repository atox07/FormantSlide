#pragma once
#include <string>
#include <vector>

struct WaveData {
    std::vector<int> cueStarts;
    std::vector<float> stereo; // interleaved stereo
    int sampleRate = 44100;
    int frames() const { return static_cast<int>(stereo.size() / 2); }
};
struct AudioSlice { int begin = 0; int end = 0; };

bool readWav(const std::wstring& path, WaveData& out, std::string& error);
std::vector<AudioSlice> sliceWav(const WaveData& wav, bool equal, int divisions);

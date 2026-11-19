#pragma once
#include <string>
#include <vector>

struct VocalFrame {
    float period = 0, voiced = 0, confidence = 0;
    bool transient = false;
    float transientBlend = 1.f;
};
struct VocalEpoch { double position; float confidence; };

struct WaveData {
    int analysisHop = 1;
    std::vector<VocalFrame> vocal;
    std::vector<VocalEpoch> epochs;
    std::vector<int> cueStarts;
    std::vector<float> stereo; // interleaved stereo
    int sampleRate = 44100;
    VocalFrame voiceAt(double position) const noexcept;
    int frames() const { return static_cast<int>(stereo.size() / 2); }
};
struct AudioSlice { int begin = 0; int end = 0; };

bool readWav(const std::wstring& path, WaveData& out, std::string& error);
std::vector<AudioSlice> sliceWav(const WaveData& wav, bool equal, int divisions);

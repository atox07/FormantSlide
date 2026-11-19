#pragma once
#include <array>
#include <string>
#include <vector>

struct VocalFrame {
    float period = 0, voiced = 0;
};

struct WaveData {
    using Envelope = std::array<float,513>;
    std::vector<Envelope> envelopes;
    std::vector<int> attacks;
    int envelopeHop = 1;
    void prepareSlide();
    int analysisHop = 1;
    std::vector<VocalFrame> vocal;
    std::vector<int> cueStarts;
    std::vector<float> stereo; // interleaved stereo
    int sampleRate = 44100;
    double slicePitchHz(int begin, int end, double fallback) const noexcept;
    int frames() const { return static_cast<int>(stereo.size() / 2); }
};
struct AudioSlice { int begin = 0; int end = 0; };

bool readWav(const std::wstring& path, WaveData& out, std::string& error);
std::vector<AudioSlice> sliceWav(const WaveData& wav, bool equal, int divisions);

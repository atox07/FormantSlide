#pragma once
#include "WaveLoader.h"
#include <array>
#include <memory>
#include <algorithm>
#include <complex>
#include <vector>
#include <StftPitchShift/StftPitchShiftCore.h>

class StftVoice {
public:
    StftVoice(std::shared_ptr<const WaveData>, int begin, int end, double hostRate);
    int render(float* left, float* right, int count, double scale);
private:
    static constexpr int N = 1024, H = 64;
    float read(double position, int channel) const;
    void frame(long long start, double scale);
    std::shared_ptr<const WaveData> wav;
    int begin, end, cursor = 0;
    double step;
    long long position = 0, length;
    bool active = false;
    std::array<stftpitchshift::RFFT, 2> fft;
    std::array<std::unique_ptr<stftpitchshift::StftPitchShiftCore<float>>, 2> core;
    std::array<float,N> window{}, time{};
    std::array<std::complex<float>,N/2+1> bins{};
    std::array<std::array<float,N>,2> overlap{};
    std::vector<double> factor{1.0};
};

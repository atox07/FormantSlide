#pragma once
#include "WaveLoader.h"
#include <algorithm>

// Both branches operate on this lattice's prediction error. Use the same
// coefficients at the same source time for analysis and inverse synthesis.
struct VocalLattice {
    std::array<double,maxVocalOrder+1> delay{};

    double analyse(double sample, const VocalFilter& k, int order) {
        double previous = delay[0];
        delay[0] = sample;
        for (int j=0; j<order; ++j) {
            const double next = delay[j+1];
            delay[j+1] = previous + k[j]*sample;
            sample += k[j]*previous;
            previous = next;
        }
        return sample;
    }
    double synthesise(double error, const VocalFilter& k, int order) {
        for (int j=order-1; j>=0; --j) {
            error -= k[j]*delay[j];
            delay[j+1] = delay[j]+k[j]*error;
        }
        delay[0] = error;
        return error;
    }
};

inline VocalFilter filterAt(const WaveData& wav, int sample) {
    const size_t frame = size_t(sample/wav.analysisHop);
    const auto& a = wav.filters[frame];
    const auto& b = wav.filters[std::min(frame+1,wav.filters.size()-1)];
    const float t = float(sample%wav.analysisHop)/wav.analysisHop;
    VocalFilter result{};
    for (int j=0; j<wav.filterOrder; ++j) result[j] = a[j]+t*(b[j]-a[j]);
    return result;
}

#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <numbers>

namespace vocal {
template<int N> class Fourier {
    std::array<int,N> reversed{};
    std::array<std::complex<float>,N/2> roots{};
    Fourier() {
        for (int i=1; i<N; ++i) reversed[i]=(reversed[i/2]>>1) | ((i&1)*(N/2));
        for (int i=0; i<N/2; ++i) roots[i]=std::polar(1.f,float(-2.*std::numbers::pi*i/N));
    }
public:
    static const Fourier& plan() { static const Fourier result; return result; }
    void transform(std::array<std::complex<float>,N>& data, bool inverse=false) const {
        if (inverse) for (auto& z : data) z=std::conj(z);
        for (int i=0; i<N; ++i) if (i<reversed[i]) std::swap(data[i],data[reversed[i]]);
        for (int span=2; span<=N; span*=2) {
            for (int base=0; base<N; base+=span) {
                for (int j=0; j<span/2; ++j) {
                    const auto a=data[base+j], b=data[base+j+span/2]*roots[j*(N/span)];
                    data[base+j]=a+b;
                    data[base+j+span/2]=a-b;
                }
            }
        }
        if (inverse) for (auto& z : data) z=std::conj(z)*(1.f/N);
    }
};
}

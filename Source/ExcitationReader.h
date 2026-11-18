#pragma once
#include "WaveLoader.h"
#include <algorithm>
#include <cmath>
#include <complex>

// Polyphase low-pass interpolation for time-domain transposition. Adjacent
// cutoff tables interpolate as the slide moves through an octave.
class ExcitationReader {
    static constexpr int taps=32, phases=128, bands=25;
    using Kernel=std::array<float,taps>;
    using PhaseBank=std::array<Kernel,phases>;
    struct Tables {
        std::array<PhaseBank,bands> bank;
        Tables() {
            constexpr double pi=3.14159265358979323846;
            for (int b=0;b<bands;++b) {
                const double speed=std::ldexp(1.+(b%4)*0.25,b/4);
                for (int p=0;p<phases;++p) {
                    auto& kernel=bank[b][p];
                    double sum=0.;
                    for (int j=0;j<taps;++j) {
                        const double x=j-(taps/2-1)-double(p)/phases;
                        const double w=0.42+0.5*std::cos(2*pi*x/taps)+0.08*std::cos(4*pi*x/taps);
                        const double sinc=x==0 ? 1./speed : std::sin(pi*x/speed)/(pi*x);
                        kernel[j]=float(w*sinc);
                        sum+=kernel[j];
                    }
                    for (auto& k:kernel) k=float(k/sum);
                }
            }
        }
    };
    static const Tables& tables() { static const Tables data; return data; }
    int band=0;
    float blend=0.f;

public:
    static void prepare() { (void)tables(); }
    void setSpeed(double ratio) {
        int exponent=0;
        const double fraction=std::frexp(std::clamp(ratio,1.,64.),&exponent)*2.-1.;
        const double index=4*(exponent-1+fraction);
        band=std::min(bands-2,int(index));
        blend=float(index-band);
    }
    std::complex<float> read(const WaveData& wav,int first,int end,double at) const {
        if (at<first || at>=end) return {};
        const int rounded=int(std::floor((at-int(at))*phases+0.5));
        const int start=int(at)+(rounded/phases)-(taps/2-1);
        const auto& a=tables().bank[band][rounded%phases];
        const auto& b=tables().bank[band+1][rounded%phases];
        const int lo=std::max(0,first-start), hi=std::min(taps,end-start);
        float left=0.f, right=0.f;
        for (int j=lo;j<hi;++j) {
            const float weight=a[j]+blend*(b[j]-a[j]);
            const size_t i=2*size_t(start+j);
            left+=weight*wav.excitation[i];
            right+=weight*wav.excitation[i+1];
        }
        return {left,right};
    }
};

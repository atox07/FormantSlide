#include "ResidualSpectrum.h"
#include <algorithm>
#include <cmath>

namespace {
constexpr double pi = 3.14159265358979323846;
template<size_t N> const std::array<float,N>& hann() {
    static const auto table = [] {
        std::array<float,N> w{};
        for (size_t j=0;j<N;++j) w[j]=float(0.5-0.5*std::cos(2*pi*j/N));
        return w;
    }();
    return table;
}
}

ResidualSpectrum::Plans::Plans() {
    (void)hann<size>();
    (void)hann<shortSize>();
}

ResidualSpectrum::Stereo ResidualSpectrum::next(const WaveData& wav, int first, int end, int at, double ratio) {
    if (!active) {
        ring.fill({});
        windowPower.fill(0.f);
        cursor = 0;
        // Start at the centre of a window. Normalising the actual overlap
        // lets several simultaneous voices start without eight FFTs each.
        frame(wav,first,end,at,-size/2,ratio,0);
        frame(wav,first,end,at,0,ratio,size/2);
        active = true;
    } else if (cursor%hop==0) frame(wav,first,end,at,0,ratio,hop);
    const Stereo result = ring[cursor]/std::max(windowPower[cursor],1e-6f);
    ring[cursor] = {};
    windowPower[cursor] = 0.f;
    cursor = (cursor+1)%size;
    return result;
}

void ResidualSpectrum::frame(const WaveData& wav, int first, int end, int at, int offset,
                             double ratio, int advance) {
    const bool initialise=advance==0;
    auto signal = [&](int j) -> Stereo {
        if (j<first || j>=end) return {};
        return {wav.excitation[2*size_t(j)],wav.excitation[2*size_t(j)+1]};
    };
    const auto& longWindow = hann<size>();
    const auto& shortWindow = hann<shortSize>();
    for (int j=0;j<shortSize;++j)
        input[j]=shortWindow[j]*signal(at+offset+(size-shortSize)/2+j);
    plans.shortWindow.perform(input.data(),shortBins.data(),false);
    for (int k=0;k<=shortSize/2;++k) {
        const int mirror=(shortSize-k)%shortSize;
        shortEnergy[k+1]=shortEnergy[k]+0.5*(std::norm(shortBins[k])+std::norm(shortBins[mirror]));
    }
    // Recover the input attack phase only in bands with a strong onset.
    // A stationary noisy band keeps its continuous phase trajectory.
    for (int k=0;k<=shortSize/2;++k) {
        const double energy=shortEnergy[std::min(shortSize/2+1,k+4)]-shortEnergy[std::max(0,k-3)];
        const double previous=previousBandPower[k];
        attack[k]=initialise ? 0.f : float(std::clamp((energy-4.*previous)/(energy+1e-6),0.,1.));
        previousBandPower[k]=initialise ? energy : std::max(energy,0.75*previous);
    }
    for (int j=0;j<size;++j) input[j]=longWindow[j]*signal(at+offset+j);
    plans.full.perform(input.data(),bins.data(),false);
    const double phaseStep=2*pi*std::max(1,advance)/size;
    for (int k=0;k<=half;++k) {
        const auto mirror=std::conj(bins[(size-k)%size]);
        left[k]=0.5f*(bins[k]+mirror);
        right[k]=Stereo(0,-0.5f)*(bins[k]-mirror);
        power[k]=std::norm(left[k])+std::norm(right[k]);
        sourceEnergy[k+1]=sourceEnergy[k]+power[k];
        const double l=std::arg(left[k]), r=std::arg(right[k]);
        const double delta=std::norm(left[k])>=std::norm(right[k]) ? l-previousL[k] : r-previousR[k];
        frequency[k]=initialise ? k : k+std::remainder(delta-phaseStep*k,2*pi)/phaseStep;
        previousL[k]=l;
        previousR[k]=r;
    }
    int count=0;
    for (int k=1;k<half;++k) {
        if (power[k]<=power[k-1] || power[k]<power[k+1] || power[k]<1e-16f) continue;
        peaks[count++]=k;
        int match=k;
        double best=1.35;
        if (!initialise) {
            for (int j=std::max(1,k-2);j<=std::min(half-1,k+2);++j) {
                if (previousPeak[j]<0) continue;
                const double distance=std::abs(frequency[k]-previousPeak[j]);
                if (distance<best) { best=distance; match=j; }
            }
        }
        const double old = initialise ? 0. : previousRotation[match];
        rotation[k]=std::remainder(old+(initialise ? 0. : phaseStep*(ratio-1.)*frequency[k]),2*pi);
        rotation[k]*=1.-attack[k/4];
    }
    shiftedL.fill({}); shiftedR.fill({});
    shiftedL[0]=left[0]; shiftedR[0]=right[0];
    int nearest=0, lastAnchor=-1;
    Stereo phaseShift{};
    for (int k=1;k<half;++k) {
        while (nearest+1<count && 2*k>peaks[nearest]+peaks[nearest+1]) ++nearest;
        const int peak=count ? peaks[nearest] : k;
        const int anchor=std::abs(peak-k)<=3 ? peak : k;
        if (k!=anchor) {
            // Save the phase that was actually used if this bin becomes a peak next hop.
            rotation[k]=rotation[anchor];
        } else if (!count || k!=peak) {
            rotation[k]=initialise ? 0. : std::remainder(previousRotation[k]+phaseStep*(ratio-1.)*frequency[k],2*pi);
            rotation[k]*=1.-attack[k/4];
        }
        const double destination=k+(ratio-1.)*std::clamp(frequency[anchor],0.,double(half));
        if (destination<1 || destination>=half) continue;
        const int lo=int(destination);
        const float frac=float(destination-lo);
        // Work relative to the window centre, including when a peak crosses a bin.
        if (anchor!=lastAnchor) {
            phaseShift=std::polar(1.f,float(rotation[anchor]));
            lastAnchor=anchor;
        }
        const auto phasor=((lo^k)&1) ? -phaseShift : phaseShift;
        const auto l=left[k]*phasor, r=right[k]*phasor;
        shiftedL[lo]+=(1-frac)*l; shiftedR[lo]+=(1-frac)*r;
        shiftedL[lo+1]-=frac*l; shiftedR[lo+1]-=frac*r;
    }
    previousPeak.fill(-1);
    for (int i=0;i<count;++i) previousPeak[peaks[i]]=frequency[peaks[i]];
    previousRotation=rotation;
    for (int k=0;k<=half;++k)
        shiftedEnergy[k+1]=shiftedEnergy[k]+std::norm(shiftedL[k])+std::norm(shiftedR[k]);
    const int width=std::max(6,int(280.*size/wav.sampleRate));
    for (int k=0;k<=half;++k) {
        const int lo=std::max(0,k-width), hi=std::min(half+1,k+width+1);
        const int slo=lo/4, shi=std::min(shortSize/2+1,(hi+3)/4);
        const double longPower=sourceEnergy[hi]-sourceEnergy[lo];
        const double localPower=(shortEnergy[shi]-shortEnergy[slo])*4.*(hi-lo)/(shi-slo);
        const double target=0.7*longPower+0.3*localPower;
        const double air=std::clamp((double(k)*wav.sampleRate/size-3200.)/3200.,0.,1.);
        const float gain=float(std::clamp(std::sqrt((target+1e-12)/(shiftedEnergy[hi]-shiftedEnergy[lo]+1e-12)),0.35,1.8-0.65*air));
        const auto l=gain*shiftedL[k], r=gain*shiftedR[k];
        if (k==0 || k==half) {
            bins[k]={l.real(),r.real()};
        } else {
            bins[k]=l+Stereo(0,1)*r;
            bins[size-k]=std::conj(l)+Stereo(0,1)*std::conj(r);
        }
    }
    plans.full.perform(bins.data(),input.data(),true);
    for (int j=std::max(0,-offset);j<size;++j) {
        const int index=(cursor+offset+j)&(size-1);
        ring[index]+=input[j]*longWindow[j];
        windowPower[index]+=longWindow[j]*longWindow[j];
    }
}

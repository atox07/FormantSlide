#include "SlideVoice.h"
#include <limits>
#include <utility>

namespace {
// Band-limited reads for the transposed waveform. The table is shared by voices.
struct KernelBank {
    using Kernel = std::array<float,16>;
    std::array<std::array<Kernel,64>,61> table{};
    KernelBank() {
        for (int band=0; band<int(table.size()); ++band) {
            const double cutoff=1./(1.+.25*band);
            for (int phase=0; phase<64; ++phase) {
                auto& taps=table[band][phase];
                double sum=0.;
                for (int i=0; i<16; ++i) {
                    const double x=i-7-phase/64.;
                    const double a=std::numbers::pi*x;
                    const double win=.42+.5*std::cos(a/8)+.08*std::cos(a/4);
                    taps[i]=float(win*(x==0. ? cutoff : std::sin(a*cutoff)/a));
                    sum+=taps[i];
                }
                for (auto& v:taps) v=float(v/sum);
            }
        }
    }
};
const KernelBank& kernels() { static const KernelBank bank; return bank; }
float envelopeAt(const WaveData::Envelope& env, double bin) {
    bin=std::clamp(bin,0.,double(env.size()-1));
    const int k=int(bin);
    return std::lerp(env[k],env[std::min(k+1,int(env.size()-1))],float(bin-k));
}
}

SlideVoice::SlideVoice(std::shared_ptr<const WaveData> data, int first, int last,
                       double rate, double nominalHz)
    : wav(std::move(data)), begin(std::clamp(first,0,wav->frames())),
      end(std::clamp(last,begin,wav->frames())), hop(std::clamp(int(std::lround(.008*rate)),32,maxHop)),
      fadeSamples(std::max(1,int(std::lround(.020*rate)))), position(begin),
      step(wav->sampleRate/rate), playbackFollow(-std::expm1(-1./(.003*rate))),
      nominalPeriod(wav->sampleRate/std::max(1.,wav->slicePitchHz(begin,end,nominalHz))) {
    (void)kernels();
    (void)vocal::Fourier<fftSize>::plan();
    for (int i=0; i<hop; ++i)
        fade[i]=float(.5-.5*std::cos(std::numbers::pi*i/hop));
    const int pad=(fftSize-2*hop)/2;
    for (int i=0; i<fftSize; ++i) {
        const int edge=std::min({i,fftSize-1-i,pad});
        guard[i]=float(.5-.5*std::cos(std::numbers::pi*edge/pad));
    }
}

SlideVoice::Stereo SlideVoice::resample(double where, double speed) const {
    if (where<begin || where>=end) return {};
    const int base=int(where);
    const int phase=int((where-base)*64.);
    const int band=std::clamp(int(std::ceil(4.*(speed-1.))),0,60);
    const auto& taps=kernels().table[band][phase];
    Stereo v{};
    const int first=base-7;
    const bool interior=first>=begin && first+15<end;
    for (int i=0; i<16; ++i) {
        const int t=interior ? first+i : std::clamp(first+i,begin,end-1);
        v[0]+=taps[i]*wav->stereo[2*t];
        v[1]+=taps[i]*wav->stereo[2*t+1];
    }
    return v;
}

SlideVoice::Stereo SlideVoice::read(double where) const noexcept {
    const double bounded = std::clamp(where, double(begin), double(end - 1));
    const int frame = int(bounded);
    const float fraction = float(bounded - frame);
    const auto sample = [&](int offset, int channel) {
        return wav->stereo[2 * size_t(std::clamp(frame + offset, begin, end - 1)) + channel];
    };
    Stereo result{sample(0, 0), sample(0, 1)};
    if (fraction == 0.f) return result;
    for (int channel = 0; channel < 2; ++channel) {
        const float a = sample(-1, channel), b = result[channel];
        const float c = sample(1, channel), d = sample(2, channel);
        result[channel] = b + 0.5f * fraction *
            ((c - a) + fraction * ((2.f * a - 5.f * b + 4.f * c - d)
                                    + fraction * (3.f * (b - c) + d - a)));
    }
    return result;
}

double SlideVoice::align(double nominal, double previous, double speed, double period) const {
    // Compare stereo waveforms over the overlap. A single displacement keeps both
    // channels phase-aligned; a low correlation never disables transposition.
    constexpr int points=48;
    const double span=std::min(double(wav->sampleRate)*.014,std::max(24.,hop*speed));
    const double radius=std::clamp(period*1.15,24.,wav->sampleRate*.019);
    const int start=std::max(begin,int(std::ceil(nominal-radius)));
    const int stop=std::min(end-1,int(std::floor(nominal+radius)));
    if (stop<=start || previous<begin || previous+span>=end) return nominal;
    std::array<int,points> offsets;
    std::array<Stereo,points> ref;
    double refEnergy=0.;
    for (int i=0; i<points; ++i) {
        offsets[i]=int((i+.5)*span/points);
        const int index=int(previous)+offsets[i];
        ref[i]={wav->stereo[2*index],wav->stereo[2*index+1]};
        refEnergy+=double(ref[i][0])*ref[i][0]+double(ref[i][1])*ref[i][1];
    }
    if (refEnergy<1.e-12) return nominal;
    const auto score=[&](int candidate) {
        if (candidate<start || candidate>stop || candidate+offsets.back()>=end) return -2.;
        double cross=0., energy=0.;
        for (int i=0; i<points; ++i) {
            const float* p=&wav->stereo[2*(candidate+offsets[i])];
            cross+=double(ref[i][0])*p[0]+double(ref[i][1])*p[1];
            energy+=double(p[0])*p[0]+double(p[1])*p[1];
        }
        return cross/std::sqrt(refEnergy*energy+1.e-24)-.003*std::abs(candidate-nominal)/radius;
    };
    int best=std::clamp(int(nominal),start,stop);
    double high=score(best);
    const auto probe=[&](int i) { double s=score(i); if (s>high) {best=i; high=s;} };
    const int stride=std::clamp(int(period/32),1,8);
    for (int i=start; i<=stop; i+=stride) probe(i);
    const int coarse=best;
    for (int i=std::max(start,coarse-stride); i<=std::min(stop,coarse+stride); ++i) probe(i);
    if (high<=-1.) return nominal;
    const double l=score(best-1), r=score(best+1);
    const double delta=std::clamp(.5*(r-l)/std::max(1.e-10,2.*high-l-r),-.5,.5);
    return std::clamp(best+delta+previous-std::floor(previous),double(begin),double(end-1));
}

void SlideVoice::makeGrain(int64_t t) {
    const auto& initial=frame(t);
    const auto& middle=frame(t+hop);
    const int pad=(fftSize-2*hop)/2;
    const bool first=grainOffset<0;
    const double nominal=initial.position;
    double period=nominalPeriod;
    if (!wav->vocal.empty()) {
        const auto& v=wav->vocal[std::min(size_t(nominal/wav->analysisHop),wav->vocal.size()-1)];
        if (v.period>0.) period=v.period;
    }
    double source=first ? nominal : align(nominal,nextAnchor,initial.wetStep,period);
    if (!wav->attacks.empty()) {
        const auto& last=frame(t+2*hop-1);
        const double nominalEnd=last.position+last.samplerStep;
        double travel=0.;
        for (int k=0; k<2*hop; ++k) travel+=frame(t+k).wetStep;
        const double lo=std::max(double(begin),std::min(nominal,source));
        const double hi=std::min(double(end),std::max(nominalEnd,source+travel));
        auto attack=std::lower_bound(wav->attacks.begin(),wav->attacks.end(),lo);
        if (attack!=wav->attacks.end() && *attack<hi) {
            // Account for attacks a fast read would otherwise play twice, even
            // if the event belongs just outside this grain's nominal interval.
            double distance=0.;
            if (*attack<nominal) {
                distance=(*attack-nominal)*initial.wetStep/initial.samplerStep;
            } else if (*attack>=nominalEnd) {
                distance=travel+(*attack-nominalEnd)*last.wetStep/last.samplerStep;
            } else {
                for (int k=0; k<2*hop; ++k) {
                    const auto& f=frame(t+k);
                    const double part=std::clamp((*attack-f.position)/f.samplerStep,0.,1.);
                    distance+=part*f.wetStep;
                    if (part<1.) break;
                }
            }
            source=*attack-distance;
        }
    }
    const double anchor=source;
    source-=pad*initial.wetStep;
    for (int i=0; i<fftSize; ++i) {
        const int k=i-pad;
        if (k==0) source=anchor;
        if (k==hop) nextAnchor=source;
        const double speed=frame(t+std::clamp(k,0,2*hop-1)).wetStep;
        const auto s=resample(source,speed);
        spectrum[i]={guard[i]*s[0],guard[i]*s[1]};
        source+=speed;
    }
    if (!wav->envelopes.empty()) {
        const auto& plan=vocal::Fourier<fftSize>::plan();
        plan.transform(spectrum);
        const double idx=middle.position/wav->envelopeHop;
        const size_t a=std::min(size_t(idx),wav->envelopes.size()-1);
        const size_t b=std::min(a+1,wav->envelopes.size()-1);
        const auto env=[&](double k) {
            return std::lerp(envelopeAt(wav->envelopes[a],k),envelopeAt(wav->envelopes[b],k),
                             float(std::clamp(idx-a,0.,1.)));
        };
        // Zero-phase EQ restores the original resonances at sampler speed.
        // The waveform already has its final pitch before this transform.
        double inputPower=0., correctedPower=0.;
        for (int k=0; k<=fftSize/2; ++k) {
            const float difference=env(k/middle.samplerStep)-env(k/middle.wetStep);
            const float gain=std::exp2(std::clamp(difference,-7.f,7.f));
            double power=std::norm(spectrum[k]);
            if (k>0 && k<fftSize/2) power+=std::norm(spectrum[fftSize-k]);
            inputPower+=power;
            correctedPower+=power*gain*gain;
            spectrum[k]*=gain;
            if (k>0 && k<fftSize/2) spectrum[fftSize-k]*=gain;
        }
        plan.transform(spectrum,true);
        const float level=float(std::clamp(std::sqrt((inputPower+1.e-16)/(correctedPower+1.e-16)),.0625,64.));
        for (int i=pad; i<pad+2*hop; ++i) spectrum[i]*=level;
    }
    for (int i=0; i<hop; ++i) {
        overlap[i]=grain[i+hop];
        grain[i]={spectrum[pad+i].real(),spectrum[pad+i].imag()};
        grain[i+hop]={spectrum[pad+i+hop].real(),spectrum[pad+i+hop].imag()};
    }
    if (!first) {
        double xx=0., yy=0., xy=0.;
        for (int i=0; i<hop; ++i) for (int c=0; c<2; ++c) {
            const double x=overlap[i][c], y=grain[i][c];
            xx+=x*x; yy+=y*y; xy+=x*y;
        }
        const double rho=std::clamp(xy/std::sqrt(xx*yy+1.e-24),0.,1.);
        for (int i=0; i<hop; ++i) {
            const float w=fade[i];
            const float gain=float(1./std::sqrt(1.-2.*w*(1.-w)*(1.-rho)));
            for (int c=0; c<2; ++c)
                grain[i][c]=std::lerp(overlap[i][c],grain[i][c],w)*gain;
        }
    }
    grainOffset=0;
}

SlideVoice::Stereo SlideVoice::wet(int64_t t) {
    if (grainOffset<0 || grainOffset==hop) makeGrain(t);
    return grain[grainOffset++];
}

SlideVoice::Stereo SlideVoice::mix(const Stereo& dry, const Stereo& wet, float amount) {
    if (amount==1.f) return wet;
    const auto dot=[](const Stereo& a,const Stereo& b) {
        return double(a[0])*b[0]+double(a[1])*b[1];
    };
    dryPower+=playbackFollow*(dot(dry,dry)-dryPower);
    wetPower+=playbackFollow*(dot(wet,wet)-wetPower);
    crossPower+=playbackFollow*(dot(dry,wet)-crossPower);
    const double rho=std::clamp(crossPower/std::sqrt(dryPower*wetPower+1.e-30),0.,1.);
    const double a=std::cos(amount*std::numbers::pi/2), b=std::sin(amount*std::numbers::pi/2);
    const double g=1./std::sqrt(1.+2.*a*b*rho);
    return {float(g*(a*dry[0]+b*wet[0])),float(g*(a*dry[1]+b*wet[1]))};
}

int SlideVoice::render(float* left,float* right,int count,double scale) {
    if (count<=0) return 0;
    const double target=std::isfinite(scale) ? std::log2(std::clamp(scale,1./16.,16.)) : 0.;
    if (!clock) playback=playbackTarget;
    if (snapPitch) previousPitch=target;
    snapPitch=false;
    int n=0;
    for (; n<count; ++n,++clock) {
        const int64_t age=releaseAt<0 ? 0 : clock-releaseAt;
        if (sourceEnd<0 && (position>=end || age>=fadeSamples)) sourceEnd=clock;
        if (sourceEnd>=0 && clock>=sourceEnd+latencySamples) break;
        auto& f=frame(clock);
        const double pitch=std::lerp(previousPitch,target,double(n+1)/count);
        const bool active=sourceEnd<0 && (slideActive || std::abs(pitch)>1.e-9);
        mixStep=std::clamp(mixStep+(active?1:-1),0,fadeSamples);
        const float x=float(mixStep)/fadeSamples;
        f.mix=x*x*(3.f-2.f*x);
        f.gain=std::max(0.f,1.f-float(age)/fadeSamples);
        f.position=position;
        f.dry=sourceEnd<0 ? read(position) : Stereo{};
        playback+=playbackFollow*(playbackTarget-playback);
        f.samplerStep=step*playback;
        f.wetStep=mixStep ? f.samplerStep*std::exp2(pitch) : f.samplerStep;
        if (sourceEnd<0) position+=f.samplerStep;
        Stereo sample{};
        if (clock>=latencySamples) {
            const int64_t t=clock-latencySamples;
            const auto& delayed=frame(t);
            sample=delayed.dry;
            if (delayed.mix>0.f) sample=mix(sample,wet(t),delayed.mix);
            else grainOffset=-1;
            if (delayed.gain!=1.f) for (float& s:sample) s*=delayed.gain;
        }
        left[n]=sample[0];
        right[n]=sample[1];
    }
    previousPitch=target;
    return n;
}

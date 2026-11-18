#include "StftEngine.h"
#include <cmath>
#include <utility>

namespace {
double smooth(double t) { return t*t*(3.-2.*t); }
double toward(double from, double to, double amount) {
    return from+std::clamp(to-from,-amount,amount);
}
}

StftVoice::StftVoice(std::shared_ptr<const WaveData> data,int first,int last,double rate,double hz,
                     double sourceStep, ResidualSpectrum::Plans& plans)
    : wav(std::move(data)),begin(std::clamp(first,0,wav->frames())),end(std::clamp(last,begin,wav->frames())),
      step(sourceStep),position(begin),startHz(hz),pitchFollow(-std::expm1(-1./(0.003*rate))),
      fadeStep(1./(0.008*wav->sampleRate)),levelFollow(-std::expm1(-1./(0.020*wav->sampleRate))),
      periodFollow(-std::expm1(-1./(0.004*wav->sampleRate))),
      switchLength(std::max(1,int(rate*0.003))),
      overlapHop(std::max(8,wav->sampleRate/80)), spectrum(plans) {}

void StftVoice::setEngineBias(int value) {
    const double next=std::clamp(value,-100,100)/100.;
    if (next!=bias && std::abs(next)==1.) {
        // The interpolation history can contain samples from the other engine.
        alignNextSample=wet>0.;
        rendered=-1;
        retry=0;
        lastSpectralPercent=-1;
        spectrum.reset();
    }
    bias=next;
}

StftVoice::Stereo StftVoice::read(const std::vector<float>& data,double at) const {
    if (at<begin || at>=end) return {};
    const int i=int(at);
    auto get=[&](int n) {
        const size_t k=2*size_t(std::clamp(n,begin,end-1));
        return Stereo(data[k],data[k+1]);
    };
    const float f=float(at-i);
    const auto b=get(i);
    if (f==0) return b;
    const auto a=get(i-1), c=get(i+1), d=get(i+2);
    return b+0.5f*f*((c-a)+f*((2.f*a-5.f*b+4.f*c-d)+f*(3.f*(b-c)+d-a)));
}

void StftVoice::start(int at) {
    rendered=at-1;
    previousDelta=currentDelta={};
    upperPulse=size_t(std::upper_bound(wav->pulses.begin(),wav->pulses.end(),at)-wav->pulses.begin());
    period=0;
    overlapAge=-1;
    pulseMix=0;
    preferPulse=false;
    firstWetSample=true;
    voicedRun=spectralZeroRun=0;
    spectrum.reset();
    retry=0;
    lattice={};
    sourcePower=outputPower=0;
    gain=1;
    // The backwards errors depend only on the last filterOrder input samples.
    // Reconstruct that state when a slide starts in the middle of a dry note.
    for (int n=std::max(begin,at-wav->filterOrder); n<at; ++n) {
        const auto k=filterAt(*wav,n);
        for (int ch=0;ch<2;++ch)
            lattice[ch].analyse(wav->stereo[2*size_t(n)+ch],k,wav->filterOrder);
    }
}

bool StftVoice::trackPulse(int at,const VocalFrame& frame) {
    const auto& marks=wav->pulses;
    while (upperPulse<marks.size() && marks[upperPulse]<=at) ++upperPulse;
    if (frame.period<=0 || !upperPulse || upperPulse>=marks.size()) return false;
    const double left=marks[upperPulse-1], right=marks[upperPulse];
    const double length=right-left;
    if (left<begin || right>=end || length<0.75*frame.period || length>1.3*frame.period) return false;
    mark=left;
    period=length;
    previousPeriod=length;
    nextPeriod=length;
    if (upperPulse>=2 && marks[upperPulse-2]>=begin) {
        const double candidate=left-marks[upperPulse-2];
        if (candidate>=0.7*length && candidate<=1.4*length) previousPeriod=candidate;
    }
    if (upperPulse+1<marks.size() && marks[upperPulse+1]<end) {
        const double candidate=marks[upperPulse+1]-right;
        if (candidate>=0.7*length && candidate<=1.4*length) nextPeriod=candidate;
    }
    cycleBlend=(at-left)/length;
    return true;
}

StftVoice::Stereo StftVoice::pulse(double ratio) {
    // Two pitch-synchronous, raised-cosine windows overlap on the excitation.
    // Their weights sum to one and their positions meet at each phase wrap.
    // Moving to the next pair of source marks preserves the output clock.
    const double offset=phase*period;
    const float tail=float(0.5-0.5*std::cos(3.141592653589793*phase));
    const auto middle=excitationReader.read(*wav,begin,end,mark+offset);
    const auto earlier=excitationReader.read(*wav,begin,end,mark+(phase-1.)*previousPeriod);
    const auto later=excitationReader.read(*wav,begin,end,mark+period+phase*nextPeriod);
    const auto a=middle+tail*(earlier-middle);
    const auto b=later+tail*(middle-later);
    phase+=ratio*cycleRate;
    phase-=std::floor(phase);
    return a+float(smooth(cycleBlend))*(b-a);
}

// Waveform-similarity overlap-add for the temporal endpoint between marked
// regions. Both heads read the excitation at the requested pitch throughout.
StftVoice::Stereo StftVoice::unmarkedGrain(int at,double ratio) {
    if (overlapAge<0) {
        overlapAge=0;
        olderHead=newerHead=at;
        overlapCorrelation=1.;
    } else if (overlapAge==overlapHop) {
        overlapAge=0;
        olderHead=newerHead;
        const double nominal=std::clamp(at+(1.-ratio)*overlapHop,double(begin),double(end-1));
        const int radius=std::max(2,wav->sampleRate/300);
        const int stride=std::max(1,radius/48);
        constexpr int probes=24;
        std::array<Stereo,probes> reference{};
        double referencePower=0.;
        for (int j=0;j<probes;++j) {
            reference[j]=read(wav->excitation,olderHead+ratio*overlapHop*(j+0.5)/probes);
            referencePower+=std::norm(reference[j]);
        }
        newerHead=nominal;
        overlapCorrelation=0.;
        double best=-2.;
        for (int d=-radius;d<=radius;d+=stride) {
            const double candidate=nominal+d;
            if (candidate<begin || candidate>=end) continue;
            double dot=0., power=0.;
            for (int j=0;j<probes;++j) {
                const auto sample=read(wav->excitation,candidate+ratio*overlapHop*(j+0.5)/probes);
                dot+=reference[j].real()*sample.real()+reference[j].imag()*sample.imag();
                power+=std::norm(sample);
            }
            const double correlation=dot/std::sqrt(referencePower*power+1e-18);
            const double distance=double(d)/radius;
            const double score=correlation-0.12*distance*distance;
            if (score>best) {
                best=score;
                newerHead=candidate;
                overlapCorrelation=std::clamp(correlation,0.,1.);
            }
        }
    }
    const double t=smooth(double(overlapAge)/overlapHop);
    const double power=1.-2.*t*(1.-t)*(1.-overlapCorrelation);
    const auto result=(float(1.-t)*excitationReader.read(*wav,begin,end,olderHead)
                      +float(t)*excitationReader.read(*wav,begin,end,newerHead))/float(std::sqrt(power));
    olderHead+=ratio;
    newerHead+=ratio;
    ++overlapAge;
    return result;
}

StftVoice::Stereo StftVoice::renderNative(int at,double ratio) {
    const auto dry=read(wav->stereo,at);
    const auto raw=read(wav->excitation,at);
    const size_t index=size_t(at/wav->analysisHop);
    const auto& frame=wav->vocal[index];
    const bool hadCycle=period>0;
    const bool onlyPulse=bias<=-1., onlySpectrum=bias>=1.;
    const bool valid=!onlySpectrum && trackPulse(at,frame);
    if (valid && !hadCycle) phase=cycleBlend;
    // Begin leaving a marked region before the last usable cycle. Confidence
    // moves the decision thresholds; it never sets a sustained audio mix.
    const size_t next=std::min(index+1,size_t((end-1)/wav->analysisHop));
    const double t=double(at%wav->analysisHop)/wav->analysisHop;
    const auto& upcoming=wav->vocal[next];
    if (valid) {
        // Epoch spacing locates the waveform. Interpolated F0 drives the
        // synthesis clock, so small landmark errors do not become pitch jitter.
        double targetPeriod=frame.period;
        if (upcoming.period>0.8*targetPeriod && upcoming.period<1.25*targetPeriod)
            targetPeriod+=t*(upcoming.period-targetPeriod);
        const double targetRate=1./targetPeriod;
        if (hadCycle) cycleRate+=periodFollow*(targetRate-cycleRate);
        else cycleRate=targetRate;
    }
    const double confidence=frame.voiced+t*(std::min(frame.voiced,upcoming.voiced)-frame.voiced);
    const double highAssist=startHz>=limit ? 0.10*std::clamp(double(guard*relax),0.,1.) : 0.;
    const double threshold=std::clamp((preferPulse ? 0.57 : 0.76)+0.45*bias+highAssist,0.05,1.);
    const bool usable=valid && (onlyPulse || confidence>=threshold);
    const int settle=std::max(1,wav->sampleRate/100);
    voicedRun=usable ? std::min(settle,voicedRun+1) : 0;
    preferPulse=usable && (onlyPulse || preferPulse || firstWetSample || voicedRun>=settle);
    const double desired=preferPulse ? 1. : 0.;
    pulseMix=firstWetSample ? desired : toward(pulseMix,desired,fadeStep);
    firstWetSample=false;
    const double amount=smooth(pulseMix);
    Stereo temporal=raw;
    if (onlyPulse || pulseMix>0) excitationReader.setSpeed(ratio);
    if (period>0 && pulseMix>0) temporal=pulse(ratio);
    else if (valid) phase=cycleBlend;
    if (!valid && pulseMix==0) period=0;

    Stereo alternate=raw;
    bool spectralOutput=false;
    if (onlyPulse && amount<1.) alternate=unmarkedGrain(at,ratio);
    else overlapAge=-1;
    if (onlyPulse || amount==1.) {
        spectrum.reset();
        spectralZeroRun=0;
    } else if (retry>0) {
        --retry;
    } else {
        try {
            alternate=spectrum.next(*wav,begin,end,at,ratio);
            spectralOutput=std::isfinite(alternate.real()) && std::isfinite(alternate.imag());
            if (alternate!=Stereo{}) spectralZeroRun=0;
            else if (std::norm(raw)>1e-10f) ++spectralZeroRun;
            if (spectralZeroRun>std::max(2048,wav->sampleRate/12)) spectralOutput=false;
        } catch (...) { spectralOutput=false; }
        if (!spectralOutput) {
            spectrum.reset();
            spectralZeroRun=0;
            retry=std::max(1,wav->sampleRate/100);
            alternate=raw;
        }
    }
    const auto excitation=alternate+float(amount)*(temporal-alternate);
    const auto k=filterAt(*wav,at);
    Stereo result{float(lattice[0].synthesise(excitation.real(),k,wav->filterOrder)),
                  float(lattice[1].synthesise(excitation.imag(),k,wav->filterOrder))};
    if (!std::isfinite(result.real()) || !std::isfinite(result.imag())) {
        lattice={};
        result=dry;
    }
    sourcePower+=levelFollow*(std::norm(dry)-sourcePower);
    outputPower+=levelFollow*(std::norm(result)-outputPower);
    const double gainLimit=1.15+0.45*frame.voiced;
    const double targetGain=std::clamp(std::sqrt((sourcePower+1e-8)/(outputPower+1e-8)),0.7,gainLimit);
    gain+=levelFollow*(targetGain-gain);
    lastSpectralPercent=spectralOutput ? int(std::lround(100*smooth(wet)*(1.-amount))) : 0;
    return float(gain)*result-dry;
}

int StftVoice::render(float* left,float* right,int count,double scale) {
    const double target=std::log2(std::isfinite(scale) ? std::clamp(scale,1./64.,64.) : 1.);
    if (firstRender && count>0) {
        logPitch=target;
        wet=target==0. ? 0. : 1.;
        firstRender=false;
    }
    const double hostFade=fadeStep*step;
    int i=0;
    for (; i<count && position<end; ++i,position+=step) {
        logPitch+=pitchFollow*(target-logPitch);
        if (std::abs(target-logPitch)<1e-9) logPitch=target;
        wet=toward(wet,(target!=0. || std::abs(logPitch)>1e-5) ? 1. : 0.,hostFade);
        auto sample=read(wav->stereo,position);
        if (wet>0.) {
            int n=int(position);
            if (rendered<0) start(n);
            const double ratio=std::exp2(logPitch);
            while (rendered<std::min(n+1,end-1)) {
                previousDelta=currentDelta;
                currentDelta=renderNative(++rendered,ratio);
            }
            const auto delta=rendered==n ? currentDelta :
                previousDelta+float(position-n)*(currentDelta-previousDelta);
            sample+=float(smooth(wet))*delta;
        } else {
            rendered=-1;
            lastSpectralPercent=-1;
        }
        if (alignNextSample) {
            switchOffset=lastOutput-sample;
            switchRemaining=switchLength;
            alignNextSample=false;
        }
        if (switchRemaining>0) {
            sample+=float(smooth(double(switchRemaining)/switchLength))*switchOffset;
            --switchRemaining;
        }
        lastOutput=sample;
        left[i]=sample.real();
        right[i]=sample.imag();
    }
    return i;
}

#pragma once
#include "ResidualSpectrum.h"
#include "ExcitationReader.h"
#include "VocalFilter.h"
#include <memory>

class StftVoice {
public:
    StftVoice(std::shared_ptr<const WaveData>, int first, int end, double rate, double hz,
              double sourceStep, ResidualSpectrum::Plans&);
    int render(float*,float*,int,double);
    int spectralPercent() const { return lastSpectralPercent; }
    bool retryingSpectrum() const { return retry>0; }
    void setEngineBias(int value);
    void setFormantGuard(float v) { guard=v; }
    void setHighNoteLimit(float v) { limit=v; }
    void setHighNoteRelaxMax(float v) { relax=v; }

private:
    using Stereo = ResidualSpectrum::Stereo;
    std::shared_ptr<const WaveData> wav;
    int begin,end;
    double step,position,startHz,pitchFollow,fadeStep,levelFollow,periodFollow;
    double logPitch=0, wet=0, pulseMix=0, bias=0;
    float guard=0,limit=2000,relax=0.75f;
    bool preferPulse=false, firstWetSample=false, firstRender=true;
    int voicedRun=0, spectralZeroRun=0, switchLength, switchRemaining=0;
    bool alignNextSample=false;
    Stereo lastOutput{}, switchOffset{};
    size_t upperPulse=0;
    double mark=0,period=0,previousPeriod=0,nextPeriod=0,cycleBlend=0,phase=0,cycleRate=0;
    int overlapAge=-1, overlapHop;
    double olderHead=0, newerHead=0, overlapCorrelation=1;
    int rendered=-1,retry=0,lastSpectralPercent=-1;
    Stereo previousDelta{},currentDelta{};
    double sourcePower=0,outputPower=0,gain=1;
    ResidualSpectrum spectrum;
    ExcitationReader excitationReader;
    std::array<VocalLattice,2> lattice{};
    Stereo read(const std::vector<float>&, double) const;
    bool trackPulse(int at, const VocalFrame&);
    Stereo pulse(double ratio);
    Stereo unmarkedGrain(int at, double ratio);
    Stereo renderNative(int at,double ratio);
    void start(int at);
};

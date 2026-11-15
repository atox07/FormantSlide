#pragma once
#include "WaveLoader.h"
#include <algorithm>
#include <array>
#include <memory>

class StftVoice {
public:
    StftVoice(std::shared_ptr<const WaveData>, int, int, double, double);
    int render(float*, float*, int, double);
    void jumpPitch() { snap = true; }
    void setFormantScale(double v) { formant = std::clamp(v,1./64,64.); }
    void setFormantGuard(float v) { guard = v; }
    void setHighNoteLimit(float v) { limit = v; }
    void setHighNoteRelaxMax(float v) { relax = v; }
private:
    struct Grain { double source, speed, phase, increment; };
    std::shared_ptr<const WaveData> wav;
    int begin,end;
    double step,startNote,smoothing,position,pitch=0,formant=1,color=1,untilPulse=0,mix=0;
    bool snap=true;
    float guard=0,limit=2000,relax=0.75f;
    std::array<Grain,32> grains{};
    int used=0;
    float read(double where,int channel) const;
};

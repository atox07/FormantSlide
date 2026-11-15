#include "StftEngine.h"
#include <cmath>
#include <utility>

StftVoice::StftVoice(std::shared_ptr<const WaveData> data,int first,int last,double rate,double hz)
    : wav(std::move(data)), begin(std::clamp(first,0,wav->frames())),
      end(std::clamp(last,begin,wav->frames())), step(wav->sampleRate/rate),
      startNote(hz), smoothing(-std::expm1(-1./(0.006*rate))), position(begin) {}
float StftVoice::read(double where, int ch) const {
    if (where < begin || where >= end) return 0.f;
    const float* s = wav->stereo.data();
    const int a = int(where);
    const float f = float(where - a);
    auto at = [&](int i) {
        return s[size_t(std::clamp(i, begin, end - 1)) * 2 + size_t(ch)];
    };
    if (f == 0.f) return at(a);
    const float y0 = at(a - 1), y1 = at(a), y2 = at(a + 1), y3 = at(a + 2);
    const float c1 = 0.5f * (y2 - y0);
    const float c2 = y0 - 2.5f * y1 + 2.f * y2 - 0.5f * y3;
    const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
    return ((c3 * f + c2) * f + c1) * f + y1;
}

int StftVoice::render(float* left,float* right,int count,double scale) {
    const double target = std::log2(std::isfinite(scale) ? std::clamp(scale,1./64,64.) : 1.);
    if (snap) { pitch = target; snap = false; }
    int i = 0;
    for (; i < count && position < end; ++i, position += step) {
        pitch += smoothing*(target-pitch);
        const double ratio = std::exp2(pitch);
        const double release = startNote >= limit ? std::clamp(double(guard*relax),0.,1.) : 0.;
        color += smoothing*(formant*std::pow(ratio/(formant*formant),release)-color);
        const size_t index = size_t(position)/wav->analysisHop;
        const VocalFrame frame = index < wav->vocal.size() ? wav->vocal[index] : VocalFrame{};
        const bool shifted = std::abs(pitch)>1e-5 || std::abs(color-1)>1e-5;
        mix += smoothing*((shifted ? frame.voiced : 0.)-mix);
        // TD-PSOLA: select a source pulse, place its waveform at the MIDI period.
        if (untilPulse <= 0 && shifted && frame.period > 0 && used < int(grains.size())) {
            const double spacing = std::max(1.,frame.period/(step*ratio));
            const double radius = std::clamp(0.8*frame.period/(step*color), std::max(1.,0.65*spacing), spacing*12.);
            const double center = position+radius*step;
            auto mark = std::lower_bound(wav->pulses.begin(),wav->pulses.end(),center);
            if (mark != wav->pulses.begin() &&
                (mark == wav->pulses.end() || center-mark[-1] < *mark-center)) --mark;
            if (mark != wav->pulses.end() && *mark >= begin && *mark < end &&
                std::abs(*mark-center) <= frame.period)
                grains[used++] = {*mark-radius*step*color, step*color, 0., 0.5/radius};
            untilPulse += spacing;
        }
        untilPulse = std::max(-1.,untilPulse-1.);
        double weight = 0, l = 0, r = 0;
        for (int g = 0; g < used;) {
            auto& grain = grains[g];
            if (grain.source >= begin && grain.source < end) {
                const double w = 0.5-0.5*std::cos(6.283185307179586*grain.phase);
                weight += w;
                l += w*read(grain.source,0);
                r += w*read(grain.source,1);
            }
            grain.source += grain.speed;
            grain.phase += grain.increment;
            if (grain.phase >= 1.) grain = grains[--used];
            else ++g;
        }
        const double wet = mix*std::min(1.,weight*8.);
        const double gain = wet/std::max(1e-9,weight);
        left[i] = float((1-wet)*read(position,0)+gain*l);
        right[i] = float((1-wet)*read(position,1)+gain*r);
    }
    return i;
}

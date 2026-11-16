#include "StftEngine.h"
#include <cmath>
#include <utility>

StftVoice::StftVoice(std::shared_ptr<const WaveData> data,int first,int last,double rate,double hz,double sourceStep)
    : wav(std::move(data)), begin(std::clamp(first,0,wav->frames())),
      end(std::clamp(last,begin,wav->frames())), step(sourceStep),
      startNote(hz), smoothing(-std::expm1(-1./(0.006*rate))), position(begin) {
    for (int j=0; j<size; ++j)
        window[j] = float(0.5-0.5*std::cos(6.283185307179586*j/size));
}
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

// Overlap-add frames share the exact source timeline of the PSOLA grains.
void StftVoice::spectralFrame(int offset, double ratio, double formantScale, bool first) {
    constexpr double tau = 6.283185307179586;
    constexpr int nyquist = size/2;
    for (int ch=0; ch<2; ++ch) {
        auto& state = spectral[ch];
        for (int j=0; j<size; ++j)
            input[j] = window[j]*read(position+(offset+j)*step,ch);
        fft.perform(input.data(),bins.data(),false);
        output.fill({});
        for (int k=0; k<=nyquist; ++k) magnitude[k] = std::abs(bins[k]);
        // Local RMS envelope provides a bounded formant correction. The
        // local floor controls how much diffuse energy stays at source Hz.
        double energy = 0;
        constexpr int width = 8;
        for (int j=0; j<width; ++j) energy += double(magnitude[j])*magnitude[j];
        int count = 0;
        for (int k=0; k<=nyquist; ++k) {
            if (k+width<=nyquist) energy += double(magnitude[k+width])*magnitude[k+width];
            if (k>width) energy -= double(magnitude[k-width-1])*magnitude[k-width-1];
            envelope[k] = float(std::sqrt(std::max(0.,energy)/
                (1+std::min(nyquist,k+width)-std::max(0,k-width))));
            if (k>0 && k<nyquist && magnitude[k]>magnitude[k-1] && magnitude[k]>=magnitude[k+1])
                peaks[count++] = k;
        }
        int nearest = 0;
        for (int k=0; k<=nyquist; ++k) {
            const double angle = std::arg(bins[k]);
            const double expected = tau*hop*k/size;
            const double advance = expected + (first ? 0. : std::remainder(angle-state.phase[k]-expected,tau));
            const float frequency = float(advance*(size/(tau*hop)));
            const float drift = std::abs(frequency-state.peakFrequency[k]);
            const float evidence = std::clamp(1.f-drift/0.20f,0.f,1.f);
            // Neighbouring harmonics can bury each other's spectral contrast.
            // Accumulate evidence only for a resolved, persistent peak.
            const bool peak = k>0 && k<nyquist && magnitude[k]>magnitude[k-1] &&
                              magnitude[k]>=magnitude[k+1] && magnitude[k]>1e-6f;
            const float target = peak && std::abs(frequency-k)<0.65f ? evidence : 0.f;
            state.stableTone[k] = first ? 0.f : state.stableTone[k] +
                (target-state.stableTone[k])*(target>state.stableTone[k] ? 0.30f : 0.70f);
            state.peakFrequency[k] = frequency;
            state.phase[k] = angle;
            state.rotation[k] = first ? 0. : std::remainder(state.rotation[k]+(ratio-1)*advance,tau);
        }
        for (int k=0; k<=nyquist; ++k) {
            if (!count) { output[k] += bins[k]; continue; }
            while (nearest+1<count && 2*k>peaks[nearest]+peaks[nearest+1]) ++nearest;
            const int pk = peaks[nearest];
            const float contrast = magnitude[pk]/std::max(1e-8f,envelope[pk]);
            const float prominence = std::clamp((contrast-1.35f)/1.35f,0.f,1.f);
            const float persistence = std::clamp((state.stableTone[pk]-0.5f)*2.5f,0.f,1.f);
            const float tonal = std::max(prominence,persistence)*
                                std::clamp((4.f-std::abs(k-pk))/2.f,0.f,1.f);
            output[k] += (1-tonal)*bins[k];
            const double dest = k+(ratio-1)*pk;
            if (dest<1 || dest>=nyquist-1 || tonal<=0) continue;
            const int bin = int(std::round(dest));
            const int envBin = int(std::clamp(dest/formantScale,0.,double(nyquist)));
            const float gain = std::clamp(envelope[envBin]/std::max(envelope[k],1e-8f),0.24f,4.f);
            // Phase is referenced to the middle of the Hann window.
            const double rotation = state.rotation[pk]+(pk*(ratio-1)-(bin-k))*tau*0.5;
            output[bin] += bins[k]*std::polar(tonal*gain,float(rotation));
        }
        for (int k=1; k<nyquist; ++k) output[size-k] = std::conj(output[k]);
        fft.perform(output.data(),input.data(),true);
        for (int j=std::max(0,-offset); j<size; ++j)
            state.overlap[(spectralCursor+offset+j)&(size-1)] +=
                input[j].real()*window[j]*(2.f/3.f);
    }
}

void StftVoice::recoverSpectrum() {
    spectralActive = false;
    spectralRetry = size;
    spectralReturn = 0;
    silentSpectrum = 0;
    for (auto& channel : spectral) channel.overlap.fill(0);
    spectralPower = crossPower = 0;
    blendGain = 1;
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
        VocalFrame frame = index < wav->vocal.size() ? wav->vocal[index] : VocalFrame{};
        if (frame.period > 0 && index+1 < wav->vocal.size()) {
            const auto& next = wav->vocal[index+1];
            if (next.period > 0 && std::abs(next.period-frame.period) < 0.2f*frame.period) {
                const float t = float(position/wav->analysisHop - double(index));
                frame.period += t*(next.period-frame.period);
            }
        }
        const bool shifted = std::abs(pitch)>1e-5 || std::abs(color-1)>1e-5;
        double confidence = frame.voiced;
        if (index+1 < wav->vocal.size())
            confidence += (wav->vocal[index+1].voiced-confidence)*
                          (position/wav->analysisHop-double(index));
        mix += (0.35*smoothing)*(confidence-mix);
        effectMix += smoothing*((shifted ? 1. : 0.)-effectMix);
        const bool process = shifted || effectMix>1e-5;
        if (!process) spectralActive = false;
        if (spectralRetry > 0) --spectralRetry;
        else if (process && (!spectralActive || spectralCursor%hop==0)) {
            try {
                if (!spectralActive) {
                    for (auto& channel : spectral) channel.overlap.fill(0);
                    spectralCursor = 0;
                    for (int offset=hop-size; offset<0; offset+=hop)
                        spectralFrame(offset,ratio,color,offset==hop-size);
                }
                spectralFrame(0,ratio,color,false);
                spectralActive = true;
            } catch (...) {
                recoverSpectrum();
            }
        }
        // TD-PSOLA: select a source pulse, place its waveform at the MIDI period.
        if (untilPulse <= 0 && shifted && frame.period > 0 && used < int(grains.size())) {
            const double spacing = std::max(1.,frame.period/(step*ratio));
            const double radius = std::clamp(0.8*frame.period/(step*color), std::max(1.,0.65*spacing), spacing*12.);
            // The pulse is usually due between two output samples.
            const double age = std::max(0.,-untilPulse);
            const double center = position+(radius-age)*step;
            auto mark = std::lower_bound(wav->pulses.begin(),wav->pulses.end(),center);
            if (mark != wav->pulses.begin() &&
                (mark == wav->pulses.end() || center-mark[-1] < *mark-center)) --mark;
            if (mark != wav->pulses.end() && *mark >= begin && *mark < end &&
                std::abs(*mark-center) <= frame.period)
                grains[used++] = {*mark+(age-radius)*step*color, step*color, age/(2*radius), 0.5/radius};
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
        const double dry[2] = {read(position,0),read(position,1)};
        const bool audible = std::abs(dry[0])+std::abs(dry[1]) > 1e-5;
        // Exact-zero runs ignore ordinary low-level output and zero crossings.
        // Require source activity during the run, and test grains only where
        // the analysis predicts a periodic signal inside this slice.
        auto stalled = [&](int& run, bool eligible, bool zero) {
            if (!eligible || !zero) run = 0;
            else if (audible) ++run;
            return run >= size*2;
        };
        double sl = spectral[0].overlap[spectralCursor];
        double sr = spectral[1].overlap[spectralCursor];
        if (!std::isfinite(sl) || !std::isfinite(sr) ||
            stalled(silentSpectrum,spectralActive,sl==0 && sr==0)) {
            recoverSpectrum();
            sl = sr = 0;
        }
        bool expectGrain = shifted && frame.voiced > 0.8 && frame.period > 0 &&
                           position+2*frame.period < end && !wav->pulses.empty();
        if (stalled(silentGrains,expectGrain,l==0 && r==0) ||
            !std::isfinite(l) || !std::isfinite(r) || !std::isfinite(weight)) {
            used = 0;
            untilPulse = 0;
            silentGrains = 0;
            grainPower = crossPower = 0;
            blendGain = 1;
            weight = l = r = 0;
        }
        if (spectralActive) spectralReturn += smoothing*(1.-spectralReturn);
        const double coverage = std::min(1.,weight*8.);
        const double wet = coverage*(1.-spectralReturn*(1.-mix));
        const double norm = 1./std::max(1e-9,weight);
        const double grain[2] = {l*norm,r*norm};
        if (!spectralActive && process) {
            sl = dry[0]; sr = dry[1];
        } else if (spectralReturn<0.99999 && process) {
            sl = dry[0]+spectralReturn*(sl-dry[0]);
            sr = dry[1]+spectralReturn*(sr-dry[1]);
        }
        const double follow = smoothing*0.25;
        spectralPower += follow*(sl*sl+sr*sr-spectralPower);
        grainPower += follow*(grain[0]*grain[0]+grain[1]*grain[1]-grainPower);
        crossPower += follow*(sl*grain[0]+sr*grain[1]-crossPower);
        const double other = 1.-wet;
        if (i+1 == count || position+step >= end)
            lastSpectralPercent = process ? int(100.*effectMix*other*(spectralActive ? spectralReturn : 0.)+0.5) : -1;
        const double expectedPower = other*spectralPower+wet*grainPower;
        const double summedPower = other*other*spectralPower+wet*wet*grainPower+
                                   2*other*wet*crossPower;
        const double compensation = std::clamp(std::sqrt((expectedPower+1e-8)/
                                              (std::max(0.,summedPower)+1e-8)),0.85,1.4);
        blendGain += follow*(compensation-blendGain);
        for (int ch=0; ch<2; ++ch) {
            float& sample = spectral[ch].overlap[spectralCursor];
            const double hybrid = blendGain*(other*(ch ? sr : sl)+wet*grain[ch]);
            (ch ? right : left)[i] = float((1-effectMix)*dry[ch]+effectMix*hybrid);
            sample = 0;
        }
        spectralCursor = (spectralCursor+1)&(size-1);
    }
    return i;
}

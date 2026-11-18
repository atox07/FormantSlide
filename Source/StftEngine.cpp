#include "StftEngine.h"
#include <cmath>
#include <utility>

StftVoice::StftVoice(std::shared_ptr<const WaveData> data,int first,int last,double rate,double hz,double sourceStep)
    : wav(std::move(data)), begin(std::clamp(first,0,wav->frames())),
      end(std::clamp(last,begin,wav->frames())), step(sourceStep),
      startNote(hz), smoothing(-std::expm1(-1./(0.006*rate))), position(begin),
      bodyFilter(-std::expm1(-6.283185307179586*1800./rate)),
      fadeStep(1./std::max(1.,0.008*rate)) {
    for (int j=0; j<size; ++j)
        window[j] = float(0.5-0.5*std::cos(6.283185307179586*j/size));
    for (int k=0; k<=size/2; ++k)
        airBand[k] = float(std::clamp((k*rate/size-3200.)/3200.,0.,1.));
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
void StftVoice::spectralFrame(int offset, double ratio, bool first) {
    constexpr double tau = 6.283185307179586;
    constexpr int nyquist = size/2;
    for (int ch=0; ch<2; ++ch) {
        auto& state = spectral[ch];
        for (int j=0; j<size; ++j)
            input[j] = window[j]*read(position+(offset+j)*step,ch);
        fft.perform(input.data(),bins.data(),false);
        output.fill({});
        for (int k=0; k<=nyquist; ++k) magnitude[k] = std::abs(bins[k]);
        // Compare broad-band power on the same frequency grid before and
        // after relocation, including diffuse components and overlapping peaks.
        constexpr int width = 8;
        auto bandPower = [&](int k, double& power) {
            if (k+width<=nyquist) power += double(magnitude[k+width])*magnitude[k+width];
            if (k>width) power -= double(magnitude[k-width-1])*magnitude[k-width-1];
            return float(std::max(0.,power));
        };
        auto initialPower = [&] {
            double power = 0;
            for (int j=0; j<width; ++j) power += double(magnitude[j])*magnitude[j];
            return power;
        };
        double energy = initialPower();
        double amplitude = 0;
        for (int j=0; j<width; ++j) amplitude += magnitude[j];
        int count = 0;
        for (int k=0; k<=nyquist; ++k) {
            envelope[k] = bandPower(k,energy);
            if (k+width<=nyquist) amplitude += magnitude[k+width];
            if (k>width) amplitude -= magnitude[k-width-1];
            // Broadband breath fills neighbouring bins. A resolved harmonic
            // concentrates the same power in fewer bins.
            const int bands = 1+std::min(nyquist,k+width)-std::max(0,k-width);
            const double occupancy = amplitude*amplitude/(bands*double(envelope[k])+1e-18);
            diffuseWeight[k] = airBand[k]*float(std::clamp((occupancy-0.35)/0.40,0.,1.));
            if (k>0 && k<nyquist && magnitude[k]>magnitude[k-1] && magnitude[k]>=magnitude[k+1])
                peaks[count++] = k;
        }
        int nearest = 0;
        for (int k=0; k<=nyquist; ++k) {
            const double angle = std::arg(bins[k]);
            const double expected = tau*hop*k/size;
            const double advance = expected + (first ? 0. : std::remainder(angle-state.phase[k]-expected,tau));
            state.phase[k] = angle;
            state.rotation[k] = first ? 0. : std::remainder(state.rotation[k]+(ratio-1)*advance,tau);
        }
        for (int k=0; k<=nyquist; ++k) {
            if (k==0) { output[0] += bins[0]; continue; }
            while (nearest+1<count && 2*k>peaks[nearest]+peaks[nearest+1]) ++nearest;
            const int pk = count ? peaks[nearest] : k;
            const int anchor = std::abs(k-pk)<=3 ? pk : k;
            const double displacement = (ratio-1)*anchor;
            const double dest = k+displacement;
            if (dest<=0 || dest>=nyquist) continue;
            const int lower = int(dest);
            const float fraction = float(dest-lower);
            // Interpolate the relocation about the window centre. Adjacent
            // FFT basis functions differ by pi there, hence the minus sign.
            const double rotation = state.rotation[anchor]+fraction*tau*0.5;
            const Complex component = bins[k]*std::polar(1.f,float(rotation));
            if (lower>0) output[lower] += (1.f-fraction)*component;
            if (lower+1<nyquist) output[lower+1] -= fraction*component;
        }
        for (int k=0; k<=nyquist; ++k) magnitude[k] = std::abs(output[k]);
        energy = initialPower();
        for (int k=0; k<=nyquist; ++k) {
            const float shiftedPower = bandPower(k,energy);
            // Limit boosts in spectral holes; never synthesise a missing harmonic.
            const float diffuse = diffuseWeight[k];
            const float gain = std::clamp(std::sqrt((envelope[k]+1e-12f)/(shiftedPower+1e-12f)),
                                          0.24f-0.18f*diffuse, 4.f-2.75f*diffuse);
            output[k] *= gain;
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
    if (target == 0. && pitch == 0. && formant == 1. && color == 1. && effectMix == 0.) {
        for (; i<count && position<end; ++i, position+=step) {
            left[i] = read(position,0);
            right[i] = read(position,1);
        }
        lastSpectralPercent = -1;
        return i;
    }
    const bool psolaOnly = engineBias <= -1.;
    if (psolaOnly) {
        spectralActive = false;
        spectralReturn = 0;
        spectralRetry = silentSpectrum = 0;
    }
    for (; i < count && position < end; ++i, position += step) {
        pitch += smoothing*(target-pitch);
        const double ratio = std::exp2(pitch);
        const double release = startNote >= limit ? std::clamp(double(guard*relax),0.,1.) : 0.;
        color += smoothing*(formant*std::pow(ratio/(formant*formant),release)-color);
        const size_t index = size_t(position)/wav->analysisHop;
        VocalFrame frame = index < wav->vocal.size() ? wav->vocal[index] : VocalFrame{};
        if (frame.period > 0 && index+1 <= size_t(end-1)/wav->analysisHop && index+1 < wav->vocal.size()) {
            const auto& next = wav->vocal[index+1];
            if (next.period > 0 && std::abs(next.period-frame.period) < 0.2f*frame.period) {
                const float t = float(position/wav->analysisHop - double(index));
                frame.period += t*(next.period-frame.period);
            }
        }
        const bool shifted = std::abs(pitch)>1e-5 || std::abs(color-1)>1e-5;
        double confidence = frame.voiced;
        if (index+1 <= size_t(end-1)/wav->analysisHop && index+1 < wav->vocal.size())
            confidence += (wav->vocal[index+1].voiced-confidence)*
                          (position/wav->analysisHop-double(index));
        // The slice is preanalysed: start yielding to STFT before pitch marks
        // disappear, and require sustained periodicity to bring PSOLA back.
        // Interpolate the lookahead as well so it has no 10 ms steps.
        const size_t lastFrame = size_t(end-1)/wav->analysisHop;
        if (index+2 <= lastFrame && index+2 < wav->vocal.size()) {
            const double t = position/wav->analysisHop-double(index);
            const double next = wav->vocal[index+1].voiced;
            const double ahead = next+t*(wav->vocal[index+2].voiced-next);
            confidence = std::min(confidence,std::min(next,ahead));
        }
        effectMix += smoothing*((shifted ? 1. : 0.)-effectMix);
        const bool process = shifted || effectMix>1e-5;
        if (!process) spectralActive = false;
        if (spectralRetry > 0) --spectralRetry;
        else if (!psolaOnly && process && (!spectralActive || spectralCursor%hop==0)) {
            try {
                if (!spectralActive) {
                    for (auto& channel : spectral) channel.overlap.fill(0);
                    spectralCursor = 0;
                    for (int offset=hop-size; offset<0; offset+=hop)
                        spectralFrame(offset,ratio,offset==hop-size);
                }
                spectralFrame(0,ratio,false);
                spectralActive = true;
            } catch (...) {
                recoverSpectrum();
            }
        }
        // TD-PSOLA: select a source pulse, place its waveform at the MIDI period.
        const double pulseRate = frame.period > 0 ? std::min(1.,step*ratio/frame.period) : 0.;
        if (pulseRate > 0 && shifted) pulseCountdown -= pulseRate;
        else pulseCountdown = 0;
        if (pulseCountdown <= 0 && shifted && pulseRate > 0 && used < int(grains.size())) {
            const double spacing = 1./pulseRate;
            const double radius = std::clamp(0.8*frame.period/(step*color), std::max(1.,0.65*spacing), spacing*12.);
            // The pulse is usually due between two output samples.
            const double age = std::min(1.,-pulseCountdown*spacing);
            const double center = position+(radius-age)*step;
            auto upper = std::lower_bound(wav->pulses.begin(),wav->pulses.end(),center);
            const bool hasUpper = upper != wav->pulses.end() && *upper >= begin && *upper < end;
            const bool hasLower = upper != wav->pulses.begin() && upper[-1] >= begin && upper[-1] < end;
            if ((hasLower && center-upper[-1] <= frame.period) || (hasUpper && *upper-center <= frame.period)) {
                // Morph adjacent aligned cycles instead of abruptly switching
                // their waveform halfway between marks. Never cross a slice
                // edge or morph across an unvoiced gap.
                double mark = hasLower ? upper[-1] : *upper;
                double distance = 0, blend = 0;
                if (hasLower && hasUpper) {
                    const double gap = *upper-mark;
                    if (gap >= 0.7*frame.period && gap <= 1.3*frame.period) {
                        distance = gap;
                        blend = (center-mark)/gap;
                    } else if (*upper-center < center-mark) {
                        mark = *upper;
                    }
                }
                grains[used++] = {mark+(age-radius)*step*color,step*color,
                                  age/(2*radius),0.5/radius,distance,blend,
                                  1./std::max(1.,std::min(0.20*frame.period,0.0015*wav->sampleRate))};
            }
            pulseCountdown = std::max(0.,pulseCountdown+1.);
        }
        double weight = 0, l = 0, r = 0;
        for (int g = 0; g < used;) {
            auto& grain = grains[g];
            const double w = 0.5-0.5*std::cos(6.283185307179586*grain.phase);
            auto accumulate = [&](double source, double amount) {
                if (source < begin || source >= end || amount <= 0) return;
                const double edge = std::min(source-begin,end-source)*grain.edgeFadeInverse;
                if (edge < 1.) amount *= edge*edge*(3.-2.*edge);
                weight += amount;
                l += amount*read(source,0);
                r += amount*read(source,1);
            };
            accumulate(grain.source,w*(1.-grain.cycleBlend));
            if (grain.cycleBlend > 0)
                accumulate(grain.source+grain.cycleDistance,w*grain.cycleBlend);
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
        double sl = psolaOnly ? dry[0] : spectral[0].overlap[spectralCursor];
        double sr = psolaOnly ? dry[1] : spectral[1].overlap[spectralCursor];
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
            pulseCountdown = 0;
            silentGrains = 0;
            grainPower = crossPower = 0;
            blendGain = 1;
            weight = l = r = 0;
        }
        if (spectralActive) spectralReturn += smoothing*(1.-spectralReturn);
        const double coverage = std::min(1.,weight*8.);
        // Compare the body of the actual STFT output with the source. Compare
        // stereo powers separately so an out-of-phase WAV cannot cancel here.
        const double inputs[] = {dry[0],dry[1],sl,sr};
        for (int channel=0; channel<4; ++channel)
            bodyLow[channel] += bodyFilter*(inputs[channel]-bodyLow[channel]);
        const double followBody = smoothing*0.25; // approximately 24 ms
        sourceBody += followBody*(bodyLow[0]*bodyLow[0]+bodyLow[1]*bodyLow[1]-sourceBody);
        spectralBody += followBody*(bodyLow[2]*bodyLow[2]+bodyLow[3]*bodyLow[3]-spectralBody);

        // Auto stays spectral until a reliable pitched source loses body on an
        // upward slide. A Schmitt trigger keeps borderline vowels on one engine.
        const double loss = std::clamp(1.-spectralBody/std::max(sourceBody,1e-10),0.,1.);
        const double upward = std::clamp(pitch*2.,0.,1.); // full sensitivity at +6 st
        const double evidence = confidence*upward*loss - engineBias;
        const bool usable = shifted && frame.period>0 && coverage>=1. && sourceBody>1e-9;
        // Start the handover while the final voiced grains still have window
        // coverage. On re-entry let the grain train fill before fading it in.
        const bool entersGap = index+1 <= lastFrame && index+1 < wav->vocal.size() &&
            wav->vocal[index+1].period <= 0 &&
            ((index+1)*wav->analysisHop-position)*fadeStep <= step;
        if (entersGap) preferGrains = false;
        else if (engineBias <= -1.) preferGrains = shifted && frame.period>0 && coverage>=1.;
        else if (!usable || engineBias >= 1.) preferGrains = false;
        else if (engineBias == 0.) preferGrains = confidence >= (preferGrains ? 0.62 : 0.80);
        else if (confidence < 0.62 || evidence < 0.24) preferGrains = false;
        else if (confidence >= 0.80 && evidence >= 0.48 && effectMix > 0.95)
            preferGrains = true;
        mix = preferGrains ? std::min(1.,mix+fadeStep) : std::max(0.,mix-fadeStep);
        const double fade = mix*mix*(3.-2.*mix);
        const bool automatic = engineBias == 0.;
        const double wet = psolaOnly ? coverage*fade :
            automatic ? fade+(1.-spectralReturn)*(1.-fade)*coverage :
                        coverage*(1.-spectralReturn*(1.-mix));
        const double norm = 1./std::max(1e-9,weight);
        double grain[2] = {l*norm,r*norm};
        for (int ch=0; ch<2; ++ch) {
            if (coverage >= 1.) lastCoveredGrain[ch] = grain[ch];
            else if (automatic) {
                // Missing marks may end the grain train before the fade ends.
                // Carry its last covered value through the tail of the ramp.
                grain[ch] = coverage*grain[ch]+(1.-coverage)*lastCoveredGrain[ch];
                if (mix == 0.) lastCoveredGrain[ch] = dry[ch];
            }
        }
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

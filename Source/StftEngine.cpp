#include "StftEngine.h"
#include <cmath>

StftVoice::StftVoice(std::shared_ptr<const WaveData> data, int first, int last, double rate)
    : wav(std::move(data)), begin(first), end(last), step(wav->sampleRate / rate),
      length(static_cast<long long>(std::ceil((last-first)/step))) {
    for (int i=0; i<N; ++i)
        window[i] = float(0.5 - 0.5 * std::cos(2 * std::acos(-1.0) * i / N));
    for (auto& processor : core) {
        processor = std::make_unique<stftpitchshift::StftPitchShiftCore<float>>(N,H,rate);
        processor->quefrency(0.001);
        processor->distortion(1.0);
    }
}

float StftVoice::read(double where, int ch) const {
    if (where < begin || where >= end) return 0;
    int a = int(where), b = std::min(a+1,end-1);
    float frac = float(where-a);
    return wav->stereo[size_t(a)*2+ch]*(1-frac) + wav->stereo[size_t(b)*2+ch]*frac;
}

void StftVoice::frame(long long start, double scale) {
    factor[0] = scale;
    for (int ch=0; ch<2; ++ch) {
        for (int i=0; i<N; ++i) time[i] = read(begin+(start+i)*step,ch)*window[i];
        fft[ch].fft(time,bins);
        core[ch]->factors(factor);
        core[ch]->shiftpitch(bins);
        bins.front() = bins.back() = 0;
        fft[ch].ifft(bins,time);
        for (int i=0; i<N; ++i) {
            auto offset = start+i-position;
            if (offset >= 0 && offset < N)
                overlap[ch][size_t(offset)] += time[i]*window[i]*(H/(0.375f*N));
        }
    }
}

int StftVoice::render(float* left, float* right, int count, double scale) {
    int frames = int(std::min<long long>(count,length-position));
    bool shifted = std::abs(scale-1.0)>0.00001;
    for (int i=0; i<frames; ++i, ++position) {
        if (shifted && !active) {
            for (int offset=-N+H; offset<=0; offset+=H) frame(position+offset,scale);
            active = true;
            cursor = 0;
        }
        if (active && cursor == H) {
            for (auto& ch : overlap) {
                std::move(ch.begin()+H,ch.end(),ch.begin());
                std::fill(ch.end()-H,ch.end(),0);
            }
            frame(position,scale);
            cursor = 0;
        }
        left[i] = shifted ? overlap[0][cursor] : read(begin+position*step,0);
        right[i] = shifted ? overlap[1][cursor] : read(begin+position*step,1);
        if (active) ++cursor;
    }
    return frames;
}

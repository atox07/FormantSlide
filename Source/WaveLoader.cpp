#include "WaveLoader.h"
#include "VocalFilter.h"
#include "ExcitationReader.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>

namespace {
struct PeriodCandidate {
    VocalFrame frame;
    float evidence = 0;
    double pathScore = 0;
    int previous = 0;
};
// Index zero is an unvoiced observation. Keep several local correlation
// maxima so a temporarily loud harmonic does not decide the pulse train.
using PeriodChoices = std::array<PeriodCandidate,7>;

void linkPeriods(PeriodChoices& current, const PeriodChoices* previous) {
    for (size_t i=0; i<current.size(); ++i) {
        auto& c = current[i];
        if (i && !c.frame.period) continue;
        const double local = i ? c.evidence : 0.56;
        c.pathScore = local;
        if (!previous) continue;
        double best = -std::numeric_limits<double>::infinity();
        for (size_t j=0; j<previous->size(); ++j) {
            const auto& p = (*previous)[j];
            if (j && !p.frame.period) continue;
            double cost = (i==0) != (j==0) ? 0.22 : 0.;
            if (i && j)
                cost = std::min(0.58,0.90*std::abs(std::log2(double(c.frame.period)/p.frame.period)));
            if (p.pathScore-cost > best) {
                best = p.pathScore-cost;
                c.previous = int(j);
            }
        }
        c.pathScore += best;
    }
}

// Estimate periodicity on a reduced-rate analysis channel. Pulse locations
// remain in original WAV coordinates and are shared by both stereo channels.
void analyzeVoice(WaveData& w) {
    const int stride = std::max(1, w.sampleRate / 12000);
    const double rate = double(w.sampleRate) / stride;
    std::vector<float> x;
    double power[2]{};
    for (size_t i = 0; i < w.stereo.size(); ++i)
        power[i % 2] += double(w.stereo[i]) * w.stereo[i];
    const int ch = power[1] > power[0];
    for (int i = 0; i < w.frames(); i += stride) {
        float sum = 0;
        const int count = std::min(stride, w.frames() - i);
        for (int j = 0; j < count; ++j) sum += w.stereo[2 * size_t(i+j) + ch];
        x.push_back(sum / count);
    }
    w.analysisHop = std::max(1, w.sampleRate / 100);
    const int maxLag = std::max(4, int(rate / 55));
    std::vector<double> score(maxLag + 1);
    std::vector<PeriodChoices> paths;
    paths.reserve((w.frames()-1)/w.analysisHop+1);
    for (int at = 0; at < w.frames(); at += w.analysisHop) {
        const int first = std::clamp(at / stride - maxLag, 0,
                                     std::max(0, int(x.size()) - 2*maxLag));
        const int count = std::min(maxLag, int(x.size()) - first - maxLag);
        PeriodChoices choices{};
        if (count >= maxLag / 2) {
            for (int lag = 1; lag <= maxLag; ++lag) {
                double diff = 0, energy = 1e-12;
                for (int j = first; j < first + count; j += 2) {
                    const double a = x[j], b = x[j+lag];
                    diff += (a-b)*(a-b);
                    energy += a*a+b*b;
                }
                score[lag] = energy > 1e-8 ? 1-diff/energy : 0;
            }
            const int lo = std::clamp(int(rate/2200), 2, maxLag-1);
            for (int lag = lo; lag < maxLag; ++lag) {
                if (score[lag] < 0.48 ||
                    score[lag] < score[lag-1] || score[lag] <= score[lag+1]) continue;
                const float evidence = float(score[lag]-0.018*std::log2(double(lag)/lo));
                if (evidence <= choices.back().evidence) continue;
                const double curvature = 2*score[lag]-score[lag-1]-score[lag+1];
                const double offset = 0.5*(score[lag+1]-score[lag-1])/std::max(curvature,1e-9);
                PeriodCandidate candidate;
                candidate.frame.period = float((lag+offset)*stride);
                candidate.frame.voiced = float(std::clamp((score[lag]-0.55)/0.30,0.0,1.0));
                candidate.evidence = evidence;
                for (size_t i=1; i<choices.size(); ++i)
                    if (candidate.evidence > choices[i].evidence) std::swap(candidate,choices[i]);
            }
        }
        linkPeriods(choices,paths.empty() ? nullptr : &paths.back());
        paths.push_back(choices);
    }
    w.vocal.resize(paths.size());
    int selected = 0;
    const auto& last = paths.back();
    for (size_t i=1; i<last.size(); ++i)
        if (last[i].frame.period && last[i].pathScore>last[selected].pathScore) selected=int(i);
    for (size_t i=paths.size(); i-- > 0;) {
        const auto& choice = paths[i][selected];
        w.vocal[i] = choice.frame;
        selected = choice.previous;
    }
    double lastPulse = -1;
    std::vector<double> reference;
    for (double pos = 0; pos < w.frames();) {
        const auto f = w.vocal[int(pos)/w.analysisHop];
        if (f.period < 2) { lastPulse = -1; pos += w.analysisHop; continue; }
        const int radius = std::max(1,int(f.period*0.15));
        const int lo = std::max(w.pulses.empty()?0:int(w.pulses.back())+1,int(pos)-radius);
        const int hi = std::min(w.frames()-1,int(pos)+radius);
        if (lo > hi) break;
        int peak = lo;
        const int half = std::max(2, int(0.35*f.period));
        double refinement = 0;
        if (lastPulse >= half && hi+half < w.frames() && lo >= half) {
            // Match a short waveform neighbourhood across cycles. Two nearby
            // maxima can trade amplitude while the underlying phase is stable.
            const int previousSample = int(lastPulse);
            const double fraction = lastPulse-previousSample;
            const int sampleStep = std::max(1,half/24);
            reference.clear();
            double sumA = 0;
            for (int j=-half; j<=half; j+=sampleStep) {
                const auto i = 2*size_t(previousSample+j)+ch;
                const double a = w.stereo[i]+fraction*(w.stereo[i+2]-w.stereo[i]);
                reference.push_back(a);
                sumA += a;
            }
            const double meanA = sumA/reference.size();
            double varianceA = 0;
            for (double& a : reference) {
                a -= meanA;
                varianceA += a*a;
            }
            auto similarity = [&](int candidate) {
                double cross = 0, energyB = 0, sumB = 0;
                int j = candidate-half;
                for (double a : reference) {
                    const double b = w.stereo[2*size_t(j)+ch];
                    cross += a*b;
                    energyB += b*b;
                    sumB += b;
                    j += sampleStep;
                }
                const double distance = (candidate-pos)/radius;
                const double varianceB = std::max(0.,energyB-sumB*sumB/reference.size());
                return cross/std::sqrt(varianceA*varianceB+1e-24)
                       - 0.035*distance*distance;
            };
            double best = -2;
            for (int candidate = lo; candidate <= hi; ++candidate) {
                const double value = similarity(candidate);
                if (value > best) { best = value; peak = candidate; }
            }
            if (peak > lo && peak < hi) {
                const double left = similarity(peak-1), right = similarity(peak+1);
                refinement = std::clamp(0.5*(right-left)/std::max(2*best-left-right,1e-9),-0.5,0.5);
            }
        } else {
            for (int i = lo+1; i <= hi; ++i)
                if (w.stereo[2*size_t(i)+ch] > w.stereo[2*size_t(peak)+ch]) peak=i;
        }
        lastPulse = peak+refinement;
        w.pulses.push_back(lastPulse);
        pos = lastPulse+f.period;
    }
}
// Fit a broad, regularised vocal-tract envelope. Lag damping keeps the
// predictor from modelling individual harmonics as narrow vowel resonances.
void analyseEnvelope(WaveData& w) {
    ExcitationReader::prepare();
    const int order = std::clamp(w.sampleRate/700+2,12,maxVocalOrder);
    w.filterOrder = order;
    w.filters.resize(w.vocal.size());
    const int half = std::max(order,w.sampleRate/80);
    std::vector<double> windowed(2*size_t(2*half+order));
    for (size_t frame=0; frame<w.filters.size(); ++frame) {
        const int centre = int(frame)*w.analysisHop;
        std::array<double,maxVocalOrder+1> correlation{}, predictor{};
        const int last = std::min(w.frames(),centre+half);
        const int first = std::max(0,centre-half);
        for (int n=first; n<last; ++n) {
            const double taper = 1.-std::abs(n-centre)/double(half);
            for (int ch=0; ch<2; ++ch)
                windowed[2*(n-first+order)+ch] = w.stereo[2*size_t(n)+ch]*taper;
        }
        const double period = w.vocal[frame].period;
        // Do not fit the repeated glottal pulse as part of the vocal tract.
        const int used = period>0 ? std::clamp(int(0.8*period),8,order) : order;
        for (int j=0; j<=used; ++j) {
            const double* a = windowed.data()+2*order;
            const double* b = a-2*j;
            for (int i=0; i<2*(last-first); ++i) correlation[j] += a[i]*b[i];
        }
        if (correlation[0]<1e-10) continue;
        const double bandwidth = std::max(45., period>0 ? 0.3*w.sampleRate/period : 90.);
        double error = 1.0005*correlation[0];
        for (int m=1; m<=used; ++m) {
            const double lag = 6.283185307179586*bandwidth*m/w.sampleRate;
            correlation[m] *= std::exp(-0.5*lag*lag);
            double cross = correlation[m];
            for (int j=1; j<m; ++j) cross += predictor[j]*correlation[m-j];
            const double reflection = std::clamp(-cross/error,-0.998,0.998);
            const auto old = predictor;
            for (int j=1; j<m; ++j) predictor[j] += reflection*old[m-j];
            predictor[m] = reflection;
            w.filters[frame][m-1] = float(reflection);
            error = std::max(error*(1.-reflection*reflection),correlation[0]*1e-7);
        }
    }
    VocalLattice lattice[2];
    w.excitation.resize(w.stereo.size());
    for (int n=0; n<w.frames(); ++n) {
        const auto k = filterAt(w,n);
        for (int ch=0; ch<2; ++ch) {
            const size_t i = 2*size_t(n)+ch;
            w.excitation[i] = float(lattice[ch].analyse(w.stereo[i],k,order));
        }
    }
}
uint16_t u16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t u32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
                                        (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }
float sampleAt(const uint8_t* p, int bits, int format) {
    if (format == 3 && bits == 32) {
        float result = 0.0f;
        std::memcpy(&result, p, 4);
        return std::isfinite(result) ? std::clamp(result, -8.0f, 8.0f) : 0.0f;
    }
    if (format != 1) return 0;
    if (bits == 8) return (float(p[0]) - 128.f) / 128.f;
    if (bits == 16) return float(int16_t(u16(p))) / 32768.f;
    if (bits == 24) {
        int32_t n = int32_t(p[0]) | (int32_t(p[1]) << 8) | (int32_t(p[2]) << 16);
        if (n & 0x800000) n |= ~0xffffff;
        return float(n) / 8388608.f;
    }
    if (bits == 32) { int32_t n = int32_t(u32(p)); return double(n) / 2147483648.0; }
    return 0;
}
}

bool readWav(const std::wstring& path, WaveData& result, std::string& error) {
    std::ifstream input(std::filesystem::path(path), std::ios::binary);
    if (!input) { error = "Cannot open WAV"; return false; }
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)), {});
    if (bytes.size() < 44 || bytes.size() > size_t(800) * 1024 * 1024 ||
        std::memcmp(bytes.data(), "RIFF", 4) || std::memcmp(bytes.data() + 8, "WAVE", 4)) {
        error = "Not a RIFF/WAVE file"; return false;
    }
    size_t pos = 12, dataStart = 0, dataSize = 0;
    int format = 0, channels = 0, sampleRate = 0, bits = 0, blockAlign = 0;
    std::vector<uint32_t> cues;
    while (pos + 8 <= bytes.size()) {
        const uint32_t chunkSize = u32(bytes.data() + pos + 4);
        const size_t begin = pos + 8;
        if (chunkSize > bytes.size() - begin) { error = "Broken WAV chunk"; return false; }
        if (!std::memcmp(bytes.data() + pos, "fmt ", 4) && chunkSize >= 16) {
            const uint8_t* p = bytes.data() + begin;
            format = u16(p); channels = u16(p + 2); sampleRate = int(u32(p + 4));
            blockAlign = u16(p + 12); bits = u16(p + 14);
            if (format == 0xfffe && chunkSize >= 40) format = u16(p + 24);
        }
        if (!std::memcmp(bytes.data() + pos, "data", 4)) { dataStart = begin; dataSize = chunkSize; }
        if (!std::memcmp(bytes.data() + pos, "cue ", 4) && chunkSize >= 4) {
            const auto* chunk = bytes.data() + begin;
            const auto count = u32(chunk);
            if (count <= (chunkSize - 4) / 24) {
                for (uint32_t i = 0; i < count; ++i) {
                    const auto* cue = chunk + 4 + size_t(i) * 24;
                    if (!std::memcmp(cue + 8, "data", 4))
                        cues.push_back(u32(cue + 20));
                }
            }
        }
        pos = begin + chunkSize + (chunkSize & 1);
    }
    if ((format != 1 && format != 3) || channels < 1 || channels > 8 ||
        sampleRate < 8000 || sampleRate > 384000 ||
        (bits != 8 && bits != 16 && bits != 24 && bits != 32) ||
        (format == 3 && bits != 32) || !blockAlign || !dataSize ||
        blockAlign < channels * (bits / 8)) {
        error = "Unsupported WAV (use PCM 16/24/32 or 32-bit float)"; return false;
    }
    const size_t frames = dataSize / size_t(blockAlign);
    if (frames < 2 || frames > size_t(sampleRate) * 600) {
        error = "WAV must be between 2 frames and 10 minutes"; return false;
    }
    WaveData tmp;
    for (const auto offset : cues)
        if (offset < frames - 1) tmp.cueStarts.push_back(int(offset));
    std::sort(tmp.cueStarts.begin(), tmp.cueStarts.end());
    tmp.cueStarts.erase(std::unique(tmp.cueStarts.begin(), tmp.cueStarts.end()), tmp.cueStarts.end());
    tmp.sampleRate = sampleRate;
    tmp.stereo.resize(frames * 2);
    for (size_t i = 0; i < frames; ++i) {
        const uint8_t* p = bytes.data() + dataStart + i * size_t(blockAlign);
        const float l = sampleAt(p, bits, format);
        const float r = channels > 1 ? sampleAt(p + bits / 8, bits, format) : l;
        tmp.stereo[i * 2] = l;
        tmp.stereo[i * 2 + 1] = r;
    }
    analyzeVoice(tmp);
    analyseEnvelope(tmp);
    result = std::move(tmp);
    error.clear();
    return true;
}

std::vector<AudioSlice> sliceWav(const WaveData& wav, bool equal, int divisions) {
    std::vector<AudioSlice> slices;
    const int frames = wav.frames();
    if (frames < 2) return slices;
    if (equal) {
        const int n = std::clamp(divisions, 1, std::min(64, frames / 2));
        for (int i = 0; i < n; ++i)
            slices.push_back({int(int64_t(i) * frames / n), int(int64_t(i + 1) * frames / n)});
        return slices;
    }
    if (!wav.cueStarts.empty()) {
        for (size_t i = 0; i < wav.cueStarts.size(); ++i) {
            const int end = i + 1 < wav.cueStarts.size() ? wav.cueStarts[i + 1] : frames;
            if (end - wav.cueStarts[i] >= 2) slices.push_back({wav.cueStarts[i], end});
        }
        return slices;
    }
    float maxPeak = 0;
    for (float v : wav.stereo) maxPeak = std::max(maxPeak, std::abs(v));
    const float threshold = std::max(0.0007f, maxPeak * 0.017f);
    const int window = std::max(1, int(wav.sampleRate * 0.003));
    const int gap = std::max(1, int(wav.sampleRate * 0.028));
    const int minSlice = std::max(2, int(wav.sampleRate * 0.042));
    const int pad = std::max(0, int(wav.sampleRate * 0.003));
    int start = -1, last = 0;
    for (int at = 0; at < frames; at += window) {
        const int end = std::min(frames, at + window);
        float peak = 0;
        for (int j = at; j < end; ++j) {
            peak = std::max(peak, std::abs(wav.stereo[size_t(j) * 2]));
            peak = std::max(peak, std::abs(wav.stereo[size_t(j) * 2 + 1]));
        }
        if (peak >= threshold) {
            if (start < 0) start = at;
            last = end;
        } else if (start >= 0 && at - last >= gap) {
            if (last - start >= minSlice)
                slices.push_back({std::max(0, start - pad), std::min(frames, last + pad)});
            start = -1;
        }
    }
    if (start >= 0 && last - start >= minSlice)
        slices.push_back({std::max(0, start - pad), std::min(frames, last + pad)});
    if (slices.empty()) slices.push_back({0, frames});
    // A chromatic WAV may have no silent gaps at all. Never silently map
    // an entire multi-note chromatic onto a single slice: use configurable
    // equal divisions as an initial guess (drag the boundaries to correct).
    // There is no duration threshold: short chromatics need this too.
    if (slices.size() == 1 && divisions > 1 && frames >= divisions * 2) {
        return sliceWav(wav, true, divisions);
    }
    if (slices.size() > 64) slices.resize(64);
    return slices;
}

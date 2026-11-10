#include "FormantSlideNative.h"
#include "WinEditor.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <filesystem>

namespace {
char kName[] = "FormantSlide";
char kShort[] = "FormantSlide";
TFruityPlugInfo kInfo = {CurrentSDKVersion, kName, kShort,
                          FPF_Type_FullGen, 0, 1, 0};
struct SavedState {
    uint32_t magic = 0x46534e32; // FSN2
    uint32_t version = 1;
    uint32_t equal = 0, slices = 12, root = 60, all = 1, glide = 250;
    int32_t offset = 0;
    wchar_t wav[512]{};
    wchar_t midi[512]{};
};
float decodeFloat(intptr_t value) {
    const uint32_t bits = static_cast<uint32_t>(value);
    float result = 0;
    std::memcpy(&result, &bits, sizeof(result));
    return result;
}
int voiceNote(PVoiceParams p) {
    // FL's pitch value is in cents relative to middle C (MIDI 60).
    return std::clamp(60 + int(std::lround(double(p->InitLevels.Pitch) / 100.0)), 0, 127);
}
}

extern "C" __declspec(dllexport) TFruityPlug* _stdcall
CreatePlugInstance(TFruityPlugHost* host, int tag) {
    return new FormantSlideNative(tag, host);
}

FormantSlideNative::FormantSlideNative(intptr_t tag, TFruityPlugHost* host)
    : TCPPFruityPlug(tag, host, nullptr) {
    Info = &kInfo;
    initStretch();
    PlugHost->Dispatcher(HostTag, FHD_SetLatency, 0, stretch_.outputLatency());
}
FormantSlideNative::~FormantSlideNative() = default;

void FormantSlideNative::initStretch() {
    // Larger than the old 1024-frame spectral window; improves formant stability
    // at the cost of increased latency. May still be imperfect on quick slides.
    stretch_.configure(2, 2048, 256, false);
    stretch_.setFormantBase(180.0f / float(sampleRate_));
    stretch_.setFormantFactor(1.0f, true);
    stretch_.setTransposeSemitones(0.0f);
    stretch_.reset();
    drainRemaining_ = 0;
}

void _stdcall FormantSlideNative::DestroyObject() {
    if (editor_) { editor_->close(); delete editor_; editor_ = nullptr; }
    TCPPFruityPlug::DestroyObject();
}

intptr_t _stdcall FormantSlideNative::Dispatcher(intptr_t id, intptr_t index, intptr_t value) {
    if (id == FPD_ShowEditor) {
        if (!editor_) editor_ = new NativeEditor(*this);
        if (value) editor_->show(reinterpret_cast<HWND>(value));
        else editor_->hide();
        EditorHandle = editor_->handle();
        return 0;
    }
    if (id == FPD_SetSampleRate && value >= 8000 && value <= 384000) {
        int latency = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            sampleRate_ = double(value);
            AudioRenderer.setSmpRate(int(value));
            initStretch();
            samplePlaying_ = false;
            latency = stretch_.outputLatency();
        }
        PlugHost->Dispatcher(HostTag, FHD_SetLatency, 0, latency);
        return 0;
    }
    if (id == FPD_SetSamplesPerTick) {
        const float v = decodeFloat(value);
        if (std::isfinite(v) && v > 0.01f && v < 500000.f)
            samplesPerTick_ = v;
        return 0;
    }
    if (id == FPD_SetTimeSig && value) {
        const auto* ts = reinterpret_cast<const TTimeSigInfo*>(value);
        if (ts->PPQ >= 24 && ts->PPQ <= 9600) hostPpq_ = ts->PPQ;
        return 0;
    }
    if (id == FPD_Flush || id == FPD_SongPosChanged) {
        std::lock_guard<std::mutex> lock(mutex_);
        samplePlaying_ = false;
        customRemaining_ = drainRemaining_ = 0;
        customPitch_ = customTarget_ = previousHostSlide_ = 0;
        stretch_.reset();
        return 0;
    }
    if (id == FPD_LoadFile && value) {
        const char* path = reinterpret_cast<const char*>(value);
        if (path) {
            // ANSI path is supplied by legacy SDK callback.
            const int n = MultiByteToWideChar(CP_ACP, 0, path, -1, nullptr, 0);
            if (n > 0) {
                std::wstring wide(size_t(n), wchar_t(0));
                MultiByteToWideChar(CP_ACP, 0, path, -1, wide.data(), n);
                wide.resize(size_t(n - 1));
                if (std::filesystem::path(wide).extension() == L".wav") {
                    std::string error;
                    return loadWav(wide, error) ? 1 : 0;
                }
            }
        }
    }
    return TCPPFruityPlug::Dispatcher(id, index, value);
}

int _stdcall FormantSlideNative::ProcessEvent(int eventId, int eventValue, int flags) {
    if (eventId == FPE_Tempo) {
        const float newTempo = decodeFloat(eventValue);
        if (std::isfinite(newTempo) && newTempo > 1.f && newTempo < 1000.f)
            bpm_.store(newTempo);
    }
    return TCPPFruityPlug::ProcessEvent(eventId, eventValue, flags);
}

void _stdcall FormantSlideNative::GetName(int section, int index, int, char* name) {
    if (!name) return;
    name[0] = '\0';
    if (section == FPN_VoiceColor && index >= 0 && index < 16) {
        if (index == 0 || index == 1) strcpy_s(name, 256, "Vocal note");
        else strcpy_s(name, 256, "Pitch slide");
    }
}

void FormantSlideNative::beginVocal(Voice& voice) {
    current_ = &voice;
    customPitch_ = customTarget_ = 0;
    previousHostSlide_ = 0;
    customRemaining_ = 0;
    stretch_.reset();
    const int count = int(slices_.size());
    if (!count || !wav_.frames()) { samplePlaying_ = false; return; }
    const int index = std::clamp(voice.note - settings_.firstNote, 0, count - 1);
    voice.baseOffset = double(voice.note - (settings_.firstNote + index));
    playhead_ = slices_[size_t(index)].begin;
    sliceEnd_ = slices_[size_t(index)].end;
    samplePlaying_ = true;
    drainRemaining_ = stretch_.outputLatency() + 2048;
}

void FormantSlideNative::beginSlide(const Voice& slide, int sdkLength, double hostTick) {
    if (!current_ || !samplePlaying_) return;
    int ticks = sdkLength;
    const int ppq = hostPpq_.load();
    if (ticks <= 0 || ticks > ppq * 256)
        ticks = midi_.durationTicks(slide.color + 1, slide.note, hostTick,
                                    ppq, settings_.midiOffsetTicks);
    int frames = 0;
    if (ticks > 0) {
        const float samplesPerTick = samplesPerTick_.load();
        if (samplesPerTick > 0)
            frames = int(std::lround(double(ticks) * samplesPerTick));
        else
            frames = int(std::lround(double(ticks) * sampleRate_ * 60.0 /
                         (double(bpm_.load()) * double(ppq))));
    }
    if (frames <= 0)
        frames = int(std::lround(sampleRate_ * settings_.fallbackGlideMs / 1000.0));
    frames = std::clamp(frames, 1, int(sampleRate_ * 20));
    // Absolute destination is determined by slide pitch, NOT duration.
    customTarget_ = double(slide.note) - double(current_->note);
    customRemaining_ = frames;
    customStep_ = (customTarget_ - customPitch_) / double(frames);
}

TVoiceHandle _stdcall FormantSlideNative::TriggerVoice(PVoiceParams params, intptr_t tag) {
    if (!params) return FVH_Null;
    int color = PlugHost->Voice_ProcessEvent(tag, FPV_GetColor, 0, 0);
    if (color < 0 || color > 15) color = 0;
    const int length = PlugHost->Voice_ProcessEvent(tag, FPV_GetLength, 0, 0);
    const double tick = PlugHost->GetSongMixingTime_A();
    auto entry = std::make_unique<Voice>();
    entry->hostTag = tag;
    entry->params = params;
    entry->note = voiceNote(params);
    entry->startPitchCents = params->InitLevels.Pitch;
    entry->color = color;
    entry->vocal = color <= 1;
    Voice* ptr = entry.get();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const bool slide = settings_.allSlideColors ? color >= 2 : color == 4;
        voices_.push_back(std::move(entry));
        if (ptr->vocal) beginVocal(*ptr);
        else if (slide) beginSlide(*ptr, length, tick);
    }
    return reinterpret_cast<TVoiceHandle>(ptr);
}

void _stdcall FormantSlideNative::Voice_Release(TVoiceHandle handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& v : voices_) {
        if (reinterpret_cast<TVoiceHandle>(v.get()) != handle) continue;
        v->released = true;
        if (v.get() == current_) samplePlaying_ = false;
        // A slide Note Off does NOT change the destination pitch. Its duration
        // was used when the ramp started; the final pitch remains held.
        break;
    }
}
void _stdcall FormantSlideNative::Voice_Kill(TVoiceHandle handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = voices_.begin(); it != voices_.end(); ++it) {
        if (reinterpret_cast<TVoiceHandle>(it->get()) != handle) continue;
        if (it->get() == current_) { current_ = nullptr; samplePlaying_ = false; }
        voices_.erase(it);
        break;
    }
}

void FormantSlideNative::render(PWAV32FS buffer, int length) {
    if (!buffer || length <= 0) return;
    const float* sources[] = {in_[0].data(), in_[1].data()};
    float* targets[] = {out_[0].data(), out_[1].data()};
    // FL computes native note slide pitch before each render block. Interpolate
    // the host values across this block to avoid control-rate stepping.
    const double nextHostSlide = current_ && current_->params
        ? (double(current_->params->FinalLevels.Pitch) - current_->startPitchCents) / 100.0 : 0.0;
    const double hostStart = previousHostSlide_;
    for (int pos = 0; pos < length; pos += 64) {
        const int run = std::min(64, length - pos);
        for (int i = 0; i < run; ++i) {
            if (customRemaining_ > 0) {
                customPitch_ += customStep_;
                if (--customRemaining_ == 0) customPitch_ = customTarget_;
            }
            float left = 0, right = 0;
            if (samplePlaying_ && wav_.frames() > 1 && playhead_ + 1 < sliceEnd_) {
                const int idx = int(playhead_);
                const float frac = float(playhead_ - idx);
                const float fade = std::clamp(float(sliceEnd_ - playhead_) /
                                std::max(8.0f, float(wav_.sampleRate) * 0.005f), 0.f, 1.f);
                left = (wav_.stereo[size_t(idx) * 2] * (1.f - frac) +
                        wav_.stereo[size_t(idx + 1) * 2] * frac) * fade;
                right = (wav_.stereo[size_t(idx) * 2 + 1] * (1.f - frac) +
                         wav_.stereo[size_t(idx + 1) * 2 + 1] * frac) * fade;
                playhead_ += double(wav_.sampleRate) / sampleRate_;
            } else {
                samplePlaying_ = false;
            }
            in_[0][size_t(i)] = left;
            in_[1][size_t(i)] = right;
            if (!samplePlaying_ && drainRemaining_ > 0) --drainRemaining_;
        }
        if ((current_ && samplePlaying_) || drainRemaining_ > 0) {
            const double interpolation = double(pos + run) / double(length);
            const double hostPitch = hostStart + (nextHostSlide - hostStart) * interpolation;
            const double semitones = hostPitch + (current_ ? current_->baseOffset : 0.0) + customPitch_;
            stretch_.setTransposeSemitones(float(std::clamp(semitones, -48.0, 48.0)));
            stretch_.process(sources, run, targets, run);
            float vol = 1.f, pan = 0.f;
            if (current_ && current_->params) {
                vol = std::max(0.f, current_->params->FinalLevels.Vol);
                pan = std::clamp(current_->params->FinalLevels.Pan, -1.f, 1.f);
            }
            for (int i = 0; i < run; ++i) {
                buffer[pos + i][0] += out_[0][size_t(i)] * vol * (pan > 0 ? 1.f - pan : 1.f);
                buffer[pos + i][1] += out_[1][size_t(i)] * vol * (pan < 0 ? 1.f + pan : 1.f);
            }
        }
    }
    previousHostSlide_ = nextHostSlide;
}
void _stdcall FormantSlideNative::Gen_Render(PWAV32FS buffer, int& length) {
    if (length <= 0) return;
    // Never wait for a GUI file operation in the audio thread.
    std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
    if (lock.owns_lock()) render(buffer, length);
}

bool FormantSlideNative::loadWav(const std::wstring& path, std::string& error) {
    WaveData wav;
    if (!readWav(path, wav, error)) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    wav_ = std::move(wav);
    wavPath_ = path;
    slices_ = sliceWav(wav_, settings_.equalSlices, settings_.sliceCount);
    samplePlaying_ = false;
    stretch_.reset();
    status_ = "WAV loaded";
    return true;
}
bool FormantSlideNative::loadMidi(const std::wstring& path, std::string& error) {
    MidiFile midi;
    if (!midi.load(path, error)) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    midi_ = std::move(midi);
    midiPath_ = path;
    status_ = "MIDI duration fallback loaded";
    return true;
}
void FormantSlideNative::updateSettings(const Settings& input) {
    std::lock_guard<std::mutex> lock(mutex_);
    settings_ = input;
    settings_.sliceCount = std::clamp(settings_.sliceCount, 1, 64);
    settings_.firstNote = std::clamp(settings_.firstNote, 0, 127);
    settings_.fallbackGlideMs = std::clamp(settings_.fallbackGlideMs, 1, 10000);
    slices_ = sliceWav(wav_, settings_.equalSlices, settings_.sliceCount);
    samplePlaying_ = false;
}
FormantSlideNative::Display FormantSlideNative::display() const {
    std::lock_guard<std::mutex> lock(mutex_);
    Display d;
    d.settings = settings_;
    d.slices = int(slices_.size());
    d.midiNotes = int(midi_.noteCount());
    if (!wavPath_.empty()) d.wavName = std::filesystem::path(wavPath_).filename().wstring();
    if (!midiPath_.empty()) d.midiName = std::filesystem::path(midiPath_).filename().wstring();
    d.status = status_;
    return d;
}

void _stdcall FormantSlideNative::SaveRestoreState(IStream* stream, BOOL save) {
    if (!stream) return;
    SavedState s;
    if (save) {
        std::lock_guard<std::mutex> lock(mutex_);
        s.equal = settings_.equalSlices;
        s.slices = settings_.sliceCount;
        s.root = settings_.firstNote;
        s.all = settings_.allSlideColors;
        s.glide = settings_.fallbackGlideMs;
        s.offset = settings_.midiOffsetTicks;
        wcsncpy_s(s.wav, wavPath_.c_str(), _TRUNCATE);
        wcsncpy_s(s.midi, midiPath_.c_str(), _TRUNCATE);
        stream->Write(&s, sizeof(s), nullptr);
    } else {
        ULONG read = 0;
        if (FAILED(stream->Read(&s, sizeof(s), &read)) || read != sizeof(s) ||
            s.magic != 0x46534e32 || s.version != 1) return;
        Settings restored;
        restored.equalSlices = s.equal != 0;
        restored.sliceCount = int(s.slices);
        restored.firstNote = int(s.root);
        restored.allSlideColors = s.all != 0;
        restored.fallbackGlideMs = int(s.glide);
        restored.midiOffsetTicks = s.offset;
        updateSettings(restored);
        std::string error;
        if (s.wav[0]) loadWav(s.wav, error);
        if (s.midi[0]) loadMidi(s.midi, error);
    }
}

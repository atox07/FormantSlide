#include "FormantSlideNative.h"
#include "WinEditor.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstddef>
#include <cwchar>
#include <filesystem>

namespace {
char kName[] = "FormantSlide";
char kShort[] = "FormantSlide";
TFruityPlugInfo kInfo = {CurrentSDKVersion, kName, kShort,
                          FPF_Type_FullGen, 0, 0, 0};
struct SavedState {
    uint32_t magic = 0x46534e32; // FSN2
    uint32_t version = 3;
    uint32_t equal = 0, slices = 12, root = 60, all = 1, glide = 250; // root retained for loading older projects
    int32_t offset = 0;
    wchar_t wav[512]{};
    wchar_t midi[512]{};
    int32_t noteOffset = 0; // appended so old project data stays compatible
};
struct SavedSliceMarkerHeader {
    uint32_t magic = 0x534c4335; // SLC5
    uint32_t count = 0;
};
float decodeFloat(intptr_t value) {
    const uint32_t bits = static_cast<uint32_t>(value);
    float result = 0;
    std::memcpy(&result, &bits, sizeof(result));
    return result;
}
int voiceNote(double cents) {
    return 60 + int(std::lround(std::clamp(cents, -6000.0, 6700.0) / 100.0));
}
}

extern "C" __declspec(dllexport) TFruityPlug* _stdcall
CreatePlugInstance(TFruityPlugHost* host, TPluginTag tag) {
    return new FormantSlideNative(tag, host);
}

FormantSlideNative::FormantSlideNative(intptr_t tag, TFruityPlugHost* host)
    : TCPPFruityPlug(tag, host, nullptr) {
    Info = &kInfo;
    PlugHost->Dispatcher(HostTag, FHD_SetLatency, 0, 0);
}
FormantSlideNative::~FormantSlideNative() = default;

void FormantSlideNative::initStretch(Voice& voice) {
    // Match the v0.2 VST3 engine settings to keep its familiar response.
    if (spareStretchers_.empty()) {
        constexpr int options = Stretcher::OptionEngineFiner | Stretcher::OptionProcessRealTime |
            Stretcher::OptionPitchHighConsistency | Stretcher::OptionFormantPreserved |
            Stretcher::OptionChannelsTogether | Stretcher::OptionWindowShort | Stretcher::OptionThreadingNever;
        voice.stretch_ = std::make_unique<Stretcher>(size_t(sampleRate_), 2, options);
        voice.stretch_->setMaxProcessSize(64);
    } else {
        voice.stretch_ = std::move(spareStretchers_.back());
        spareStretchers_.pop_back();
    }
    voice.engineRate = sampleRate_;
    voice.stretch_->reset();
    voice.drainRemaining_ = 0;
}

void _stdcall FormantSlideNative::DestroyObject() {
    if (editor_) { editor_->close(); delete editor_; editor_ = nullptr; }
    EditorHandle = nullptr;
    TCPPFruityPlug::DestroyObject();
}

intptr_t _stdcall FormantSlideNative::Dispatcher(intptr_t id, intptr_t index, intptr_t value) {
    if (id == FPD_ShowEditor) {
        // The native host checks EditorHandle after this callback. Only publish
        // a real, valid HWND; never leave a stale handle after editor teardown.
        if (value) {
            const auto parent = reinterpret_cast<HWND>(value);
            if (!IsWindow(parent)) { EditorHandle = nullptr; return 0; }
            if (!editor_) editor_ = new NativeEditor(*this);
            EditorHandle = editor_->show(parent);
            if (!IsWindow(EditorHandle)) EditorHandle = nullptr;
        } else {
            if (editor_) editor_->hide();
            EditorHandle = nullptr; // Never expose a hidden/stale HWND to the host.
        }
        return 0;
    }
    if (id == FPD_SetSampleRate && value >= 8000 && value <= 384000) {
        int latency = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            spareStretchers_.clear();
            sampleRate_ = double(value);
            AudioRenderer.setSmpRate(int(value));
            stopVoices();
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
        stopVoices();
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

void FormantSlideNative::stopVoices() {
    current_ = nullptr;
    for (auto& voice : voices_) {
        voice->samplePlaying_ = false;
        voice->drainRemaining_ = 0;
    }
}

void FormantSlideNative::beginVocal(Voice& voice) {
    current_ = &voice;
    initStretch(voice);
    voice.customPitch_ = voice.customTarget_ = 0;
    voice.previousHostSlide_ = 0;
    voice.customRemaining_ = 0;
    const int count = int(slices_.size());
    if (!count || !wav_.frames()) { voice.samplePlaying_ = false; return; }
    // Match the working v0.2 VST3: a fixed root defines slice position.
    const int index = std::clamp(voice.note - settings_.firstNote, 0, count - 1);
    voice.baseOffset = 60.0 + voice.startPitchCents / 100.0 - (settings_.firstNote + index);
    lastMidiNote_ = voice.note;
    voice.sliceIndex = index;
    lastSlice_ = index;
    lastPitchCents_ = voice.startPitchCents;
    voice.playhead_ = slices_[size_t(index)].begin;
    voice.sliceEnd_ = slices_[size_t(index)].end;
    voice.samplePlaying_ = true;
    voice.stretch_->setPitchScale(std::exp2(std::clamp(voice.baseOffset, -48.0, 48.0) / 12.0));
    float silence[64]{};
    const float* pad[] = {silence, silence};
    size_t padding = voice.stretch_->getPreferredStartPad();
    while (padding) {
        const size_t run = std::min<size_t>(64, padding);
        voice.stretch_->process(pad, run, false);
        padding -= run;
    }
    TWAV32FS discard[64]{};
    int pending = int(voice.stretch_->getStartDelay());
    voice.drainRemaining_ = 1; // Output may remain after final input.
    while (pending > 0) {
        const int run = std::min(64, pending);
        render(voice, discard, run);
        pending -= run;
    }
}

void FormantSlideNative::beginSlide(const Voice& slide, int sdkLength, double hostTick) {
    if (!current_ || current_->released || (!current_->samplePlaying_ && !current_->drainRemaining_)) return;
    int ticks = sdkLength;
    const int ppq = hostPpq_.load();
    if (ticks <= 0 || ticks > ppq * 256)
        ticks = midi_.durationTicks(slide.color + 1, slide.note, hostTick,
                                    ppq, settings_.midiOffsetTicks);
    double duration = sampleRate_ * settings_.fallbackGlideMs / 1000.0;
    if (ticks > 0) {
        const double hostSamples = samplesPerTick_.load();
        const double perTick = hostSamples > 0 ? hostSamples
            : sampleRate_ * 60.0 / (double(bpm_.load()) * ppq);
        duration = ticks * perTick;
    }
    const int frames = int(std::lround(std::clamp(duration, 1.0, sampleRate_ * 20)));
    // Absolute destination is determined by slide pitch, NOT duration.
    const double liveCents = current_->params->FinalLevels.Pitch;
    if (!std::isfinite(liveCents)) return;
    current_->customTarget_ = (slide.startPitchCents - liveCents) / 100.0;
    current_->customRemaining_ = frames;
    current_->customStep_ = (current_->customTarget_ - current_->customPitch_) / double(frames);
}

TVoiceHandle _stdcall FormantSlideNative::TriggerVoice(PVoiceParams params, intptr_t tag) {
    if (!params) return FVH_Null;
    int color = PlugHost->Voice_ProcessEvent(tag, FPV_GetColor, 0, 0);
    if (color < 0 || color > 15) color = 0;
    const int length = PlugHost->Voice_ProcessEvent(tag, FPV_GetLength, 0, 0);
    const double tick = PlugHost->GetSongMixingTime_A();
    auto entry = std::make_unique<Voice>();
    Voice& voice = *entry;
    voice.hostTag = tag;
    voice.params = params;
    const float pitch = params->FinalLevels.Pitch;
    if (!std::isfinite(pitch)) return FVH_Null;
    voice.note = voiceNote(pitch);
    voice.startPitchCents = pitch;
    voice.color = color;
    voice.vocal = color <= 1;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!voice.vocal) {
            const bool slide = settings_.allSlideColors || color == 4;
            if (slide) beginSlide(voice, length, tick);
            return FVH_Null; // Control event consumed; FL has no audio voice to release.
        }
        voices_.push_back(std::move(entry));
        beginVocal(*voices_.back());
        return reinterpret_cast<TVoiceHandle>(current_);
    }
}

void _stdcall FormantSlideNative::Voice_Release(TVoiceHandle handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& v : voices_) {
        if (reinterpret_cast<TVoiceHandle>(v.get()) != handle) continue;
        v->released = true;
        v->releaseFrames = 64;
        break;
    }
}
void _stdcall FormantSlideNative::Voice_Kill(TVoiceHandle handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = voices_.begin(); it != voices_.end(); ++it) {
        if (reinterpret_cast<TVoiceHandle>(it->get()) != handle) continue;
        if (it->get() == current_) current_ = nullptr;
        if ((*it)->engineRate == sampleRate_ && spareStretchers_.size() < 16) spareStretchers_.push_back(std::move((*it)->stretch_));
        voices_.erase(it);
        break;
    }
}

void FormantSlideNative::render(Voice& voice, PWAV32FS buffer, int length) {
    if (!buffer || length <= 0) return;
    const float* sources[] = {in_[0].data(), in_[1].data()};
    float* targets[] = {out_[0].data(), out_[1].data()};
    // FL computes native note slide pitch before each render block. Interpolate
    // the host values across this block to avoid control-rate stepping.
    const double nextHostSlide = std::isfinite(voice.params->FinalLevels.Pitch)
        ? (double(voice.params->FinalLevels.Pitch) - voice.startPitchCents) / 100.0 : 0.0;
    const double hostStart = voice.previousHostSlide_;
    for (int pos = 0; pos < length; pos += 64) {
        const int run = std::min(64, length - pos);
        for (int i = 0; i < run; ++i) {
            if (voice.customRemaining_ > 0) {
                voice.customPitch_ += voice.customStep_;
                if (--voice.customRemaining_ == 0) voice.customPitch_ = voice.customTarget_;
            }
        }
        if ((voice.samplePlaying_) || voice.drainRemaining_ > 0) {
            const double interpolation = double(pos + run) / double(length);
            const double hostPitch = hostStart + (nextHostSlide - hostStart) * interpolation;
            const double semitones = hostPitch + voice.baseOffset + voice.customPitch_;
            voice.stretch_->setPitchScale(std::exp2(std::clamp(semitones, -48.0, 48.0) / 12.0));
            while (voice.stretch_->available() < run && voice.samplePlaying_) {
                int count = 0;
                while (count < 64 && voice.playhead_ + 1 < voice.sliceEnd_) {
                    const int idx = int(voice.playhead_);
                    const float frac = float(voice.playhead_ - idx);
                    const float fade = std::clamp(float(voice.sliceEnd_ - voice.playhead_) /
                                    std::max(8.0f, float(wav_.sampleRate) * 0.005f), 0.f, 1.f);
                    const float left = (wav_.stereo[size_t(idx) * 2] * (1.f - frac) +
                            wav_.stereo[size_t(idx + 1) * 2] * frac) * fade;
                    const float right = (wav_.stereo[size_t(idx) * 2 + 1] * (1.f - frac) +
                             wav_.stereo[size_t(idx + 1) * 2 + 1] * frac) * fade;
                    voice.playhead_ += double(wav_.sampleRate) / sampleRate_;
                    in_[0][count] = left;
                    in_[1][count++] = right;
                }
                voice.samplePlaying_ = voice.playhead_ + 1 < voice.sliceEnd_;
                voice.stretch_->process(sources, count, !voice.samplePlaying_);
            }
            const int frames = int(voice.stretch_->retrieve(targets, run));
            if (!voice.samplePlaying_ && voice.stretch_->available() <= 0)
                voice.drainRemaining_ = 0;
            float vol = 1.f, pan = 0.f;
            if (voice.params) {
                vol = std::max(0.f, voice.params->FinalLevels.Vol);
                pan = std::clamp(voice.params->FinalLevels.Pan, -1.f, 1.f);
            }
            for (int i = 0; i < frames; ++i) {
                const float gain = voice.releaseFrames < 0 ? 1.f : float(voice.releaseFrames) / 64.f;
                if (voice.releaseFrames > 0) --voice.releaseFrames;
                buffer[pos + i][0] += out_[0][size_t(i)] * gain * vol * (pan > 0 ? 1.f - pan : 1.f);
                buffer[pos + i][1] += out_[1][size_t(i)] * gain * vol * (pan < 0 ? 1.f + pan : 1.f);
            }
        }
    }
    voice.previousHostSlide_ = nextHostSlide;
}
void _stdcall FormantSlideNative::Gen_Render(PWAV32FS buffer, int& length) {
    if (!buffer || length <= 0) { length = 0; return; }
    // Never wait for a GUI file operation in the audio thread.
    std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock()) { length = 0; return; }
    bool rendered = false;
    for (auto& v : voices_) {
        if ((v->samplePlaying_ || v->drainRemaining_ > 0) && v->releaseFrames != 0) {
            render(*v, buffer, length);
            rendered = true;
        }
    }
    if (!rendered) length = 0;
    // Released handles belong to the host until acknowledged. Host callbacks
    // can re-enter this plugin, so discard the local handle before notifying FL.
    for (;;) {
        const auto done = std::find_if(voices_.begin(), voices_.end(), [this](const auto& v) {
            return v->released && (v->releaseFrames == 0 || (!v->samplePlaying_ && !v->drainRemaining_));
        });
        if (done == voices_.end()) break;
        const auto tag = (*done)->hostTag;
        if (done->get() == current_) current_ = nullptr;
        if ((*done)->engineRate == sampleRate_ && spareStretchers_.size() < 16) spareStretchers_.push_back(std::move((*done)->stretch_));
        voices_.erase(done);
        lock.unlock();
        PlugHost->Voice_Kill(tag, FALSE);
        lock.lock();
    }
}

bool FormantSlideNative::loadWav(const std::wstring& path, std::string& error) {
    WaveData wav;
    if (!readWav(path, wav, error)) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    wav_ = std::move(wav);
    wavPath_ = path;
    slices_ = sliceWav(wav_, settings_.equalSlices, settings_.sliceCount);
    slicesManuallyEdited_ = false;
    lastMidiNote_ = lastSlice_ = -1;
    stopVoices();
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
    const auto previous = settings_;
    settings_ = input;
    settings_.sliceCount = std::clamp(settings_.sliceCount, 1, 64);
    settings_.firstNote = std::clamp(settings_.firstNote, 0, 127);
    settings_.fallbackGlideMs = std::clamp(settings_.fallbackGlideMs, 1, 10000);
    // Do not rebuild/silence a playing sample just because glide or pitch
    // calibration has been adjusted with a slider.
    if (settings_.equalSlices != previous.equalSlices || settings_.sliceCount != previous.sliceCount) {
        slices_ = sliceWav(wav_, settings_.equalSlices, settings_.sliceCount);
        slicesManuallyEdited_ = false;
        lastMidiNote_ = lastSlice_ = -1;
        stopVoices();
    }
}
FormantSlideNative::Display FormantSlideNative::display() const {
    std::lock_guard<std::mutex> lock(mutex_);
    Display d;
    d.settings = settings_;
    d.slices = int(slices_.size());
    d.midiNotes = int(midi_.noteCount());
    if (!wavPath_.empty()) d.wavName = std::filesystem::path(wavPath_).filename().wstring();
    d.wavFullPath = wavPath_;
    if (!midiPath_.empty()) d.midiName = std::filesystem::path(midiPath_).filename().wstring();
    d.midiFullPath = midiPath_;
    d.status = status_;
    d.rootMidi = settings_.firstNote;
    d.lastMidiNote = lastMidiNote_;
    d.lastSlice = lastSlice_;
    d.lastPitchCents = lastPitchCents_;
    return d;
}

std::vector<AudioSlice> FormantSlideNative::sliceSnapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return slices_;
}
bool FormantSlideNative::moveSliceBoundary(int boundaryIndex, int frame) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (boundaryIndex < 1 || boundaryIndex >= int(slices_.size())) return false;
    auto& left = slices_[size_t(boundaryIndex - 1)];
    auto& right = slices_[size_t(boundaryIndex)];
    // Keep a minimum audible region on either side, and never cross markers.
    const int minGap = std::max(2, wav_.sampleRate / 200); // 5 ms
    const int low = left.begin + minGap;
    const int high = right.end - minGap;
    if (low > high) return false;
    const int boundary = std::clamp(frame, low, high);
    left.end = boundary;
    right.begin = boundary;
    slicesManuallyEdited_ = true;
    // Current playback finishes with its original end until the next note.
    return true;
}

std::vector<int> FormantSlideNative::playingSlices() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<int> active;
    for (const auto& voice : voices_)
        if (voice->releaseFrames != 0 && (voice->samplePlaying_ || voice->drainRemaining_ > 0))
            active.push_back(voice->sliceIndex);
    return active;
}

int FormantSlideNative::wavFrameCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return wav_.frames();
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
        SavedSliceMarkerHeader markerHeader;
        markerHeader.count = slicesManuallyEdited_ ? uint32_t(slices_.size()) : 0;
        stream->Write(&markerHeader, sizeof(markerHeader), nullptr);
        if (markerHeader.count > 0) {
            for (const auto& slice : slices_) {
                const int32_t frames[2] = {slice.begin, slice.end};
                stream->Write(frames, sizeof(frames), nullptr);
            }
        }
    } else {
        ULONG read = 0;
        if (FAILED(stream->Read(&s, sizeof(s), &read)) ||
            read < offsetof(SavedState, noteOffset) ||
            s.magic != 0x46534e32 || (s.version < 1 || s.version > 3)) return;
        Settings restored;
        restored.equalSlices = s.equal != 0;
        restored.sliceCount = int(s.slices);
        restored.allSlideColors = s.all != 0;
        restored.fallbackGlideMs = int(s.glide);
        restored.midiOffsetTicks = s.offset;
        restored.firstNote = std::clamp(int(s.root), 0, 127);
        if (s.version == 2 && read == sizeof(s))
            restored.firstNote = std::clamp(restored.firstNote - std::clamp(s.noteOffset, -24, 24), 0, 127);
        updateSettings(restored);
        std::string error;
        if (s.wav[0]) loadWav(s.wav, error);
        if (s.midi[0]) loadMidi(s.midi, error);
        SavedSliceMarkerHeader markerHeader{};
        ULONG headerRead = 0;
        if (SUCCEEDED(stream->Read(&markerHeader, sizeof(markerHeader), &headerRead)) &&
            headerRead == sizeof(markerHeader) && markerHeader.magic == 0x534c4335 &&
            markerHeader.count >= 2 && markerHeader.count <= uint32_t(wavFrameCount() / 2)) {
            std::vector<AudioSlice> restoredSlices;
            for (uint32_t i = 0; i < markerHeader.count; ++i) {
                int32_t bounds[2]{};
                ULONG got = 0;
                if (FAILED(stream->Read(bounds, sizeof(bounds), &got)) || got != sizeof(bounds)) {
                    restoredSlices.clear(); break;
                }
                restoredSlices.push_back({bounds[0], bounds[1]});
            }
            std::lock_guard<std::mutex> lock(mutex_);
            bool valid = restoredSlices.size() == markerHeader.count && wav_.frames() > 0;
            for (size_t i = 0; valid && i < restoredSlices.size(); ++i) {
                valid = restoredSlices[i].begin >= 0 &&
                        restoredSlices[i].end <= wav_.frames() &&
                        restoredSlices[i].begin < restoredSlices[i].end;
                if (i > 0) valid = valid && restoredSlices[i-1].end <= restoredSlices[i].begin;
            }
            if (valid) { slices_ = std::move(restoredSlices); slicesManuallyEdited_ = true; }
        }
    }
}

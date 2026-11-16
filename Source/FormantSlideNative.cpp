#include "FormantSlideNative.h"
#include "StftDevParams.h"
#include "WinEditor.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstddef>
#include <cwchar>
#include <filesystem>
#include <exception>
#include <atomic>
#include <mutex>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <limits>
#include <locale>
#include <sstream>
#include <thread>

namespace {

constexpr int slotForColor(int color) { return color % 4; }
char kName[] = "FormantSlide";
char kShort[] = "FormantSlide";
TFruityPlugInfo kInfo = {CurrentSDKVersion, kName, kShort,
                          FPF_Type_FullGen, 0, 0, 0};
struct SavedState {
    uint32_t magic = 0x46534e32; // FSN2
    uint32_t version = 4;
    uint32_t equal = 0, slices = 12, root = 60, all = 1, glide = 250; // root retained for loading older projects
    int32_t offset = 0;
    wchar_t wav[512]{};
    wchar_t midi[512]{};
    int32_t pitchOrLegacyOffset = 0; // v2: key calibration in semitones; v4: audio transpose in cents
};
struct SavedFormantGuard {
    uint32_t magic = 0x46474431; // FGD1
    float guard = 1.0f, limitHz = 2000.0f, relaxMax = 0.75f;
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
FormantSlideNative::~FormantSlideNative() {}

void _stdcall FormantSlideNative::DestroyObject() {
    if (editor_) { editor_->close(); delete editor_; editor_ = nullptr; }
    EditorHandle = nullptr;
    TCPPFruityPlug::DestroyObject();
}

intptr_t _stdcall FormantSlideNative::Dispatcher(intptr_t id, intptr_t index, intptr_t value) {
    if (id == FPD_ShowEditor) {
        // The native host checks EditorHandle after this callback. Only publish
        // a real, valid HWND; never leave a stale handle after editor teardown.
        if (!value) {
            if (editor_) editor_->hide();
            EditorHandle = nullptr;
            return 0;
        }
        const auto parent = reinterpret_cast<HWND>(value);
        if (!IsWindow(parent)) {
            EditorHandle = nullptr;
            return 0;
        }
        if (!editor_) editor_ = new NativeEditor(*this);
        EditorHandle = editor_->show(parent);
        if (!IsWindow(EditorHandle)) EditorHandle = nullptr;
        return 0;
    }
    if (id == FPD_SetSampleRate && value >= 8000 && value <= 384000) {
        int latency = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
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
    if (eventId == FPE_MIDI_Pitch) channelPitchCents_.store(eventValue);
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
        sprintf_s(name, 256, "Slot %c", 'A'+slotForColor(index));
    }
}

void FormantSlideNative::stopVoices() {
    for (auto& voice : voices_) {
        voice->samplePlaying_ = false;
        voice->drainRemaining_ = 0;
    }
}

void FormantSlideNative::beginVocal(Voice& voice) {
    auto& slot = slots_[slotForColor(voice.color)];
    voice.previousHostSlide_ = 0;
    const int count = int(slot.slices_.size());
    if (!count || !slot.wav_->frames()) { voice.samplePlaying_ = false; return; }
    // Match the working v0.2 VST3: a fixed root defines slice position.
    const int index = std::clamp(voice.note - slot.settings_.firstNote, 0, count - 1);
    voice.baseOffset = 60.0 + voice.startPitchCents / 100.0 - (slot.settings_.firstNote + index);
    // FL includes static tuning in the initial voice pitch. Keep small
    // offsets in sample playback, leaving subsequent native slides to DSP.
    voice.fineTune = std::abs(voice.baseOffset) <= 0.5 ? voice.baseOffset : 0.;
    voice.sourceStep = slot.wav_->sampleRate / sampleRate_ * std::exp2(voice.fineTune / 12.);
    slot.lastMidiNote_ = voice.note;
    voice.sliceIndex = index;
    slot.lastSlice_ = index;
    slot.lastPitchCents_ = voice.startPitchCents;
    voice.playhead_ = slot.slices_[size_t(index)].begin;
    voice.sliceEnd_ = slot.slices_[size_t(index)].end;
    voice.samplePlaying_ = true;
    // FL reports cents relative to middle C (900 cents below A4).
    const double startNoteHz = 440.0 * std::exp2(
        (voice.startPitchCents + slot.settings_.pitchCents - 900.0) / 1200.0);
    voice.engine_ = std::make_unique<StftVoice>(
        slot.wav_, int(voice.playhead_), voice.sliceEnd_, sampleRate_, startNoteHz, voice.sourceStep);
    voice.drainRemaining_ = 0;
}

TVoiceHandle _stdcall FormantSlideNative::TriggerVoice(PVoiceParams params, intptr_t tag) {
    if (!params) return FVH_Null;
    int color = PlugHost->Voice_ProcessEvent(tag, FPV_GetColor, 0, 0);
    if (color < 0 || color > 15) color = 0;
    auto entry = std::make_unique<Voice>();
    Voice& voice = *entry;
    voice.hostTag = tag;
    voice.params = params;
    const float pitch = params->FinalLevels.Pitch;
    if (!std::isfinite(pitch)) return FVH_Null;
    // FL supplies the sounding pitch relative to middle C. Remove only channel tuning for the key lookup.
    voice.note = voiceNote(double(pitch) - channelPitchCents_.load());
    voice.startPitchCents = pitch;
    voice.color = color;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!slots_[slotForColor(color)].wav_->frames()) return FVH_Null;
        try {
            beginVocal(voice);
            voices_.push_back(std::move(entry));
        } catch (...) {
            return FVH_Null;
        }
        return reinterpret_cast<TVoiceHandle>(&voice);
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
        voices_.erase(it);
        break;
    }
}

void FormantSlideNative::render(Voice& voice, PWAV32FS buffer, int length) {
    if (!buffer || length <= 0 || !voice.params || !voice.engine_) return;
    const auto& slot = slots_[slotForColor(voice.color)];
    // I parametri dev arrivano dagli slider (StftDevParams): li legge StftVoice::render().
    // Il knob sposta pitch E formante di tutto il canale; slide e note diverse
    // cambiano solo il pitch, quindi il formante resta fisso al valore del knob.
    // (1.0 = il formante segue il knob per intero; 0.5 = meta', 0 = knob solo pitch)
    constexpr double kKnobFormantAmount = 1.0;
    voice.engine_->setEngineBias(slot.settings_.engineBias);
    voice.engine_->setFormantScale(std::exp2(kKnobFormantAmount * slot.settings_.pitchCents / 1200.0));
    {
        auto& dp = controls_;
        voice.engine_->setFormantGuard(dp.formantGuard.load());
        voice.engine_->setHighNoteLimit(dp.highNoteLimitHz.load());
        voice.engine_->setHighNoteRelaxMax(dp.highNoteRelaxMax.load());
    }
    const double nextHostSlide = std::isfinite(voice.params->FinalLevels.Pitch)
        ? (double(voice.params->FinalLevels.Pitch) - voice.startPitchCents) / 100.0 : 0.0;
    const double hostStart = voice.previousHostSlide_;
    for (int pos = 0; pos < length && voice.samplePlaying_; pos += 64) {
        const int run = std::min(64, length - pos);
        const double hostPitch = hostStart + (nextHostSlide - hostStart) * double(pos + run) / length;
        const double shift = std::clamp(hostPitch + voice.baseOffset + slot.settings_.pitchCents / 100.0, -48.0, 48.0);
        voice.shiftCents = int(std::lround(shift * 100.0));
        const int frames = voice.engine_->render(out_[0].data(), out_[1].data(), run, std::exp2((shift - voice.fineTune) / 12.0));
        const float vol = std::max(0.f, voice.params->FinalLevels.Vol);
        const float pan = std::clamp(voice.params->FinalLevels.Pan, -1.f, 1.f);
        for (int i = 0; i < frames; ++i) {
            const float gain = voice.releaseFrames < 0 ? 1.f : float(voice.releaseFrames) / 64.f;
            if (voice.releaseFrames > 0) --voice.releaseFrames;
            const float fade = std::clamp(float(voice.sliceEnd_ - voice.playhead_) /
                std::max(8.0f, float(slot.wav_->sampleRate) * 0.005f), 0.f, 1.f);
            const float sample = out_[0][size_t(i)] * gain * fade * vol;
            buffer[pos + i][0] += sample * (pan > 0 ? 1.f - pan : 1.f);
            buffer[pos + i][1] += out_[1][size_t(i)] * gain * fade * vol * (pan < 0 ? 1.f + pan : 1.f);
            voice.playhead_ += voice.sourceStep;
        }
        if (frames < run || voice.playhead_ >= voice.sliceEnd_) voice.samplePlaying_ = false;
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
            try {
                render(*v, buffer, length);
                rendered = true;
            } catch (...) {
                // An engine failure must not escape an audio callback into FL.
                v->samplePlaying_ = false;
                v->drainRemaining_ = 0;
                v->releaseFrames = 0;
                v->released = true;
                v->engine_.reset();
            }
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
        // Keep the old handle's address alive until FL has acknowledged it.
        // A reentrant TriggerVoice must never reuse this address for a new note.
        auto retired = std::move(*done);
        const auto tag = retired->hostTag;
        voices_.erase(done);
        lock.unlock();
        PlugHost->Voice_Kill(tag, FALSE);
        lock.lock();
    }
}

void FormantSlideNative::checkpointLocked(int index) {
    auto& dp = controls_;
    if (history_.size() >= 32) history_.erase(history_.begin());
    history_.push_back({slots_[index],index,{dp.formantGuard.load(),dp.highNoteLimitHz.load(),dp.highNoteRelaxMax.load()}});
}
void FormantSlideNative::checkpoint() {
    std::lock_guard<std::mutex> lock(mutex_);
    checkpointLocked(selectedSlot());
}
bool FormantSlideNative::undo() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (history_.empty()) return false;
    auto saved = std::move(history_.back());
    history_.pop_back();
    auto& slot = slots_[saved.index];
    if (slot.wav_ != saved.slot.wav_)
        for (auto& v : voices_) if (slotForColor(v->color)==saved.index) v->samplePlaying_=false;
    slot = std::move(saved.slot);
    selectSlot(saved.index);
    auto& dp = controls_;
    dp.formantGuard.store(saved.guard[0]);
    dp.highNoteLimitHz.store(saved.guard[1]);
    dp.highNoteRelaxMax.store(saved.guard[2]);
    return true;
}
bool FormantSlideNative::loadWav(const std::wstring& path, std::string& error, int slotIndex, bool remember) {
    const int index = slotIndex < 0 ? selectedSlot() : std::clamp(slotIndex,0,3);
    auto wav = std::make_shared<WaveData>();
    if (!readWav(path,*wav,error)) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    auto& slot = slots_[index];
    auto slices = sliceWav(*wav,slot.settings_.equalSlices,slot.settings_.sliceCount);
    if (remember) checkpointLocked(index);
    for (auto& v : voices_) if (slotForColor(v->color)==index) v->samplePlaying_=false;
    slot.wav_ = std::move(wav);
    slot.wavPath_ = path;
    slot.slices_ = std::move(slices);
    slot.slicesManuallyEdited_ = false;
    slot.lastMidiNote_ = slot.lastSlice_ = -1;
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
    auto& slot = slots_[selectedSlot()];
    const auto previous = slot.settings_;
    slot.settings_ = input;
    slot.settings_.engineBias = std::clamp(slot.settings_.engineBias, -100, 100);
    slot.settings_.pitchCents = std::clamp(slot.settings_.pitchCents, -2400, 2400);
    slot.settings_.sliceCount = std::clamp(slot.settings_.sliceCount, 1, 64);
    slot.settings_.firstNote = std::clamp(slot.settings_.firstNote, 0, 127);
    slot.settings_.fallbackGlideMs = std::clamp(slot.settings_.fallbackGlideMs, 1, 10000);
    // Do not rebuild/silence a playing sample just because glide or pitch
    // calibration has been adjusted with a slider.
    if (slot.settings_.equalSlices != previous.equalSlices || slot.settings_.sliceCount != previous.sliceCount) {
        slot.slices_ = sliceWav(*slot.wav_, slot.settings_.equalSlices, slot.settings_.sliceCount);
        slot.slicesManuallyEdited_ = false;
        slot.lastMidiNote_ = slot.lastSlice_ = -1;
        for (auto& v : voices_) if (slotForColor(v->color)==selectedSlot()) v->samplePlaying_=false;
    }
}
FormantSlideNative::Display FormantSlideNative::display() const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& slot = slots_[selectedSlot()];
    Display d;
    d.settings = slot.settings_;
    d.slices = int(slot.slices_.size());
    d.midiNotes = int(midi_.noteCount());
    if (!slot.wavPath_.empty()) d.wavName = std::filesystem::path(slot.wavPath_).filename().wstring();
    d.wavFullPath = slot.wavPath_;
    if (!midiPath_.empty()) d.midiName = std::filesystem::path(midiPath_).filename().wstring();
    d.midiFullPath = midiPath_;
    d.status = status_;
    d.rootMidi = slot.settings_.firstNote;
    d.lastMidiNote = slot.lastMidiNote_;
    d.lastSlice = slot.lastSlice_;
    d.lastPitchCents = slot.lastPitchCents_;
    for (auto it = voices_.rbegin(); it != voices_.rend(); ++it) {
        const auto& voice = **it;
        if (slotForColor(voice.color) == selectedSlot() && voice.samplePlaying_ && voice.engine_) {
            d.voiceActive = true;
            d.shiftCents = voice.shiftCents;
            d.lastMidiNote = voice.note;
            d.lastSlice = voice.sliceIndex;
            d.lastPitchCents = voice.startPitchCents;
            d.spectralPercent = voice.engine_->spectralPercent();
            d.spectralRecovery = voice.engine_->retryingSpectrum();
            break;
        }
    }
    return d;
}

std::vector<AudioSlice> FormantSlideNative::sliceSnapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& slot = slots_[selectedSlot()];
    return slot.slices_;
}
bool FormantSlideNative::moveSliceBoundary(int boundaryIndex, int frame) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& slot = slots_[selectedSlot()];
    if (boundaryIndex < 1 || boundaryIndex >= int(slot.slices_.size())) return false;
    auto& left = slot.slices_[size_t(boundaryIndex - 1)];
    auto& right = slot.slices_[size_t(boundaryIndex)];
    // Keep a minimum audible region on either side, and never cross markers.
    const int minGap = std::max(2, slot.wav_->sampleRate / 200); // 5 ms
    const int low = left.begin + minGap;
    const int high = right.end - minGap;
    if (low > high) return false;
    const int boundary = std::clamp(frame, low, high);
    left.end = boundary;
    right.begin = boundary;
    slot.slicesManuallyEdited_ = true;
    // Current playback finishes with its original end until the next note.
    return true;
}

std::vector<int> FormantSlideNative::playingSlices() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<int> active;
    for (const auto& voice : voices_)
        if (slotForColor(voice->color)==selectedSlot() && voice->releaseFrames != 0 && (voice->samplePlaying_ || voice->drainRemaining_ > 0))
            active.push_back(voice->sliceIndex);
    return active;
}

int FormantSlideNative::wavFrameCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& slot = slots_[selectedSlot()];
    return slot.wav_->frames();
}

void _stdcall FormantSlideNative::SaveRestoreState(IStream* stream, BOOL save) {
    if (!stream) return;
    int records = save ? 4 : 1;
    int selection = selectedSlot();
    for (int index=0; index<records; ++index) {
        auto& slot = slots_[index];
        SavedState s;
        if (save) {
            std::lock_guard<std::mutex> lock(mutex_);
            s.version = 7;
            s.equal = slot.settings_.equalSlices;
            s.slices = slot.settings_.sliceCount;
            s.root = slot.settings_.firstNote;
            s.all = uint32_t(slot.settings_.engineBias + 100);
            s.glide = slot.settings_.fallbackGlideMs;
            s.pitchOrLegacyOffset = slot.settings_.pitchCents;
            s.offset = slot.settings_.midiOffsetTicks;
            wcsncpy_s(s.wav, slot.wavPath_.c_str(), _TRUNCATE);
            wcsncpy_s(s.midi, midiPath_.c_str(), _TRUNCATE);
            stream->Write(&s, sizeof(s), nullptr);
            SavedSliceMarkerHeader markerHeader;
            markerHeader.count = slot.slicesManuallyEdited_ ? uint32_t(slot.slices_.size()) : 0;
            stream->Write(&markerHeader, sizeof(markerHeader), nullptr);
            if (markerHeader.count > 0) {
                for (const auto& slice : slot.slices_) {
                    const int32_t frames[2] = {slice.begin, slice.end};
                    stream->Write(frames, sizeof(frames), nullptr);
                }
            }
            SavedFormantGuard guardBlock;
            guardBlock.guard = controls_.formantGuard.load();
            guardBlock.limitHz = controls_.highNoteLimitHz.load();
            guardBlock.relaxMax = controls_.highNoteRelaxMax.load();
            stream->Write(&guardBlock, sizeof(guardBlock), nullptr);
        } else {
            ULONG read = 0;
            if (FAILED(stream->Read(&s, sizeof(s), &read)) ||
                read < offsetof(SavedState, pitchOrLegacyOffset) ||
                s.magic != 0x46534e32 || (s.version < 1 || s.version > 7)) return;
            if (index==0) {
                std::lock_guard<std::mutex> lock(mutex_);
                const StftDevParams defaults;
                controls_.formantGuard.store(defaults.formantGuard.load());
                controls_.highNoteLimitHz.store(defaults.highNoteLimitHz.load());
                controls_.highNoteRelaxMax.store(defaults.highNoteRelaxMax.load());
                stopVoices(); slots_ = {}; history_.clear(); selection=0;
                midi_ = {}; midiPath_.clear();
            }
            if (s.version>=5) records=4;
            Settings restored;
            if (s.version >= 7) restored.engineBias = int(std::min(s.all, uint32_t(200))) - 100;
            restored.equalSlices = s.equal != 0;
            restored.sliceCount = int(s.slices);
            restored.fallbackGlideMs = int(s.glide);
            restored.midiOffsetTicks = s.offset;
            restored.firstNote = std::clamp(int(s.root), 0, 127);
            if (s.version == 2 && read == sizeof(s))
                restored.firstNote = std::clamp(restored.firstNote - std::clamp(s.pitchOrLegacyOffset, -24, 24), 0, 127);
            if (s.version >= 4 && read == sizeof(s)) restored.pitchCents = s.pitchOrLegacyOffset;
            s.wav[511] = L'\0';
            s.midi[511] = L'\0';
            restored.sliceCount = std::clamp(restored.sliceCount,1,64);
            { std::lock_guard<std::mutex> lock(mutex_); slot.settings_ = restored; }
            std::string error;
            if (s.wav[0]) loadWav(s.wav, error,index,false);
            if (index==0 && s.midi[0]) loadMidi(s.midi, error);
            SavedSliceMarkerHeader markerHeader{};
            ULONG headerRead = 0;
            if (SUCCEEDED(stream->Read(&markerHeader, sizeof(markerHeader), &headerRead)) &&
                headerRead == sizeof(markerHeader) && markerHeader.magic == 0x534c4335 &&
                markerHeader.count > 0 && markerHeader.count <= 65536) {
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
                bool valid = restoredSlices.size() == markerHeader.count && slot.wav_->frames() > 0;
                for (size_t i = 0; valid && i < restoredSlices.size(); ++i) {
                    valid = restoredSlices[i].begin >= 0 &&
                            restoredSlices[i].end <= slot.wav_->frames() &&
                            restoredSlices[i].begin < restoredSlices[i].end;
                    if (i > 0) valid = valid && restoredSlices[i-1].end <= restoredSlices[i].begin;
                }
                if (valid) { slot.slices_ = std::move(restoredSlices); slot.slicesManuallyEdited_ = true; }
            }
            // Blocco opzionale (assente nei progetti vecchi: restano i default).
            SavedFormantGuard guardBlock{};
            ULONG guardRead = 0;
            if (SUCCEEDED(stream->Read(&guardBlock, sizeof(guardBlock), &guardRead)) &&
                guardRead == sizeof(guardBlock) && guardBlock.magic == 0x46474431) {
                auto& dp = controls_;
                if (std::isfinite(guardBlock.guard))
                    dp.formantGuard.store(std::clamp(guardBlock.guard, 0.0f, 2.0f));
                if (std::isfinite(guardBlock.limitHz))
                    dp.highNoteLimitHz.store(std::clamp(guardBlock.limitHz, 500.0f, 4000.0f));
                if (std::isfinite(guardBlock.relaxMax))
                    dp.highNoteRelaxMax.store(std::clamp(guardBlock.relaxMax, 0.0f, 1.0f));
            }
        }
    }
    if (save) stream->Write(&selection,sizeof(selection),nullptr);
    else {
        if (records==4) stream->Read(&selection,sizeof(selection),nullptr);
        selectSlot(selection);
    }
}

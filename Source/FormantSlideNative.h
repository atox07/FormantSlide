#pragma once
#include "fp_cplug.h"
#include "MidiFile.h"
#include "WaveLoader.h"
#include "StftEngine.h"
#include "StftDevParams.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class NativeEditor;

class FormantSlideNative final : public TCPPFruityPlug {
public:
    struct Settings {
        bool equalSlices = false;
        int sliceCount = 12;
        int firstNote = 60; // MIDI key assigned to slice 1
        int fallbackGlideMs = 250;
        int pitchCents = 0;
        int engineBias = 0; // -100 PSOLA, 0 automatic, +100 STFT
        int midiOffsetTicks = 0;
    };
    struct Display {
        Settings settings;
        int slices = 0;
        int spectralPercent = -1;
        bool spectralRecovery = false;
        bool voiceActive = false;
        int shiftCents = 0;
        int midiNotes = 0;
        std::wstring wavName, midiName;
        std::wstring wavFullPath, midiFullPath;
        std::string status;
        int rootMidi = 60, lastMidiNote = -1, lastSlice = -1;
        float lastPitchCents = 0.f;
    };

    FormantSlideNative(intptr_t tag, TFruityPlugHost* host);
    ~FormantSlideNative() override;
    void _stdcall DestroyObject() override;
    intptr_t _stdcall Dispatcher(intptr_t id, intptr_t index, intptr_t value) override;
    int _stdcall ProcessEvent(int eventId, int eventValue, int flags) override;
    void _stdcall SaveRestoreState(IStream* stream, BOOL save) override;
    void _stdcall GetName(int section, int index, int value, char* name) override;
    void _stdcall Gen_Render(PWAV32FS buffer, int& length) override;
    TVoiceHandle _stdcall TriggerVoice(PVoiceParams params, intptr_t tag) override;
    void _stdcall Voice_Release(TVoiceHandle handle) override;
    void _stdcall Voice_Kill(TVoiceHandle handle) override;

    StftDevParams& guardControls() { return controls_; }
    int selectedSlot() const { return selected_.load(); }
    void selectSlot(int index) { selected_.store(std::clamp(index,0,3)); }
    void checkpoint();
    bool undo();
    bool loadWav(const std::wstring& path, std::string& error, int slotIndex = -1, bool remember = true);
    bool loadMidi(const std::wstring& path, std::string& error);
    void updateSettings(const Settings& value);
    Display display() const;
    std::vector<AudioSlice> sliceSnapshot() const;
    std::vector<int> playingSlices() const;
    int wavFrameCount() const;
    bool moveSliceBoundary(int boundaryIndex, int frame);

private:
    struct Voice {
        intptr_t hostTag = 0;
        PVoiceParams params = nullptr; // HOST-owned, live-updated for native slides
        int note = 60, color = 0;
        float startPitchCents = 0;
        double baseOffset = 0;
        double fineTune = 0; // Static sub-semitone offset in semitones
        double sourceStep = 1;
        int shiftCents = 0;
        bool released = false;
        double playhead_ = 0;
        int sliceIndex = -1;
        int releaseFrames = -1;
        int sliceEnd_ = 0;
        bool samplePlaying_ = false;
        int drainRemaining_ = 0;
        double previousHostSlide_ = 0;
        std::unique_ptr<StftVoice> engine_;

    };
    StftDevParams controls_;
    mutable std::mutex mutex_;
    struct Slot {
        Settings settings_;
        std::shared_ptr<const WaveData> wav_ = std::make_shared<WaveData>();
        std::vector<AudioSlice> slices_;
        bool slicesManuallyEdited_ = false;
        int lastMidiNote_ = -1, lastSlice_ = -1;
        float lastPitchCents_ = 0.f;
        std::wstring wavPath_;
    };
    std::array<Slot,4> slots_;
    std::atomic<int> selected_{0};
    struct History { Slot slot; int index; std::array<float,3> guard; };
    std::vector<History> history_;
    void checkpointLocked(int index);
    MidiFile midi_;
    std::wstring midiPath_;
    std::string status_ = "Load a chromatic WAV to begin";
    ResidualSpectrum::Plans fftPlans_;
    std::vector<std::unique_ptr<Voice>> voices_;

    NativeEditor* editor_ = nullptr;

    double sampleRate_ = 44100;
    std::atomic<float> samplesPerTick_{0.f};
    std::atomic<int> channelPitchCents_{0};
    std::atomic<float> bpm_{120.f};
    std::atomic<int> hostPpq_{192};
    std::array<std::array<float, 64>, 2> out_{};

    void stopVoices();
    void beginVocal(Voice& voice);

    void render(Voice& voice, PWAV32FS buffer, int length);
};

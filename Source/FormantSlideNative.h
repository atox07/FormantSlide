#pragma once
#include "fp_cplug.h"
#include "MidiFile.h"
#include "WaveLoader.h"
#include <signalsmith-stretch.h>
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
        int noteOffset = 0; // Native SDK pitch calibration (semitones)
        bool allSlideColors = true; // false = channel/color 5 only
        int fallbackGlideMs = 250;
        int midiOffsetTicks = 0;
    };
    struct Display {
        Settings settings;
        int slices = 0;
        int midiNotes = 0;
        std::wstring wavName, midiName;
        std::wstring wavFullPath, midiFullPath;
        std::string status;
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

    bool loadWav(const std::wstring& path, std::string& error);
    bool loadMidi(const std::wstring& path, std::string& error);
    void updateSettings(const Settings& value);
    Display display() const;
    std::vector<AudioSlice> sliceSnapshot() const;
    int wavFrameCount() const;
    bool moveSliceBoundary(int boundaryIndex, int frame);

private:
    struct Voice {
        intptr_t hostTag = 0;
        PVoiceParams params = nullptr; // HOST-owned, live-updated for native slides
        int note = 60, color = 0;
        float startPitchCents = 0;
        double baseOffset = 0;
        bool vocal = false;
        bool released = false;
    };
    mutable std::mutex mutex_;
    Settings settings_;
    WaveData wav_;
    std::vector<AudioSlice> slices_;
    bool slicesManuallyEdited_ = false;
    int anchorNote_ = -1; // first played vocal note maps to first WAV slice
    MidiFile midi_;
    std::wstring wavPath_, midiPath_;
    std::string status_ = "Load a chromatic WAV to begin";
    std::vector<std::unique_ptr<Voice>> voices_;
    Voice* current_ = nullptr;
    NativeEditor* editor_ = nullptr;

    double sampleRate_ = 44100, playhead_ = 0;
    std::atomic<float> samplesPerTick_{0.f};
    std::atomic<float> bpm_{120.f};
    std::atomic<int> hostPpq_{192};
    int sliceEnd_ = 0;
    bool samplePlaying_ = false;
    int drainRemaining_ = 0;
    double customPitch_ = 0, customTarget_ = 0, customStep_ = 0;
    double previousHostSlide_ = 0;
    int customRemaining_ = 0;
    signalsmith::stretch::SignalsmithStretch<float> stretch_;
    std::array<std::array<float, 64>, 2> in_{};
    std::array<std::array<float, 64>, 2> out_{};

    void initStretch();
    void beginVocal(Voice& voice);
    void beginSlide(const Voice& slideVoice, int noteLengthTicks, double hostTick);
    void render(PWAV32FS buffer, int length);
};

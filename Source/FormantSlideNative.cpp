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
    uint32_t version = 4;
    uint32_t equal = 0, slices = 12, root = 60, all = 1, glide = 250; // root retained for loading older projects
    int32_t offset = 0;
    wchar_t wav[512]{};
    wchar_t midi[512]{};
    int32_t pitchOrLegacyOffset = 0; // v2: key calibration in semitones; v4: audio transpose in cents
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
void applyVoicePitch(RubberBand::RubberBandStretcher& engine, double noteShift, int knobCents) {
    const double knobShift = knobCents / 100.0;
    const double totalShift = std::clamp(noteShift + knobShift, -48.0, 48.0);
    engine.setPitchScale(std::exp2(totalShift / 12.0));
    // R3 applies this envelope correction in addition to the overall pitch shift.
    // At neutral, use the original automatic formant preservation.
    engine.setFormantScale(knobCents == 0 ? 0.0 : std::exp2((knobShift - totalShift) / 12.0));
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

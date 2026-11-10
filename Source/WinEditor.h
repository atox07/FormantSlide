#pragma once
#include <memory>
#include <windows.h>

namespace juce { class ScopedJuceInitialiser_GUI; }
class FormantSlideNative;
class FormantSlideView;

// JUCE-rendered editor, hosted as a child HWND by the native FL Studio SDK.
class NativeEditor final {
public:
    explicit NativeEditor(FormantSlideNative& plugin);
    ~NativeEditor();
    HWND show(HWND parent);
    void hide();
    void close();
    HWND handle() const;
private:
    FormantSlideNative& plugin_;
    std::unique_ptr<juce::ScopedJuceInitialiser_GUI> juceLifetime_;
    std::unique_ptr<FormantSlideView> view_;
};

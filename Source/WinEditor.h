#pragma once
#include <windows.h>
#include <string>

class FormantSlideNative;

class NativeEditor {
public:
    explicit NativeEditor(FormantSlideNative& plugin) : plugin_(plugin) {}
    ~NativeEditor();
    void show(HWND parent);
    void hide();
    void close();
    HWND handle() const { return window_; }
private:
    FormantSlideNative& plugin_;
    HWND window_ = nullptr;
    HWND wavLabel_ = nullptr, midiLabel_ = nullptr, statusLabel_ = nullptr;
    HWND sliceMode_ = nullptr, sliceCount_ = nullptr, root_ = nullptr;
    HWND slideMode_ = nullptr, fallback_ = nullptr, offset_ = nullptr;
    static LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM w, LPARAM l);
    LRESULT onMessage(UINT message, WPARAM w, LPARAM l);
    void createWidgets();
    void syncUi();
    void saveUi();
    void browse(bool midi);
    void dropFile(const std::wstring& path);
};

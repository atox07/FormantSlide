#include "WinEditor.h"
#include "FormantSlideNative.h"
#include <algorithm>
#include <commdlg.h>
#include <cwctype>
#include <filesystem>
#include <mutex>
#include <shellapi.h>
#include <string>

namespace {
constexpr wchar_t kClass[] = L"FormantSlideNativeEditorV1";
enum Ids { LoadWav = 101, LoadMidi, SliceMode, SliceCount, Root,
           SlideMode, Fallback, Offset };
HFONT textFont() { return static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT)); }
HWND child(HWND parent, const wchar_t* cls, const wchar_t* label,
           DWORD style, int x, int y, int w, int h, int id = 0) {
    HWND ctl = CreateWindowExW(0, cls, label, WS_CHILD | WS_VISIBLE | style,
                              x, y, w, h, parent,
                              reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                              GetModuleHandleW(nullptr), nullptr);
    SendMessageW(ctl, WM_SETFONT, reinterpret_cast<WPARAM>(textFont()), TRUE);
    return ctl;
}
int number(HWND edit, int fallback) {
    wchar_t text[64]{};
    GetWindowTextW(edit, text, 63);
    wchar_t* end = nullptr;
    const long result = wcstol(text, &end, 10);
    return end == text ? fallback : static_cast<int>(result);
}
void numberText(HWND ctl, int v) { SetWindowTextW(ctl, std::to_wstring(v).c_str()); }
}

NativeEditor::~NativeEditor() { close(); }

void NativeEditor::show(HWND parent) {
    if (!window_) {
        static std::once_flag once;
        std::call_once(once, [] {
            WNDCLASSEXW cls{};
            cls.cbSize = sizeof(cls);
            cls.lpfnWndProc = &NativeEditor::windowProc;
            cls.hInstance = GetModuleHandleW(nullptr);
            cls.lpszClassName = kClass;
            cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
            RegisterClassExW(&cls);
        });
        window_ = CreateWindowExW(0, kClass, L"FormantSlide Native",
                                  WS_CHILD | WS_CLIPCHILDREN,
                                  0, 0, 740, 425, parent, nullptr,
                                  GetModuleHandleW(nullptr), this);
        if (window_) { DragAcceptFiles(window_, TRUE); syncUi(); }
    }
    if (window_) {
        SetParent(window_, parent);
        SetWindowPos(window_, nullptr, 0, 0, 740, 425, SWP_NOZORDER | SWP_SHOWWINDOW);
    }
}
void NativeEditor::hide() { if (window_) ShowWindow(window_, SW_HIDE); }
void NativeEditor::close() {
    if (window_) {
        DestroyWindow(window_);
        window_ = nullptr;
    }
}
LRESULT CALLBACK NativeEditor::windowProc(HWND window, UINT message, WPARAM w, LPARAM l) {
    NativeEditor* self = reinterpret_cast<NativeEditor*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
        self = static_cast<NativeEditor*>(cs->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->window_ = window;
    }
    if (self && message == WM_NCDESTROY) {
        LRESULT result = DefWindowProcW(window, message, w, l);
        self->window_ = nullptr;
        return result;
    }
    if (self) return self->onMessage(message, w, l);
    return DefWindowProcW(window, message, w, l);
}
void NativeEditor::createWidgets() {
    child(window_, L"STATIC", L"FormantSlide  /  FL Studio Native", 0, 20, 14, 580, 30);
    child(window_, L"STATIC", L"Native slide notes are controlled by FL Studio's own pitch curve.",
          0, 20, 48, 695, 25);
    child(window_, L"BUTTON", L"Load chromatic WAV...", BS_PUSHBUTTON, 20, 82, 180, 31, LoadWav);
    wavLabel_ = child(window_, L"STATIC", L"No WAV loaded", 0, 217, 87, 490, 25);
    child(window_, L"BUTTON", L"Import MIDI (fallback)...", BS_PUSHBUTTON, 20, 123, 180, 31, LoadMidi);
    midiLabel_ = child(window_, L"STATIC", L"No MIDI loaded", 0, 217, 129, 490, 25);

    child(window_, L"STATIC", L"Slicer", 0, 20, 177, 175, 22);
    sliceMode_ = child(window_, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL,
                      20, 201, 184, 250, SliceMode);
    SendMessageW(sliceMode_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Detect silence"));
    SendMessageW(sliceMode_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Equal divisions"));
    child(window_, L"STATIC", L"Divisions", 0, 223, 177, 100, 22);
    sliceCount_ = child(window_, L"EDIT", L"12", WS_BORDER | ES_NUMBER, 223, 201, 86, 27, SliceCount);
    child(window_, L"STATIC", L"First MIDI note", 0, 334, 177, 120, 22);
    root_ = child(window_, L"EDIT", L"60", WS_BORDER | ES_NUMBER, 334, 201, 86, 27, Root);

    child(window_, L"STATIC", L"Slide colors", 0, 20, 246, 180, 22);
    slideMode_ = child(window_, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL,
                      20, 270, 184, 250, SlideMode);
    SendMessageW(slideMode_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Only channel 5"));
    SendMessageW(slideMode_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Channels 3 to 16"));
    child(window_, L"STATIC", L"Fallback glide (ms)", 0, 223, 246, 150, 22);
    fallback_ = child(window_, L"EDIT", L"250", WS_BORDER | ES_NUMBER, 223, 270, 86, 27, Fallback);
    child(window_, L"STATIC", L"MIDI offset (host ticks)", 0, 334, 246, 195, 22);
    offset_ = child(window_, L"EDIT", L"0", WS_BORDER | ES_AUTOHSCROLL,
                    334, 270, 86, 27, Offset);

    child(window_, L"STATIC", L"Native pitch slide: target & length from FL Studio, no MIDI import needed.",
          0, 20, 312, 690, 23);
    child(window_, L"STATIC", L"Color slides: SDK GetLength -> imported MIDI -> fallback glide.",
          0, 20, 336, 690, 23);
    statusLabel_ = child(window_, L"STATIC", L"", 0, 20, 377, 700, 26);
}
void NativeEditor::syncUi() {
    if (!window_) return;
    const auto d = plugin_.display();
    SendMessageW(sliceMode_, CB_SETCURSEL, d.settings.equalSlices ? 1 : 0, 0);
    SendMessageW(slideMode_, CB_SETCURSEL, d.settings.allSlideColors ? 1 : 0, 0);
    numberText(sliceCount_, d.settings.sliceCount);
    numberText(root_, d.settings.firstNote);
    numberText(fallback_, d.settings.fallbackGlideMs);
    numberText(offset_, d.settings.midiOffsetTicks);
    SetWindowTextW(wavLabel_, d.wavName.empty() ? L"No WAV loaded" : d.wavName.c_str());
    SetWindowTextW(midiLabel_, d.midiName.empty() ? L"No MIDI loaded" : d.midiName.c_str());
    const std::wstring status = std::to_wstring(d.slices) + L" slices / " +
                                std::to_wstring(d.midiNotes) + L" MIDI notes / " +
                                std::wstring(d.status.begin(), d.status.end());
    SetWindowTextW(statusLabel_, status.c_str());
}
void NativeEditor::saveUi() {
    const auto old = plugin_.display().settings;
    auto s = old;
    s.equalSlices = SendMessageW(sliceMode_, CB_GETCURSEL, 0, 0) == 1;
    s.allSlideColors = SendMessageW(slideMode_, CB_GETCURSEL, 0, 0) == 1;
    s.sliceCount = number(sliceCount_, old.sliceCount);
    s.firstNote = number(root_, old.firstNote);
    s.fallbackGlideMs = number(fallback_, old.fallbackGlideMs);
    s.midiOffsetTicks = number(offset_, old.midiOffsetTicks);
    plugin_.updateSettings(s);
    syncUi();
}
void NativeEditor::dropFile(const std::wstring& path) {
    std::string error;
    std::wstring ext = std::filesystem::path(path).extension().wstring();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    bool ok = false;
    if (ext == L".wav") ok = plugin_.loadWav(path, error);
    else if (ext == L".mid" || ext == L".midi") ok = plugin_.loadMidi(path, error);
    if (!ok) MessageBoxA(window_, error.empty() ? "Unsupported file type" : error.c_str(),
                         "FormantSlide", MB_ICONWARNING | MB_OK);
    syncUi();
}
void NativeEditor::browse(bool midi) {
    wchar_t file[MAX_PATH]{};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = window_;
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter = midi ? L"MIDI files (*.mid)\0*.mid;*.midi\0All files\0*.*\0"
                            : L"WAV files (*.wav)\0*.wav\0All files\0*.*\0";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameW(&ofn)) dropFile(file);
}
LRESULT NativeEditor::onMessage(UINT message, WPARAM w, LPARAM l) {
    switch (message) {
    case WM_CREATE: createWidgets(); return 0;
    case WM_COMMAND: {
        const int id = LOWORD(w), code = HIWORD(w);
        if (code == BN_CLICKED && id == LoadWav) { browse(false); return 0; }
        if (code == BN_CLICKED && id == LoadMidi) { browse(true); return 0; }
        if ((code == CBN_SELCHANGE && (id == SliceMode || id == SlideMode)) ||
            (code == EN_KILLFOCUS && (id == SliceCount || id == Root || id == Fallback || id == Offset))) {
            saveUi(); return 0;
        }
        break;
    }
    case WM_DROPFILES: {
        HDROP drop = reinterpret_cast<HDROP>(w);
        wchar_t path[MAX_PATH]{};
        if (DragQueryFileW(drop, 0, path, MAX_PATH)) dropFile(path);
        DragFinish(drop);
        return 0;
    }
    }
    return DefWindowProcW(window_, message, w, l);
}

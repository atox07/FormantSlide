#include "WinEditor.h"
#include "FormantSlideNative.h"
#include <juce_audio_utils/juce_audio_utils.h>
#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

namespace {
constexpr int editorWidth = 760;
constexpr int editorHeight = 520;

juce::String fileName(const std::wstring& path) {
    return path.empty() ? juce::String() : juce::String(path.c_str());
}

void setLabel(juce::Label& label, const juce::String& value) {
    label.setText(value, juce::dontSendNotification);
    label.setColour(juce::Label::textColourId, juce::Colour(0xffbed0e2));
}
}

// Matches the graphical editor of the v0.2 VST3, without requiring a VST wrapper.
class FormantSlideView final : public juce::Component,
                             public juce::FileDragAndDropTarget,
                             private juce::ChangeListener,
                             private juce::Timer {
public:
    explicit FormantSlideView(FormantSlideNative& native)
        : plugin_(native), thumbnail_(16, thumbnailFormats_, cache_) {
        setOpaque(true);
        setSize(editorWidth, editorHeight);
        thumbnailFormats_.registerBasicFormats();
        thumbnail_.addChangeListener(this);

        theme_.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff2d4560));
        theme_.setColour(juce::ComboBox::backgroundColourId, juce::Colour(0xff273449));
        theme_.setColour(juce::ComboBox::textColourId, juce::Colours::white);
        theme_.setColour(juce::Slider::thumbColourId, juce::Colour(0xff74c9ff));
        theme_.setColour(juce::Slider::trackColourId, juce::Colour(0xff3979b8));
        theme_.setColour(juce::Slider::textBoxTextColourId, juce::Colours::white);
        theme_.setColour(juce::Slider::textBoxBackgroundColourId, juce::Colour(0xff273449));
        setLookAndFeel(&theme_);

        addAndMakeVisible(loadButton_);
        addAndMakeVisible(loadMidiButton_);
        addAndMakeVisible(wavLabel_);
        addAndMakeVisible(midiLabel_);
        addAndMakeVisible(modeLabel_);
        addAndMakeVisible(sliceModeLabel_);
        addAndMakeVisible(countLabel_);
        addAndMakeVisible(glideLabel_);
        addAndMakeVisible(rootLabel_);
        addAndMakeVisible(statusLabel_);
        addAndMakeVisible(warningLabel_);
        addAndMakeVisible(mode_);
        addAndMakeVisible(sliceMode_);
        addAndMakeVisible(count_);
        addAndMakeVisible(glide_);
        addAndMakeVisible(root_);

        setLabel(modeLabel_, "Slide MIDI channels");
        setLabel(sliceModeLabel_, "Slicer");
        setLabel(countLabel_, "Equal divisions");
        setLabel(glideLabel_, "Fallback glide (ms)");
        setLabel(rootLabel_, "First slice MIDI note");
        setLabel(warningLabel_, "Ctrl + wheel: zoom   |   Wheel: scroll   |   Double-click: fit   |   Drag a marker to edit");
        warningLabel_.setJustificationType(juce::Justification::centred);
        warningLabel_.setColour(juce::Label::textColourId, juce::Colour(0xffffb65c));
        wavLabel_.setJustificationType(juce::Justification::centredLeft);
        midiLabel_.setJustificationType(juce::Justification::centredLeft);
        midiLabel_.setColour(juce::Label::textColourId, juce::Colour(0xff8aa5be));
        statusLabel_.setJustificationType(juce::Justification::centredLeft);

        mode_.addItem("Channel 5", 1);
        mode_.addItem("Channels 3-16", 2);
        sliceMode_.addItem("WAV markers / detect silence", 1);
        sliceMode_.addItem("Equal divisions", 2);

        for (auto* slider : { &count_, &glide_, &root_ }) {
            slider->setSliderStyle(juce::Slider::LinearHorizontal);
            slider->setTextBoxStyle(juce::Slider::TextBoxRight, false, 54, 24);
        }
        count_.setRange(1.0, 64.0, 1.0);
        glide_.setRange(10.0, 2000.0, 1.0);
        root_.setRange(0.0, 127.0, 1.0);

        loadButton_.onClick = [this] { choose(false); };
        loadMidiButton_.onClick = [this] { choose(true); };
        mode_.onChange = [this] { pushSettings(); };
        sliceMode_.onChange = [this] { pushSettings(); };
        count_.onValueChange = [this] { pushSettings(); };
        glide_.onValueChange = [this] { pushSettings(); };
        root_.onValueChange = [this] { pushSettings(); };

        pullSettings();
        refreshFilePreview();
        const auto saved = plugin_.display();
        if (!saved.wavFullPath.empty()) {
            juce::File existing(juce::String(saved.wavFullPath.c_str()));
            if (existing.existsAsFile()) thumbnail_.setSource(new juce::FileInputSource(existing));
        }
        refreshStatus();
        startTimerHz(40); // refresh host pitch/slice diagnostics when FL plays notes
    }

    ~FormantSlideView() override {
        stopTimer();
        thumbnail_.removeChangeListener(this);
        setLookAndFeel(nullptr);
    }

    void paint(juce::Graphics& g) override {
        g.fillAll(juce::Colour(0xff171c25));
        g.setColour(juce::Colours::white);
        g.setFont(juce::FontOptions(28.f, juce::Font::bold));
        g.drawText("FormantSlide", 20, 14, editorWidth - 40, 35, juce::Justification::centred);
        g.setFont(juce::FontOptions(13.f));
        g.setColour(juce::Colour(0xff91a8bd));
        g.drawText("CHROMATIC SLICER / MIDI SLIDES / FORMANT COMPENSATION", 20, 53,
                   editorWidth - 40, 20, juce::Justification::centred);

        g.setColour(juce::Colour(0xff233044));
        g.fillRoundedRectangle(waveBounds_.toFloat(), 7.f);
        if (thumbnail_.getTotalLength() > 0) {
            g.setColour(juce::Colour(0xff74c9ff));
            thumbnail_.drawChannels(g, waveBounds_, viewStart_ * thumbnail_.getTotalLength(),
                                    (viewStart_ + viewSpan_) * thumbnail_.getTotalLength(), 0.85f);
        } else {
            g.setColour(juce::Colour(0xff91a8bd));
            g.drawFittedText("Load a chromatic-scale WAV to see slices", waveBounds_,
                             juce::Justification::centred, 1);
        }
        const auto slices = plugin_.sliceSnapshot();
        const int total = plugin_.wavFrameCount();
        g.reduceClipRegion(waveBounds_);
        if (total > 0) {
            for (size_t i = 0; i < slices.size(); ++i) {
                const float left = screenX(double(slices[i].begin) / total);
                const float right = screenX(double(slices[i].end) / total);
                if (right < waveBounds_.getX() || left > waveBounds_.getRight()) continue;
                const bool playing = std::find(active_.begin(), active_.end(), int(i)) != active_.end();
                if (playing || int(i) == selectedSlice_) {
                    g.setColour(playing ? juce::Colour(0x9954c996) : juce::Colours::yellow.withAlpha(0.15f));
                    g.fillRect(juce::Rectangle<float>(left, float(waveBounds_.getY()),
                                                     right - left, float(waveBounds_.getHeight())));
                }
                g.setColour(juce::Colours::white.withAlpha(0.55f));
                g.drawVerticalLine(juce::roundToInt(left), float(waveBounds_.getY()),
                                   float(waveBounds_.getBottom()));
                if (i > 0) {
                    g.setColour(juce::Colour(0xff74c9ff));
                    g.fillRoundedRectangle(left - 4.f, float(waveBounds_.getY()) + 3.f,
                                           8.f, 19.f, 3.f);
                }
            }
        }
    }

    void resized() override {
        wavLabel_.setBounds(26, 85, 540, 27);
        loadButton_.setBounds(605, 83, 125, 30);
        waveBounds_ = {26, 125, 704, 125};
        statusLabel_.setBounds(26, 261, 527, 26);
        loadMidiButton_.setBounds(575, 260, 155, 28);
        midiLabel_.setBounds(26, 287, 704, 19);

        sliceModeLabel_.setBounds(26, 319, 140, 20);
        sliceMode_.setBounds(26, 343, 205, 30);
        countLabel_.setBounds(256, 319, 160, 20);
        count_.setBounds(256, 344, 450, 28);

        modeLabel_.setBounds(26, 388, 185, 20);
        mode_.setBounds(26, 412, 205, 29);
        glideLabel_.setBounds(280, 388, 205, 20);
        glide_.setBounds(280, 413, 250, 28);
        rootLabel_.setBounds(543, 388, 190, 20);
        root_.setBounds(543, 413, 182, 28);
        warningLabel_.setBounds(20, 457, 720, 24);
    }

    void mouseDown(const juce::MouseEvent& event) override {
        if (!waveBounds_.contains(event.getPosition())) return;
        const auto slices = plugin_.sliceSnapshot();
        const int total = plugin_.wavFrameCount();
        if (total <= 0) return;
        draggedBoundary_ = -1;
        float bestDistance = 9.0f;
        for (size_t i = 1; i < slices.size(); ++i) {
            const float x = screenX(double(slices[i].begin) / total);
            const float distance = std::abs(float(event.x) - x);
            if (distance < bestDistance) {
                bestDistance = distance;
                draggedBoundary_ = int(i);
            }
        }
        if (draggedBoundary_ > 0) {
            selectedSlice_ = draggedBoundary_;
            setMouseCursor(juce::MouseCursor::LeftRightResizeCursor);
            updateDraggedBoundary(event.x);
        } else {
            const double position = viewStart_ + viewSpan_ * fraction(event.x);
            selectedSlice_ = -1;
            for (size_t i = 0; i < slices.size(); ++i) {
                if (position >= float(slices[i].begin) / float(total) &&
                    position < float(slices[i].end) / float(total)) {
                    selectedSlice_ = int(i);
                    break;
                }
            }
        }
        refreshStatus();
        repaint();
    }

    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override {
        if (!waveBounds_.contains(e.getPosition())) return;
        const int frames = plugin_.wavFrameCount();
        if (frames < 2) return;
        if (e.mods.isCtrlDown()) {
            const double anchor = fraction(e.x);
            const double next = std::clamp(viewSpan_ * std::exp(-2.0 * wheel.deltaY),
                                           std::min(1.0, 32.0 / frames), 1.0);
            viewStart_ += anchor * (viewSpan_ - next);
            viewSpan_ = next;
        } else {
            viewStart_ -= viewSpan_ * (wheel.deltaX + wheel.deltaY) * 0.4;
        }
        viewStart_ = std::clamp(viewStart_, 0.0, 1.0 - viewSpan_);
        repaint(waveBounds_);
    }

    void mouseDoubleClick(const juce::MouseEvent& e) override {
        if (waveBounds_.contains(e.getPosition())) { viewStart_ = 0; viewSpan_ = 1; repaint(waveBounds_); }
    }

    void mouseDrag(const juce::MouseEvent& event) override {
        if (draggedBoundary_ > 0) updateDraggedBoundary(event.x);
    }

    void mouseUp(const juce::MouseEvent&) override {
        draggedBoundary_ = -1;
        setMouseCursor(juce::MouseCursor::NormalCursor);
    }

    bool isInterestedInFileDrag(const juce::StringArray& files) override {
        for (const auto& path : files)
            if (juce::File(path).hasFileExtension("wav;mid;midi")) return true;
        return false;
    }

    void filesDropped(const juce::StringArray& files, int, int) override {
        for (const auto& path : files) {
            const juce::File file(path);
            if (file.hasFileExtension("wav;mid;midi")) { loadFile(file); break; }
        }
    }

private:
    FormantSlideNative& plugin_;
    juce::LookAndFeel_V4 theme_;
    juce::TextButton loadButton_{"Load WAV"};
    juce::TextButton loadMidiButton_{"Import MIDI"};
    juce::Label wavLabel_, midiLabel_, modeLabel_, sliceModeLabel_, countLabel_;
    juce::Label glideLabel_, rootLabel_, statusLabel_, warningLabel_;
    juce::ComboBox mode_, sliceMode_;
    juce::Slider count_, glide_, root_;
    juce::AudioFormatManager thumbnailFormats_;
    juce::AudioThumbnailCache cache_{5};
    juce::AudioThumbnail thumbnail_;
    juce::Rectangle<int> waveBounds_;
    double viewStart_ = 0, viewSpan_ = 1;
    std::vector<int> active_;
    double fraction(int x) const { return double(x - waveBounds_.getX()) / waveBounds_.getWidth(); }
    float screenX(double position) const {
        return float(waveBounds_.getX() + waveBounds_.getWidth() * (position - viewStart_) / viewSpan_);
    }
    int selectedSlice_ = -1;
    int draggedBoundary_ = -1;
    bool updatingControls_ = false;

    void updateDraggedBoundary(int x) {
        const int total = plugin_.wavFrameCount();
        if (draggedBoundary_ < 1 || total < 2) return;
        const double proportion = juce::jlimit(0.0, 1.0,
            double(x - waveBounds_.getX()) / double(waveBounds_.getWidth()));
        const int frame = juce::roundToInt((viewStart_ + proportion * viewSpan_) * total);
        if (plugin_.moveSliceBoundary(draggedBoundary_, frame)) {
            refreshStatus();
            repaint(waveBounds_);
        }
    }

    void pullSettings() {
        updatingControls_ = true;
        const auto s = plugin_.display().settings;
        mode_.setSelectedId(s.allSlideColors ? 2 : 1, juce::dontSendNotification);
        sliceMode_.setSelectedId(s.equalSlices ? 2 : 1, juce::dontSendNotification);
        count_.setValue(s.sliceCount, juce::dontSendNotification);
        glide_.setValue(s.fallbackGlideMs, juce::dontSendNotification);
        root_.setValue(s.firstNote, juce::dontSendNotification);
        updatingControls_ = false;
    }

    void pushSettings() {
        if (updatingControls_) return;
        auto settings = plugin_.display().settings;
        settings.allSlideColors = mode_.getSelectedId() == 2;
        settings.equalSlices = sliceMode_.getSelectedId() == 2;
        settings.sliceCount = int(std::lround(count_.getValue()));
        settings.fallbackGlideMs = int(std::lround(glide_.getValue()));
        settings.firstNote = int(std::lround(root_.getValue()));
        plugin_.updateSettings(settings);
        selectedSlice_ = -1;
        refreshStatus();
        repaint();
    }

    void refreshFilePreview() {
        const auto data = plugin_.display();
        wavLabel_.setText(data.wavName.empty() ? "No WAV loaded" : fileName(data.wavName),
                          juce::dontSendNotification);
        midiLabel_.setText(data.midiName.empty() ? "MIDI duration fallback: none" :
                           "MIDI: " + fileName(data.midiName), juce::dontSendNotification);
        // A JUCE thumbnail reproduces the same waveform display as our VST3.
        // We only know the basename in Display, so the full path is passed to this
        // method by loadFile() after each import.
    }

    void refreshStatus() {
        const auto data = plugin_.display();
        juce::String status = juce::String(data.slices) + " slices | root " +
            juce::String(data.rootMidi);
        if (data.lastMidiNote >= 0)
            status += " | MIDI " + juce::String(data.lastMidiNote) +
                      " [" + juce::String(data.lastPitchCents, 0) + "c]" +
                      " -> #" + juce::String(data.lastSlice + 1);
        if (selectedSlice_ >= 0)
            status = "Selected slice " + juce::String(selectedSlice_ + 1) +
                     " | drag a white marker to adjust its boundary";
        else if (data.slices == 1 && !data.wavName.empty())
            status += "  |  Only 1 slice: try Equal divisions";
        if (data.lastMidiNote >= 0)
            status = "Init " + juce::String(data.initCents, 2) + "   Final " + juce::String(data.lastPitchCents, 2) +
                     "   Channel " + juce::String(data.channelCents) + "   Slice #" + juce::String(data.lastSlice + 1);
        statusLabel_.setText(status, juce::dontSendNotification);
    }

    void loadFile(const juce::File& file) {
        if (!file.existsAsFile()) return;
        const std::wstring path(file.getFullPathName().toWideCharPointer());
        std::string error;
        const bool wav = file.hasFileExtension("wav");
        const bool ok = wav ? plugin_.loadWav(path, error) : plugin_.loadMidi(path, error);
        if (!ok) {
            juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                                   "FormantSlide", juce::String::fromUTF8(error.c_str()));
            return;
        }
        selectedSlice_ = -1;
        if (wav) { viewStart_ = 0; viewSpan_ = 1; thumbnail_.setSource(new juce::FileInputSource(file)); }
        refreshFilePreview();
        refreshStatus();
        repaint();
    }

    void choose(bool midi) {
        const auto title = midi ? "Choose a MIDI file" : "Choose a chromatic WAV";
        auto chooser = std::make_shared<juce::FileChooser>(title, juce::File(),
                                                            midi ? "*.mid;*.midi" : "*.wav");
        juce::Component::SafePointer<FormantSlideView> safeThis(this);
        chooser->launchAsync(juce::FileBrowserComponent::openMode |
                             juce::FileBrowserComponent::canSelectFiles,
                             [safeThis, chooser](const juce::FileChooser& fc) {
                                 if (safeThis != nullptr && fc.getResult().existsAsFile())
                                     safeThis->loadFile(fc.getResult());
                             });
    }

    void changeListenerCallback(juce::ChangeBroadcaster*) override { repaint(); }
    void timerCallback() override {
        auto playing = plugin_.playingSlices();
        if (playing != active_) { active_ = std::move(playing); repaint(waveBounds_); }
        refreshStatus();
    }
};

NativeEditor::NativeEditor(FormantSlideNative& plugin) : plugin_(plugin) {}
NativeEditor::~NativeEditor() { close(); }

HWND NativeEditor::show(HWND parent) {
    if (!::IsWindow(parent)) return nullptr;
    if (!juceLifetime_) juceLifetime_ = std::make_unique<juce::ScopedJuceInitialiser_GUI>();
    if (!view_) view_ = std::make_unique<FormantSlideView>(plugin_);
    if (view_->isOnDesktop()) {
        const auto existing = static_cast<HWND>(view_->getPeer() ? view_->getPeer()->getNativeHandle() : nullptr);
        if (!::IsWindow(existing) || ::GetParent(existing) != parent)
            view_->removeFromDesktop();
    }
    if (!view_->isOnDesktop())
        view_->addToDesktop(0, parent); // Host-owned parent HWND
    const auto handle = static_cast<HWND>(view_->getPeer() ? view_->getPeer()->getNativeHandle() : nullptr);
    if (!::IsWindow(handle)) return nullptr;
    // Ensure the JUCE peer really is a child window before publishing it to FL.
    ::SetWindowLongPtrW(handle, GWL_STYLE,
        (::GetWindowLongPtrW(handle, GWL_STYLE) & ~WS_POPUP) | WS_CHILD | WS_CLIPCHILDREN);
    if (::GetParent(handle) != parent) ::SetParent(handle, parent);
    view_->setVisible(true);
    ::SetWindowPos(handle, nullptr, 0, 0, editorWidth, editorHeight,
                   SWP_NOACTIVATE | SWP_NOZORDER | SWP_SHOWWINDOW);
    return handle;
}

void NativeEditor::hide() {
    if (!view_) return;
    view_->setVisible(false);
    if (const auto hwnd = handle()) ::ShowWindow(hwnd, SW_HIDE);
}

void NativeEditor::close() {
    // Destruction order is important: destroy all JUCE components before
    // the JUCE message manager is shut down.
    view_.reset();
    juceLifetime_.reset();
}

HWND NativeEditor::handle() const {
    if (!view_ || !view_->isOnDesktop()) return nullptr;
    const auto hwnd = static_cast<HWND>(view_->getPeer() ? view_->getPeer()->getNativeHandle() : nullptr);
    return ::IsWindow(hwnd) ? hwnd : nullptr;
}

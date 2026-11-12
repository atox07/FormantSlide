#include "WinEditor.h"
#include "FormantSlideNative.h"
#include "StftDevParams.h"
#include <juce_audio_utils/juce_audio_utils.h>
#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

namespace {
constexpr int editorWidth = 680;
constexpr int editorHeight = 384;

// ---------------------------------------------------------------------------
// Palette (FL-like: charcoal greys + orange accent). Tweak here to retune
// the whole interface; nothing else in the file hard-codes colours.
// ---------------------------------------------------------------------------
namespace ui {
const juce::Colour bg        {0xff2a2f33};
const juce::Colour panel     {0xff343a3f};
const juce::Colour panelEdge {0xff1d2124};
const juce::Colour well      {0xff1a1e21};
const juce::Colour control   {0xff454d53};
const juce::Colour controlHi {0xff566066};
const juce::Colour text      {0xffd8dde1};
const juce::Colour dim       {0xff8c969d};
const juce::Colour accent    {0xffff9a2e};
const juce::Colour playing   {0xff5fd068};

juce::Font font(float height, bool bold = false) {
    return juce::Font(juce::FontOptions(height, bold ? juce::Font::bold : juce::Font::plain));
}
}

juce::String fileName(const std::wstring& path) {
    return path.empty() ? juce::String() : juce::String(path.c_str());
}

void setLabel(juce::Label& label, const juce::String& value) {
    label.setText(value, juce::dontSendNotification);
    label.setColour(juce::Label::textColourId, ui::text);
}

// ---------------------------------------------------------------------------
// FL-style look and feel: flat dark controls, orange value arcs, no chrome.
// ---------------------------------------------------------------------------
class FlLook final : public juce::LookAndFeel_V4 {
public:
    FlLook() {
        setColour(juce::ResizableWindow::backgroundColourId, ui::bg);
        setColour(juce::Label::textColourId, ui::text);

        setColour(juce::TextButton::buttonColourId, ui::control);
        setColour(juce::TextButton::textColourOffId, ui::text);
        setColour(juce::TextButton::textColourOnId, ui::text);

        setColour(juce::ComboBox::backgroundColourId, ui::control);
        setColour(juce::ComboBox::textColourId, ui::text);
        setColour(juce::ComboBox::outlineColourId, ui::panelEdge);
        setColour(juce::ComboBox::arrowColourId, ui::accent);

        setColour(juce::PopupMenu::backgroundColourId, ui::panel);
        setColour(juce::PopupMenu::textColourId, ui::text);
        setColour(juce::PopupMenu::highlightedBackgroundColourId, ui::accent);
        setColour(juce::PopupMenu::highlightedTextColourId, ui::well);

        setColour(juce::Slider::thumbColourId, ui::text);
        setColour(juce::Slider::trackColourId, ui::accent);
        setColour(juce::Slider::rotarySliderFillColourId, ui::accent);
        setColour(juce::Slider::textBoxTextColourId, ui::text);
        setColour(juce::Slider::textBoxBackgroundColourId, ui::well);
        setColour(juce::Slider::textBoxOutlineColourId, ui::panelEdge);
        setColour(juce::Slider::textBoxHighlightColourId, ui::accent.withAlpha(0.4f));

        setColour(juce::TextEditor::backgroundColourId, ui::well);
        setColour(juce::TextEditor::textColourId, ui::text);
        setColour(juce::TextEditor::highlightColourId, ui::accent.withAlpha(0.4f));
        setColour(juce::TextEditor::outlineColourId, ui::panelEdge);
        setColour(juce::TextEditor::focusedOutlineColourId, ui::accent);
        setColour(juce::CaretComponent::caretColourId, ui::accent);
    }

    juce::Font getTextButtonFont(juce::TextButton&, int) override { return ui::font(12.5f); }
    juce::Font getComboBoxFont(juce::ComboBox&) override { return ui::font(12.5f); }
    juce::Font getPopupMenuFont() override { return ui::font(12.5f); }

    juce::Label* createSliderTextBox(juce::Slider& slider) override {
        auto* label = LookAndFeel_V4::createSliderTextBox(slider);
        label->setFont(ui::font(12.f));
        label->setJustificationType(juce::Justification::centred);
        return label;
    }

    void drawButtonBackground(juce::Graphics& g, juce::Button& button, const juce::Colour&,
                              bool highlighted, bool down) override {
        const auto r = button.getLocalBounds().toFloat().reduced(0.5f);
        const auto base = down ? ui::accent.darker(0.15f)
                               : (highlighted ? ui::controlHi : ui::control);
        g.setGradientFill(juce::ColourGradient(base.brighter(0.08f), 0.f, r.getY(),
                                               base.darker(0.12f), 0.f, r.getBottom(), false));
        g.fillRoundedRectangle(r, 3.f);
        g.setColour(ui::panelEdge);
        g.drawRoundedRectangle(r, 3.f, 1.f);
    }

    void drawComboBox(juce::Graphics& g, int width, int height, bool, int, int, int, int,
                      juce::ComboBox& box) override {
        const auto r = juce::Rectangle<float>(0.f, 0.f, float(width), float(height)).reduced(0.5f);
        g.setColour(box.isMouseOver() ? ui::controlHi : ui::control);
        g.fillRoundedRectangle(r, 3.f);
        g.setColour(ui::panelEdge);
        g.drawRoundedRectangle(r, 3.f, 1.f);
        const float cx = float(width) - 14.f, cy = float(height) * 0.5f;
        juce::Path arrow;
        arrow.addTriangle(cx - 4.f, cy - 2.f, cx + 4.f, cy - 2.f, cx, cy + 3.f);
        g.setColour(ui::accent);
        g.fillPath(arrow);
    }

    void drawLinearSlider(juce::Graphics& g, int x, int y, int width, int height, float sliderPos,
                          float minSliderPos, float maxSliderPos, juce::Slider::SliderStyle style,
                          juce::Slider& slider) override {
        if (style != juce::Slider::LinearHorizontal) {
            LookAndFeel_V4::drawLinearSlider(g, x, y, width, height, sliderPos,
                                             minSliderPos, maxSliderPos, style, slider);
            return;
        }
        const float cy = float(y) + float(height) * 0.5f;
        const juce::Rectangle<float> track(float(x), cy - 3.f, float(width), 6.f);
        g.setColour(ui::well);
        g.fillRoundedRectangle(track, 3.f);
        g.setColour(ui::panelEdge);
        g.drawRoundedRectangle(track, 3.f, 1.f);
        g.setColour(ui::accent);
        g.fillRoundedRectangle(track.withRight(std::max(track.getX() + 3.f, sliderPos)).reduced(1.f, 1.f), 2.f);
        const bool hot = slider.isMouseOverOrDragging();
        g.setColour(hot ? juce::Colours::white : ui::text);
        g.fillRoundedRectangle(juce::Rectangle<float>(8.f, 16.f).withCentre({sliderPos, cy}), 2.5f);
        g.setColour(ui::panelEdge);
        g.drawRoundedRectangle(juce::Rectangle<float>(8.f, 16.f).withCentre({sliderPos, cy}), 2.5f, 1.f);
    }

    void drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height, float pos,
                          float startAngle, float endAngle, juce::Slider& slider) override {
        const auto area = juce::Rectangle<float>(float(x), float(y), float(width), float(height)).reduced(4.f);
        const float d = std::min(area.getWidth(), area.getHeight());
        const auto c = area.getCentre();
        const float angle = startAngle + pos * (endAngle - startAngle);
        const float mid = 0.5f * (startAngle + endAngle);      // top = zero for the bipolar pitch knob
        const float arcR = d * 0.5f - 2.f;
        const bool hot = slider.isMouseOverOrDragging();

        juce::Path track, value;
        track.addCentredArc(c.x, c.y, arcR, arcR, 0.f, startAngle, endAngle, true);
        value.addCentredArc(c.x, c.y, arcR, arcR, 0.f, mid, angle, true);
        const juce::PathStrokeType stroke(3.f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
        g.setColour(ui::well);
        g.strokePath(track, stroke);
        g.setColour(hot ? ui::accent.brighter(0.2f) : ui::accent);
        g.strokePath(value, stroke);

        const auto body = juce::Rectangle<float>(d, d).withCentre(c).reduced(d * 0.17f);
        g.setGradientFill(juce::ColourGradient(juce::Colour(0xff555e64), body.getX(), body.getY(),
                                               juce::Colour(0xff2b3135), body.getX(), body.getBottom(), false));
        g.fillEllipse(body);
        g.setColour(ui::panelEdge);
        g.drawEllipse(body, 1.2f);

        juce::Path tick;
        tick.addRoundedRectangle(-1.3f, -body.getHeight() * 0.5f + 2.5f, 2.6f, body.getHeight() * 0.32f, 1.f);
        g.setColour(juce::Colours::white);
        g.fillPath(tick, juce::AffineTransform::rotation(angle).translated(c.x, c.y));
    }
};
}

// Matches the graphical editor of the v0.2 VST3, without requiring a VST wrapper.
// Slider senza limiti: se scrivi nel box un valore fuori range, il range si allarga.
class FreeSlider final : public juce::Slider {
public:
    double getValueFromText(const juce::String& text) override {
        const double v = text.replaceCharacter(',', '.').getDoubleValue();
        if (v < getMinimum() || v > getMaximum())
            setRange(std::min(getMinimum(), v), std::max(getMaximum(), v), 0.0);
        return v;
    }
};

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

        setLookAndFeel(&theme_);

        addAndMakeVisible(loadButton_);
        addAndMakeVisible(wavLabel_);
        addAndMakeVisible(sliceModeLabel_);
        addAndMakeVisible(countLabel_);
        addAndMakeVisible(rootLabel_);
        addAndMakeVisible(statusLabel_);
        addAndMakeVisible(warningLabel_);
        addAndMakeVisible(sliceMode_);
        addAndMakeVisible(count_);
        addAndMakeVisible(root_);
        addAndMakeVisible(pitchLabel_);
        addAndMakeVisible(pitch_);
        addAndMakeVisible(devButton_);
        setupDevControls();

        for (auto* label : { &wavLabel_, &sliceModeLabel_, &countLabel_, &rootLabel_,
                             &statusLabel_, &warningLabel_ })
            label->setFont(ui::font(12.f));
        wavLabel_.setFont(ui::font(13.5f, true));

        setLabel(pitchLabel_, "PITCH");
        pitchLabel_.setFont(ui::font(10.5f, true));
        pitchLabel_.setColour(juce::Label::textColourId, ui::dim);
        pitch_.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        pitch_.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 78, 18);
        pitch_.setRange(-24.0, 24.0, 0.01);
        pitch_.setTextValueSuffix(" st");
        pitch_.setDoubleClickReturnValue(true, 0.0);
        pitch_.onValueChange = [this] { pushSettings(); };

        setLabel(sliceModeLabel_, "Slicer");
        setLabel(countLabel_, "Equal divisions");
        setLabel(rootLabel_, "First slice MIDI note");
        for (auto* label : { &sliceModeLabel_, &countLabel_, &rootLabel_ })
            label->setColour(juce::Label::textColourId, ui::dim);
        setLabel(warningLabel_, "Ctrl + wheel: zoom   |   Wheel: scroll   |   Double-click: fit   |   Drag a marker to edit");
        warningLabel_.setJustificationType(juce::Justification::centred);
        warningLabel_.setColour(juce::Label::textColourId, ui::dim);
        wavLabel_.setJustificationType(juce::Justification::centredLeft);
        statusLabel_.setJustificationType(juce::Justification::centredLeft);

        sliceMode_.addItem("WAV markers / detect silence", 1);
        sliceMode_.addItem("Equal divisions", 2);

        for (auto* slider : { &count_, &root_ }) {
            slider->setSliderStyle(juce::Slider::LinearHorizontal);
            slider->setTextBoxStyle(juce::Slider::TextBoxRight, false, 50, 22);
        }
        count_.setRange(1.0, 64.0, 1.0);
        root_.setRange(0.0, 127.0, 1.0);

        loadButton_.onClick = [this] { chooseWav(); };
        devButton_.onClick = [this] {
            devPage_ = (devPage_ + 1) % 3;
            devButton_.setButtonText(devPage_ == 0 ? "DEV" : devPage_ == 1 ? "DEV 1" : "DEV 2");
            updateDevVisibility();
            repaint(panelSlicer_);
        };
        sliceMode_.onChange = [this] { pushSettings(); };
        count_.onValueChange = [this] { pushSettings(); };
        root_.onValueChange = [this] { pushSettings(); };

        pullSettings();
        updateDevVisibility();
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
        g.fillAll(ui::bg);

        drawPanel(g, panelSlicer_, devPage_ == 1 ? "DEV 1 - live (scrivi nel box: nessun limite)"
                                 : devPage_ == 2 ? "DEV 2 - FFT / crossover: valgono dalla PROSSIMA nota"
                                                 : "SLICER");
        drawPanel(g, panelPitch_, nullptr); // its header is pitchLabel_

        // Waveform well / drop zone.
        const auto waveF = waveBounds_.toFloat();
        g.setColour(ui::well);
        g.fillRoundedRectangle(waveF, 4.f);
        g.setColour(ui::panelEdge);
        g.drawRoundedRectangle(waveF.reduced(0.5f), 4.f, 1.f);
        if (dragOver_) {
            g.setColour(ui::accent);
            g.drawRoundedRectangle(waveF.reduced(1.f), 4.f, 2.f);
        }
        g.setColour(ui::panelEdge.brighter(0.2f));
        g.drawHorizontalLine(waveBounds_.getCentreY(), float(waveBounds_.getX()), float(waveBounds_.getRight()));

        if (thumbnail_.getTotalLength() > 0) {
            g.setColour(ui::accent.withAlpha(0.92f));
            thumbnail_.drawChannels(g, waveBounds_, viewStart_ * thumbnail_.getTotalLength(),
                                    (viewStart_ + viewSpan_) * thumbnail_.getTotalLength(), 0.85f);
        } else {
            g.setColour(ui::dim);
            g.setFont(ui::font(13.f));
            g.drawFittedText("Drop a chromatic WAV here, or press Load WAV", waveBounds_,
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
                    g.setColour(playing ? ui::playing.withAlpha(0.30f) : ui::accent.withAlpha(0.16f));
                    g.fillRect(juce::Rectangle<float>(left, float(waveBounds_.getY()),
                                                     right - left, float(waveBounds_.getHeight())));
                }
                g.setColour(ui::accent.withAlpha(0.55f));
                g.drawVerticalLine(juce::roundToInt(left), float(waveBounds_.getY()),
                                   float(waveBounds_.getBottom()));
                if (i > 0) {
                    g.setColour(ui::accent);
                    g.fillRoundedRectangle(left - 4.f, float(waveBounds_.getY()) + 3.f,
                                           8.f, 19.f, 3.f);
                }
                // Note name of the key that plays this slice (FL convention: C5 = MIDI 60).
                const float visibleLeft = std::max(left, float(waveBounds_.getX()));
                const float visibleRight = std::min(right, float(waveBounds_.getRight()));
                if (visibleRight - visibleLeft >= 24.f) {
                    g.setColour(playing ? ui::playing : ui::dim);
                    g.setFont(ui::font(10.5f));
                    const auto name = juce::MidiMessage::getMidiNoteName(
                        juce::jlimit(0, 127, rootNote_ + int(i)), true, true, 5);
                    g.drawText(name, juce::Rectangle<float>(visibleLeft, float(waveBounds_.getBottom()) - 17.f,
                                                            visibleRight - visibleLeft, 14.f),
                               juce::Justification::centred, false);
                }
            }
        }
    }

    void resized() override {
        const int m = 12;
        const int w = getWidth();

        // Top strip: file name + Load WAV.
        wavLabel_.setBounds(m, 10, w - 2 * m - 210, 26);
        devButton_.setBounds(w - m - 110 - 8 - 62, 9, 62, 28);
        loadButton_.setBounds(w - m - 110, 9, 110, 28);

        // Waveform / drop zone.
        waveBounds_ = {m, 46, w - 2 * m, 150};

        // Status line.
        statusLabel_.setBounds(m, 202, w - 2 * m, 24);

        // Panels.
        const int py = 236, ph = 120;
        panelSlicer_ = {m, py, 506, ph};
        const int pitchX = panelSlicer_.getRight() + 10;
        panelPitch_ = {pitchX, py, w - m - pitchX, ph};

        // SLICER: label on the left, control on the right.
        {
            const int lx = panelSlicer_.getX() + 12, cx = panelSlicer_.getX() + 136;
            const int cw = panelSlicer_.getRight() - 12 - cx;
            sliceModeLabel_.setBounds(lx, py + 28, 120, 26);
            sliceMode_.setBounds(cx, py + 28, cw, 26);
            countLabel_.setBounds(lx, py + 58, 120, 26);
            count_.setBounds(cx, py + 58, cw, 26);
            rootLabel_.setBounds(lx, py + 88, 124, 26);
            root_.setBounds(cx, py + 88, cw, 26);

            // DEV: stessa area dello SLICER, 4 righe da 22 px.
            for (int i = 0; i < 8; ++i) {
                devLabel_[i].setBounds(lx, py + 24 + (i % 4) * 23, 120, 22);
                devSlider_[i].setBounds(cx - 14, py + 24 + (i % 4) * 23, cw + 14, 22);
            }
        }

        // PITCH: header + big knob with its value underneath.
        pitchLabel_.setBounds(panelPitch_.getX() + 12, py + 5, panelPitch_.getWidth() - 24, 16);
        pitch_.setBounds(panelPitch_.getX() + (panelPitch_.getWidth() - 80) / 2, py + 22, 80, 94);

        warningLabel_.setBounds(m, py + ph + 6, w - 2 * m, 20);
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
            if (juce::File(path).hasFileExtension("wav")) return true;
        return false;
    }

    void fileDragEnter(const juce::StringArray&, int, int) override {
        dragOver_ = true;
        repaint(waveBounds_);
    }

    void fileDragExit(const juce::StringArray&) override {
        dragOver_ = false;
        repaint(waveBounds_);
    }

    void filesDropped(const juce::StringArray& files, int, int) override {
        dragOver_ = false;
        repaint(waveBounds_);
        for (const auto& path : files) {
            const juce::File file(path);
            if (file.hasFileExtension("wav")) { loadFile(file); break; }
        }
    }

private:
    FormantSlideNative& plugin_;
    FlLook theme_;
    juce::TextButton loadButton_{"Load WAV"};
    juce::Label wavLabel_, sliceModeLabel_, countLabel_, rootLabel_, statusLabel_, warningLabel_;
    juce::ComboBox sliceMode_;
    juce::Label pitchLabel_;
    juce::Slider pitch_;
    juce::Slider count_, root_;
    juce::TextButton devButton_{"DEV"};
    juce::Label devLabel_[8];
    FreeSlider devSlider_[8];
    int devPage_ = 0; // 0 = Slicer, 1 = DEV 1 (live), 2 = DEV 2 (alla prossima nota)
    juce::AudioFormatManager thumbnailFormats_;
    juce::AudioThumbnailCache cache_{5};
    juce::AudioThumbnail thumbnail_;
    juce::Rectangle<int> waveBounds_;
    juce::Rectangle<int> panelSlicer_, panelPitch_;
    double viewStart_ = 0, viewSpan_ = 1;
    std::vector<int> active_;
    int rootNote_ = 60;        // cached copy of the first-slice note, so paint() never takes the plugin lock
    bool dragOver_ = false;
    double fraction(int x) const { return double(x - waveBounds_.getX()) / waveBounds_.getWidth(); }
    float screenX(double position) const {
        return float(waveBounds_.getX() + waveBounds_.getWidth() * (position - viewStart_) / viewSpan_);
    }
    int selectedSlice_ = -1;
    int draggedBoundary_ = -1;
    bool updatingControls_ = false;

    static void drawPanel(juce::Graphics& g, juce::Rectangle<int> r, const char* title) {
        g.setColour(ui::panel);
        g.fillRoundedRectangle(r.toFloat(), 4.f);
        g.setColour(ui::panelEdge);
        g.drawRoundedRectangle(r.toFloat().reduced(0.5f), 4.f, 1.f);
        if (title) {
            g.setColour(ui::dim);
            g.setFont(ui::font(10.5f, true));
            g.drawText(title, r.getX() + 12, r.getY() + 5, r.getWidth() - 24, 16,
                       juce::Justification::centredLeft);
        }
    }

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

    // ---- DEV sliders: nessun limite, valori applicati dal motore al volo ----
    void setupDevControls() {
        auto& p = StftDevParams::instance();
        struct Def { const char* name; double lo, hi, init, reset; int dec; };
        const Def defs[8] = {
            {"envSmooth",       0.0, 1.0,     p.envSmooth.load(),      0.30,   3},
            {"envSmoothSlide",  0.0, 1.0,     p.envSmoothSlide.load(), 0.05,   3},
            {"maxGain",         0.0, 20.0,    p.maxGain.load(),        6.0,    3},
            {"quefrency (ms)",  0.1, 20.0,    p.quefrencyMs.load(),    2.0,    3},
            {"FFT bassa",       128, 8192,    double(p.fftLow.load()), 2048,   0},
            {"FFT alta",        128, 8192,    double(p.fftHigh.load()),512,    0},
            {"Crossover da Hz", 0.0, 20000.0, p.crossLowHz.load(),     2200.0, 1},
            {"Crossover a Hz",  0.0, 20000.0, p.crossHighHz.load(),    2800.0, 1},
        };
        for (int i = 0; i < 8; ++i) {
            auto& s = devSlider_[i];
            addChildComponent(devLabel_[i]);
            addChildComponent(s);
            setLabel(devLabel_[i], defs[i].name);
            devLabel_[i].setFont(ui::font(12.f));
            devLabel_[i].setColour(juce::Label::textColourId, ui::dim);
            s.setSliderStyle(juce::Slider::LinearHorizontal);
            s.setTextBoxStyle(juce::Slider::TextBoxRight, false, 78, 22);
            s.setNumDecimalPlacesToDisplay(defs[i].dec);
            s.setRange(std::min(defs[i].lo, defs[i].init), std::max(defs[i].hi, defs[i].init), 0.0);
            s.setValue(defs[i].init, juce::dontSendNotification);
            s.setDoubleClickReturnValue(true, defs[i].reset);
            s.onValueChange = [this] { pushDev(); };
        }
    }

    void pushDev() {
        auto& p = StftDevParams::instance();
        p.envSmooth.store(float(devSlider_[0].getValue()));
        p.envSmoothSlide.store(float(devSlider_[1].getValue()));
        p.maxGain.store(float(devSlider_[2].getValue()));
        p.quefrencyMs.store(devSlider_[3].getValue());
        p.fftLow.store(int(std::clamp(std::llround(devSlider_[4].getValue()), 128LL, 16384LL)));
        p.fftHigh.store(int(std::clamp(std::llround(devSlider_[5].getValue()), 128LL, 16384LL)));
        p.crossLowHz.store(devSlider_[6].getValue());
        p.crossHighHz.store(devSlider_[7].getValue());
        p.version.fetch_add(1, std::memory_order_release);
    }

    void updateDevVisibility() {
        for (auto* c : std::initializer_list<juce::Component*>{ &sliceModeLabel_, &sliceMode_,
                 &countLabel_, &count_, &rootLabel_, &root_ })
            c->setVisible(devPage_ == 0);
        for (int i = 0; i < 8; ++i) {
            const bool show = devPage_ != 0 && (i / 4) == devPage_ - 1;
            devLabel_[i].setVisible(show);
            devSlider_[i].setVisible(show);
        }
    }

    void pullSettings() {
        updatingControls_ = true;
        const auto s = plugin_.display().settings;
        sliceMode_.setSelectedId(s.equalSlices ? 2 : 1, juce::dontSendNotification);
        count_.setValue(s.sliceCount, juce::dontSendNotification);
        pitch_.setValue(s.pitchCents / 100.0, juce::dontSendNotification);
        root_.setValue(s.firstNote, juce::dontSendNotification);
        rootNote_ = s.firstNote;
        updatingControls_ = false;
    }

    // Only the controls that exist in the UI are written back; every other setting
    // (fallback glide, legacy slide-colour flag, MIDI offset) keeps its stored value.
    void pushSettings() {
        if (updatingControls_) return;
        auto settings = plugin_.display().settings;
        settings.equalSlices = sliceMode_.getSelectedId() == 2;
        settings.sliceCount = int(std::lround(count_.getValue()));
        settings.pitchCents = int(std::lround(pitch_.getValue() * 100.0));
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
        // A JUCE thumbnail reproduces the same waveform display as our VST3.
        // We only know the basename in Display, so the full path is passed to this
        // method by loadFile() after each import.
    }

    void refreshStatus() {
        const auto data = plugin_.display();
        if (rootNote_ != data.rootMidi) {
            rootNote_ = data.rootMidi;
            repaint(waveBounds_);
        }
        juce::String status = juce::String(data.slices) + " slices | root " +
            juce::String(data.rootMidi);
        if (data.lastMidiNote >= 0)
            status += " | MIDI " + juce::String(data.lastMidiNote) +
                      " [" + juce::String(data.lastPitchCents, 0) + "c]" +
                      " -> #" + juce::String(data.lastSlice + 1);
        if (selectedSlice_ >= 0)
            status = "Selected slice " + juce::String(selectedSlice_ + 1) +
                     " | drag a marker to adjust its boundary";
        else if (data.slices == 1 && !data.wavName.empty())
            status += "  |  Only 1 slice: try Equal divisions";
        statusLabel_.setText(status, juce::dontSendNotification);
    }

    void loadFile(const juce::File& file) {
        if (!file.existsAsFile() || !file.hasFileExtension("wav")) return;
        const std::wstring path(file.getFullPathName().toWideCharPointer());
        std::string error;
        if (!plugin_.loadWav(path, error)) {
            juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                                   "FormantSlide", juce::String::fromUTF8(error.c_str()));
            return;
        }
        selectedSlice_ = -1;
        viewStart_ = 0;
        viewSpan_ = 1;
        thumbnail_.setSource(new juce::FileInputSource(file));
        refreshFilePreview();
        refreshStatus();
        repaint();
    }

    void chooseWav() {
        auto chooser = std::make_shared<juce::FileChooser>("Choose a chromatic WAV", juce::File(), "*.wav");
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

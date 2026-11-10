#include "PluginEditor.h"

FormantSlideAudioProcessorEditor::FormantSlideAudioProcessorEditor(FormantSlideAudioProcessor& p)
    : AudioProcessorEditor(&p), processor(p)
{
    setSize(760, 485);
    thumbnailFormats.registerBasicFormats();
    thumbnail.addChangeListener(this);

    fileLabel.setJustificationType(juce::Justification::centredLeft);
    modeLabel.setText("Slide MIDI channels", juce::dontSendNotification);
    glideLabel.setText("Glide maximum (ms)", juce::dontSendNotification);
    sliceModeLabel.setText("Slicer", juce::dontSendNotification);
    countLabel.setText("Equal divisions", juce::dontSendNotification);
    rootLabel.setText("First slice MIDI note", juce::dontSendNotification);
    warningLabel.setText("Formant compensation: experimental | Exact slide duration needs MIDI look-ahead", juce::dontSendNotification);
    warningLabel.setJustificationType(juce::Justification::centred);
    warningLabel.setColour(juce::Label::textColourId, juce::Colours::orange);
    statusLabel.setJustificationType(juce::Justification::centredLeft);

    mode.addItem("Channel 5", 1);
    mode.addItem("Channels 3-16", 2);
    sliceMode.addItem("Detect silence", 1);
    sliceMode.addItem("Equal divisions", 2);

    for (auto* slider : { &glide, &count, &root })
    {
        slider->setSliderStyle(juce::Slider::LinearHorizontal);
        slider->setTextBoxStyle(juce::Slider::TextBoxRight, false, 60, 24);
    }
    count.setRange(1.0, 64.0, 1.0);
    root.setRange(0.0, 127.0, 1.0);
    glide.setRange(10.0, 2000.0, 1.0);

    addAndMakeVisible(loadButton);
    addAndMakeVisible(fileLabel);
    addAndMakeVisible(modeLabel);
    addAndMakeVisible(glideLabel);
    addAndMakeVisible(sliceModeLabel);
    addAndMakeVisible(countLabel);
    addAndMakeVisible(rootLabel);
    addAndMakeVisible(statusLabel);
    addAndMakeVisible(warningLabel);
    addAndMakeVisible(mode);
    addAndMakeVisible(sliceMode);
    addAndMakeVisible(glide);
    addAndMakeVisible(count);
    addAndMakeVisible(root);

    modeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(p.parameters, "slideMode", mode);
    sliceAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(p.parameters, "sliceMode", sliceMode);
    glideAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(p.parameters, "glideMs", glide);
    countAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(p.parameters, "sliceCount", count);
    rootAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(p.parameters, "rootNote", root);

    sliceMode.onChange = [this] { refreshSlices(); };
    count.onValueChange = [this] { refreshSlices(); };
    root.onValueChange = [this] { refreshSlices(); };

    loadButton.onClick = [this]
    {
        auto chooser = std::make_shared<juce::FileChooser>("Choose a WAV", juce::File(), "*.wav");
        juce::Component::SafePointer<FormantSlideAudioProcessorEditor> safeThis(this);
        chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
            [safeThis, chooser](const juce::FileChooser& fc)
            {
                if (safeThis != nullptr && fc.getResult().existsAsFile()) safeThis->load(fc.getResult());
            });
    };
    refreshWaveform();
    refreshSlices();
}

FormantSlideAudioProcessorEditor::~FormantSlideAudioProcessorEditor()
{
    thumbnail.removeChangeListener(this);
}

void FormantSlideAudioProcessorEditor::refreshWaveform()
{
    const auto path = processor.getLoadedFile();
    fileLabel.setText(processor.getLoadedName(), juce::dontSendNotification);
    if (path.existsAsFile()) thumbnail.setSource(new juce::FileInputSource(path));
    else thumbnail.clear();
    repaint();
}

void FormantSlideAudioProcessorEditor::refreshSlices()
{
    processor.rebuildSlices();
    const int n = processor.getSliceCount();
    const int first = (int) processor.parameters.getRawParameterValue("rootNote")->load();
    statusLabel.setText(juce::String(n) + " slices | MIDI " + juce::String(first) + " to " + juce::String(first + juce::jmax(0, n - 1)),
                        juce::dontSendNotification);
    repaint();
}

void FormantSlideAudioProcessorEditor::load(const juce::File& file)
{
    if (processor.loadWav(file))
    {
        selectedSlice = -1;
        refreshWaveform();
        refreshSlices();
    }
    else fileLabel.setText("Could not load WAV", juce::dontSendNotification);
}

bool FormantSlideAudioProcessorEditor::isInterestedInFileDrag(const juce::StringArray& files)
{
    for (const auto& f : files) if (juce::File(f).hasFileExtension("wav")) return true;
    return false;
}

void FormantSlideAudioProcessorEditor::filesDropped(const juce::StringArray& files, int, int)
{
    for (const auto& f : files) if (juce::File(f).hasFileExtension("wav")) { load(juce::File(f)); break; }
}

void FormantSlideAudioProcessorEditor::changeListenerCallback(juce::ChangeBroadcaster*) { repaint(); }

void FormantSlideAudioProcessorEditor::mouseDown(const juce::MouseEvent& event)
{
    if (!waveBounds.contains(event.getPosition())) return;
    const float x = (float) (event.x - waveBounds.getX()) / (float) waveBounds.getWidth();
    const auto fractions = processor.getSliceFractions();
    selectedSlice = -1;
    for (int i = 0; i < (int) fractions.size(); ++i)
        if (fractions[(size_t) i].contains(x)) { selectedSlice = i; break; }
    if (selectedSlice >= 0)
    {
        const int first = (int) processor.parameters.getRawParameterValue("rootNote")->load();
        statusLabel.setText("Selected slice " + juce::String(selectedSlice + 1) + " | MIDI note " + juce::String(first + selectedSlice),
                            juce::dontSendNotification);
    }
    repaint();
}

void FormantSlideAudioProcessorEditor::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff171c25));
    g.setColour(juce::Colours::white);
    g.setFont(juce::FontOptions(28.0f, juce::Font::bold));
    g.drawText("FormantSlide", 20, 14, 720, 35, juce::Justification::centred);
    g.setFont(juce::FontOptions(13.0f));
    g.setColour(juce::Colour(0xff91a8bd));
    g.drawText("CHROMATIC SLICER / MIDI SLIDES / FORMANT COMPENSATION", 20, 53, 720, 20, juce::Justification::centred);

    g.setColour(juce::Colour(0xff233044));
    g.fillRoundedRectangle(waveBounds.toFloat(), 7.0f);
    g.setColour(juce::Colour(0xff74c9ff));
    if (thumbnail.getTotalLength() > 0)
        thumbnail.drawChannels(g, waveBounds.reduced(4), 0.0, thumbnail.getTotalLength(), 0.85f);
    else
        g.drawFittedText("Load a chromatic-scale WAV to see slices", waveBounds, juce::Justification::centred, 1);

    const auto fractions = processor.getSliceFractions();
    for (int i = 0; i < (int) fractions.size(); ++i)
    {
        const auto range = fractions[(size_t) i];
        const float x1 = (float) waveBounds.getX() + range.getStart() * (float) waveBounds.getWidth();
        const float x2 = (float) waveBounds.getX() + range.getEnd() * (float) waveBounds.getWidth();
        if (i == selectedSlice)
        {
            g.setColour(juce::Colours::yellow.withAlpha(0.13f));
            g.fillRect(juce::Rectangle<float>(x1, (float) waveBounds.getY(), x2 - x1, (float) waveBounds.getHeight()));
        }
        g.setColour(juce::Colours::white.withAlpha(0.55f));
        g.drawVerticalLine(juce::roundToInt(x1), (float) waveBounds.getY(), (float) waveBounds.getBottom());
    }
}

void FormantSlideAudioProcessorEditor::resized()
{
    fileLabel.setBounds(26, 85, 540, 27);
    loadButton.setBounds(605, 83, 125, 30);
    waveBounds = { 26, 125, 704, 125 };
    statusLabel.setBounds(26, 261, 704, 26);

    sliceModeLabel.setBounds(26, 305, 140, 20);
    sliceMode.setBounds(26, 329, 205, 30);
    countLabel.setBounds(256, 305, 150, 20);
    count.setBounds(256, 330, 185, 28);
    rootLabel.setBounds(475, 305, 200, 20);
    root.setBounds(475, 330, 240, 28);

    modeLabel.setBounds(26, 375, 185, 20);
    mode.setBounds(26, 400, 205, 29);
    glideLabel.setBounds(280, 375, 205, 20);
    glide.setBounds(280, 400, 260, 28);
    warningLabel.setBounds(20, 447, 720, 24);
}

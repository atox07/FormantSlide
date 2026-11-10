#include "PluginEditor.h"
FormantSlideAudioProcessorEditor::FormantSlideAudioProcessorEditor(FormantSlideAudioProcessor& p)
    : AudioProcessorEditor(&p), processor(p)
{
    setSize(480, 300);
    fileLabel.setText(p.getLoadedName(), juce::dontSendNotification);
    fileLabel.setJustificationType(juce::Justification::centred);
    modeLabel.setText("Slide channels", juce::dontSendNotification);
    glideLabel.setText("Glide duration (prototype)", juce::dontSendNotification);
    warningLabel.setText("Prototype: resampling only. Formants are NOT preserved.", juce::dontSendNotification);
    warningLabel.setJustificationType(juce::Justification::centred);
    warningLabel.setColour(juce::Label::textColourId, juce::Colours::orange);
    mode.addItem("Channel 5", 1);
    mode.addItem("Channels 3-16", 2);
    glide.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    glide.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 85, 22);
    addAndMakeVisible(loadButton);
    addAndMakeVisible(fileLabel);
    addAndMakeVisible(modeLabel);
    addAndMakeVisible(glideLabel);
    addAndMakeVisible(warningLabel);
    addAndMakeVisible(mode);
    addAndMakeVisible(glide);
    modeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(p.parameters, "slideMode", mode);
    glideAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(p.parameters, "glideMs", glide);
    loadButton.onClick = [this]
    {
        auto chooser = std::make_shared<juce::FileChooser>("Choose a WAV", juce::File(), "*.wav");
        chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
            [this, chooser](const juce::FileChooser& fc) { if (fc.getResult().existsAsFile()) load(fc.getResult()); });
    };
}
void FormantSlideAudioProcessorEditor::load(const juce::File& file)
{
    if (processor.loadWav(file)) fileLabel.setText(file.getFileName(), juce::dontSendNotification);
    else fileLabel.setText("Could not load WAV", juce::dontSendNotification);
}
bool FormantSlideAudioProcessorEditor::isInterestedInFileDrag(const juce::StringArray& files)
{
    for (auto& f : files) if (juce::File(f).hasFileExtension("wav")) return true;
    return false;
}
void FormantSlideAudioProcessorEditor::filesDropped(const juce::StringArray& files, int, int)
{
    for (auto& f : files) if (juce::File(f).hasFileExtension("wav")) { load(juce::File(f)); break; }
}
void FormantSlideAudioProcessorEditor::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff171c25));
    g.setColour(juce::Colours::white);
    g.setFont(juce::FontOptions(27.0f, juce::Font::bold));
    g.drawText("FormantSlide", 20, 12, 440, 40, juce::Justification::centred);
    g.setColour(juce::Colour(0xff334257));
    g.drawRoundedRectangle(22.f, 64.f, 436.f, 78.f, 10.f, 2.f);
}
void FormantSlideAudioProcessorEditor::resized()
{
    fileLabel.setBounds(35, 73, 410, 25);
    loadButton.setBounds(177, 106, 126, 27);
    modeLabel.setBounds(34, 159, 160, 24);
    mode.setBounds(35, 186, 200, 30);
    glideLabel.setBounds(275, 151, 180, 24);
    glide.setBounds(315, 175, 100, 85);
    warningLabel.setBounds(20, 266, 440, 26);
}

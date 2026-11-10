#pragma once
#include "PluginProcessor.h"
class FormantSlideAudioProcessorEditor final : public juce::AudioProcessorEditor, public juce::FileDragAndDropTarget
{
public:
    explicit FormantSlideAudioProcessorEditor(FormantSlideAudioProcessor&);
    void paint(juce::Graphics&) override;
    void resized() override;
    bool isInterestedInFileDrag(const juce::StringArray&) override;
    void filesDropped(const juce::StringArray&, int, int) override;
private:
    FormantSlideAudioProcessor& processor;
    juce::TextButton loadButton { "Load WAV" };
    juce::Label fileLabel, modeLabel, glideLabel, warningLabel;
    juce::ComboBox mode;
    juce::Slider glide;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> modeAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> glideAttachment;
    void load(const juce::File&);
};

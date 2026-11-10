#pragma once
#include "PluginProcessor.h"

class FormantSlideAudioProcessorEditor final : public juce::AudioProcessorEditor,
                                                public juce::FileDragAndDropTarget,
                                                private juce::ChangeListener
{
public:
    explicit FormantSlideAudioProcessorEditor(FormantSlideAudioProcessor&);
    ~FormantSlideAudioProcessorEditor() override;
    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;
    bool isInterestedInFileDrag(const juce::StringArray&) override;
    void filesDropped(const juce::StringArray&, int, int) override;

private:
    FormantSlideAudioProcessor& processor;
    juce::TextButton loadButton { "Load WAV" };
    juce::Label fileLabel, modeLabel, glideLabel, sliceModeLabel, countLabel, rootLabel, statusLabel, warningLabel;
    juce::ComboBox mode, sliceMode;
    juce::Slider glide, count, root;
    juce::AudioFormatManager thumbnailFormats;
    juce::AudioThumbnailCache thumbnailCache { 5 };
    juce::AudioThumbnail thumbnail { 256, thumbnailFormats, thumbnailCache };
    juce::Rectangle<int> waveBounds;
    int selectedSlice = -1;

    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> modeAttachment, sliceAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> glideAttachment, countAttachment, rootAttachment;
    void load(const juce::File&);
    void refreshSlices();
    void refreshWaveform();
    void changeListenerCallback(juce::ChangeBroadcaster*) override;
};

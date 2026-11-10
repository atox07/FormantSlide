#pragma once
#include <JuceHeader.h>
#include <atomic>

class FormantSlideAudioProcessor final : public juce::AudioProcessor
{
public:
    FormantSlideAudioProcessor();
    void prepareToPlay(double, int) override;
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return "FormantSlide"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override;
    void setStateInformation(const void*, int) override;
    bool loadWav(const juce::File&);
    juce::String getLoadedName() const { return loadedName; }
    juce::AudioProcessorValueTreeState parameters;
    static juce::AudioProcessorValueTreeState::ParameterLayout makeParameters();
private:
    juce::AudioFormatManager formats;
    juce::AudioBuffer<float> sample;
    juce::String loadedName { "Drop a WAV to load" };
    juce::CriticalSection sampleLock;
    double sampleRate = 44100.0, sourceRate = 44100.0;
    double playhead = 0.0, pitchSemitones = 0.0, pitchTarget = 0.0, pitchStep = 0.0;
    int glideRemaining = 0, voiceNote = -1, slideNote = -1, slideChannel = -1;
    bool playing = false;
    void onMidi(const juce::MidiMessage&);
    void setGlideTarget(double);
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(FormantSlideAudioProcessor)
};

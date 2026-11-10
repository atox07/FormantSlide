#pragma once
#include <JuceHeader.h>
#include <signalsmith-stretch.h>
#include <vector>

class FormantSlideAudioProcessor final : public juce::AudioProcessor
{
public:
    FormantSlideAudioProcessor();
    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return "FormantSlide"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.1; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override;
    void setStateInformation(const void*, int) override;

    bool loadWav(const juce::File&);
    void rebuildSlices();
    juce::String getLoadedName() const;
    juce::File getLoadedFile() const;
    int getSliceCount() const;
    std::vector<juce::Range<float>> getSliceFractions() const;
    juce::AudioProcessorValueTreeState parameters;
    static juce::AudioProcessorValueTreeState::ParameterLayout makeParameters();

private:
    struct Slice { int start = 0; int end = 0; };
    juce::AudioFormatManager formats;
    juce::AudioBuffer<float> sample;
    juce::File loadedFile;
    mutable juce::CriticalSection sampleLock;
    std::vector<Slice> slices;
    double outputRate = 44100.0, sourceRate = 44100.0;
    double playhead = 0.0, pitchSemitones = 0.0, pitchTarget = 0.0, pitchStep = 0.0;
    int glideRemaining = 0, voiceNote = -1, slideNote = -1, slideChannel = -1;
    int currentSliceEnd = 0;
    bool playing = false;

    signalsmith::stretch::SignalsmithStretch<float> stretcher;
    juce::AudioBuffer<float> inputScratch;
    int scratchCapacity = 0;

    void onMidi(const juce::MidiMessage&);
    void setGlideTarget(double);
    void renderSpan(juce::AudioBuffer<float>& output, int start, int length);
    void rebuildSlicesLocked();
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(FormantSlideAudioProcessor)
};

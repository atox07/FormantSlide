#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <cmath>

juce::AudioProcessorValueTreeState::ParameterLayout FormantSlideAudioProcessor::makeParameters()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> p;
    p.push_back(std::make_unique<juce::AudioParameterChoice>(juce::ParameterID { "slideMode", 1 }, "Slide Channels", juce::StringArray { "Channel 5", "Channels 3-16" }, 1));
    p.push_back(std::make_unique<juce::AudioParameterFloat>(juce::ParameterID { "glideMs", 1 }, "Glide (test)", juce::NormalisableRange<float>(10.f, 1000.f, 1.f), 250.f));
    return { p.begin(), p.end() };
}
FormantSlideAudioProcessor::FormantSlideAudioProcessor()
    : AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      parameters(*this, nullptr, "Parameters", makeParameters())
{
    formats.registerBasicFormats();
}
void FormantSlideAudioProcessor::prepareToPlay(double sr, int)
{
    sampleRate = sr;
    playing = false;
    voiceNote = -1;
    pitchSemitones = pitchTarget = 0.0;
    glideRemaining = 0;
}
void FormantSlideAudioProcessor::setGlideTarget(double semitones)
{
    pitchTarget = semitones;
    glideRemaining = juce::jmax(1, (int) std::round(sampleRate * parameters.getRawParameterValue("glideMs")->load() / 1000.0));
    pitchStep = (pitchTarget - pitchSemitones) / glideRemaining;
}
void FormantSlideAudioProcessor::onMidi(const juce::MidiMessage& m)
{
    if (!m.isNoteOnOrOff()) return;
    const int channel = m.getChannel(), note = m.getNoteNumber();
    const bool voiceChannel = channel == 1 || channel == 2;
    const bool slideAllowed = parameters.getRawParameterValue("slideMode")->load() > 0.5f
        ? channel >= 3 && channel <= 16 : channel == 5;
    if (m.isNoteOn())
    {
        if (voiceChannel)
        {
            voiceNote = note;
            playing = sample.getNumSamples() > 1;
            playhead = 0.0;
            pitchSemitones = pitchTarget = 0.0;
            glideRemaining = 0;
            slideNote = slideChannel = -1;
        }
        else if (slideAllowed && playing && voiceNote >= 0)
        {
            slideNote = note;
            slideChannel = channel;
            setGlideTarget(note - voiceNote);
        }
    }
    else if (voiceChannel && note == voiceNote)
    {
        playing = false;
        voiceNote = -1;
        glideRemaining = 0;
    }
    else if (slideAllowed && note == slideNote && channel == slideChannel)
    {
        // Retain current trajectory/target after note-off in this prototype.
        slideNote = slideChannel = -1;
    }
}
void FormantSlideAudioProcessor::processBlock(juce::AudioBuffer<float>& output, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    output.clear();
    // A non-blocking lock prevents the audio thread from waiting for a file load.
    const juce::ScopedTryLock lock(sampleLock);
    if (!lock.isLocked()) return;
    juce::MidiBuffer::Iterator it(midi);
    juce::MidiMessage message;
    int eventPosition = 0;
    bool hasEvent = it.getNextEvent(message, eventPosition);
    for (int i = 0; i < output.getNumSamples(); ++i)
    {
        while (hasEvent && eventPosition <= i)
        {
            onMidi(message);
            hasEvent = it.getNextEvent(message, eventPosition);
        }
        if (glideRemaining > 0)
        {
            pitchSemitones += pitchStep;
            if (--glideRemaining == 0) pitchSemitones = pitchTarget;
        }
        if (!playing || sample.getNumSamples() < 2) continue;
        const int index = (int) playhead;
        if (index + 1 >= sample.getNumSamples()) { playing = false; continue; }
        const float frac = (float) (playhead - index);
        for (int c = 0; c < output.getNumChannels(); ++c)
        {
            const int sourceChannel = juce::jmin(c, sample.getNumChannels() - 1);
            const float* data = sample.getReadPointer(sourceChannel);
            output.setSample(c, i, data[index] + frac * (data[index + 1] - data[index]));
        }
        playhead += (sourceRate / sampleRate) * std::pow(2.0, pitchSemitones / 12.0);
    }
    midi.clear();
}
bool FormantSlideAudioProcessor::loadWav(const juce::File& file)
{
    if (!file.existsAsFile() || !file.hasFileExtension("wav")) return false;
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
    if (!reader || reader->lengthInSamples < 2 || reader->lengthInSamples > 44100LL * 60 * 10) return false;
    juce::AudioBuffer<float> newSample((int) juce::jmin((unsigned int) 2, reader->numChannels), (int) reader->lengthInSamples);
    if (!reader->read(&newSample, 0, newSample.getNumSamples(), 0, true, true)) return false;
    const juce::ScopedLock lock(sampleLock);
    playing = false;
    sample = std::move(newSample);
    sourceRate = reader->sampleRate;
    loadedName = file.getFileName();
    return true;
}
void FormantSlideAudioProcessor::getStateInformation(juce::MemoryBlock& dest)
{
    auto state = parameters.copyState();
    state.setProperty("samplePath", loadedName, nullptr); // Display only; samples are not embedded yet.
    std::unique_ptr<juce::XmlElement> xml(state.createXml());
    copyXmlToBinary(*xml, dest);
}
void FormantSlideAudioProcessor::setStateInformation(const void* data, int size)
{
    std::unique_ptr<juce::XmlElement> xml(getXmlFromBinary(data, size));
    if (xml && xml->hasTagName(parameters.state.getType())) parameters.replaceState(juce::ValueTree::fromXml(*xml));
}
juce::AudioProcessorEditor* FormantSlideAudioProcessor::createEditor() { return new FormantSlideAudioProcessorEditor(*this); }
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new FormantSlideAudioProcessor(); }

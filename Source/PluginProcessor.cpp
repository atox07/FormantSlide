#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <algorithm>
#include <cmath>

juce::AudioProcessorValueTreeState::ParameterLayout FormantSlideAudioProcessor::makeParameters()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> p;
    p.push_back(std::make_unique<juce::AudioParameterChoice>(juce::ParameterID { "slideMode", 1 }, "Slide channels", juce::StringArray { "Channel 5", "Channels 3-16" }, 1));
    p.push_back(std::make_unique<juce::AudioParameterFloat>(juce::ParameterID { "glideMs", 1 }, "Glide (ms)", juce::NormalisableRange<float>(10.f, 2000.f, 1.f), 250.f));
    p.push_back(std::make_unique<juce::AudioParameterChoice>(juce::ParameterID { "sliceMode", 1 }, "Slice mode", juce::StringArray { "Detect silence", "Equal divisions" }, 0));
    p.push_back(std::make_unique<juce::AudioParameterInt>(juce::ParameterID { "sliceCount", 1 }, "Equal slice count", 1, 64, 12));
    p.push_back(std::make_unique<juce::AudioParameterInt>(juce::ParameterID { "rootNote", 1 }, "First slice MIDI note", 0, 127, 60));
    return { p.begin(), p.end() };
}

FormantSlideAudioProcessor::FormantSlideAudioProcessor()
    : AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      parameters(*this, nullptr, "Parameters", makeParameters())
{
    formats.registerBasicFormats();
}

void FormantSlideAudioProcessor::prepareToPlay(double sr, int maxBlock)
{
    const juce::ScopedLock lock(sampleLock);
    outputRate = sr;
    scratchCapacity = juce::jmax(32, maxBlock);
    inputScratch.setSize(2, scratchCapacity, false, true, false);
    // A relatively short spectral window keeps this first live test responsive.
    stretcher.configure(2, 1024, 192, false);
    stretcher.setFormantBase(200.0f / (float) outputRate);
    stretcher.setFormantFactor(1.0f, true); // Compensate for transposition.
    stretcher.setTransposeSemitones(0.0f);
    setLatencySamples(stretcher.outputLatency());
    playing = false;
    voiceNote = -1;
    glideRemaining = 0;
    pitchSemitones = pitchTarget = 0.0;
}

void FormantSlideAudioProcessor::setGlideTarget(double semitones)
{
    pitchTarget = semitones;
    glideRemaining = juce::jmax(1, (int) std::round(outputRate * parameters.getRawParameterValue("glideMs")->load() / 1000.0));
    pitchStep = (pitchTarget - pitchSemitones) / (double) glideRemaining;
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
            slideNote = slideChannel = -1;
            glideRemaining = 0;
            pitchSemitones = pitchTarget = 0.0;
            const int firstNote = (int) parameters.getRawParameterValue("rootNote")->load();
            const int sliceIndex = slices.empty() ? 0 : juce::jlimit(0, (int) slices.size() - 1, note - firstNote);
            playing = !slices.empty() && sample.getNumSamples() > 1;
            if (playing)
            {
                const Slice s = slices[(size_t) sliceIndex];
                playhead = (double) s.start;
                currentSliceEnd = s.end;
                // Outside the mapped range, transpose the nearest slice.
                pitchSemitones = pitchTarget = (double) (note - (firstNote + sliceIndex));
                // Reset spectral history on a fresh vocal attack.
                stretcher.reset();
            }
        }
        else if (slideAllowed && playing && voiceNote >= 0)
        {
            slideNote = note;
            slideChannel = channel;
            const int firstNote = (int) parameters.getRawParameterValue("rootNote")->load();
            const int originalSlice = juce::jlimit(0, juce::jmax(0, (int) slices.size() - 1), voiceNote - firstNote);
            const double baseOffset = voiceNote - (firstNote + originalSlice);
            setGlideTarget(baseOffset + note - voiceNote);
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
        // A short MIDI note stops the glide earlier; it cannot define the
        // exact full-length ramp unless the Note Off is known in advance.
        glideRemaining = 0;
        pitchTarget = pitchSemitones;
        slideNote = slideChannel = -1;
    }
}

void FormantSlideAudioProcessor::renderSpan(juce::AudioBuffer<float>& output, int start, int length)
{
    // Keep transpose automation smooth without per-sample spectral calls.
    while (length > 0)
    {
        const int run = std::min({ length, scratchCapacity, 64 });
        inputScratch.clear(0, run);
        for (int i = 0; i < run; ++i)
        {
            if (glideRemaining > 0)
            {
                pitchSemitones += pitchStep;
                if (--glideRemaining == 0) pitchSemitones = pitchTarget;
            }
            if (!playing || sample.getNumSamples() < 2) continue;
            const int index = (int) playhead;
            if (index + 1 >= currentSliceEnd || index + 1 >= sample.getNumSamples())
            {
                playing = false;
                continue;
            }
            const float frac = (float) (playhead - (double) index);
            const int fadeLength = juce::jmax(8, (int) (sourceRate * 0.005));
            const float tailGain = juce::jlimit(0.0f, 1.0f, (float) (currentSliceEnd - playhead) / (float) fadeLength);
            for (int c = 0; c < 2; ++c)
            {
                const int sourceChannel = juce::jmin(c, sample.getNumChannels() - 1);
                const float* data = sample.getReadPointer(sourceChannel);
                inputScratch.setSample(c, i, (data[index] + frac * (data[index + 1] - data[index])) * tailGain);
            }
            playhead += sourceRate / outputRate; // Independent of pitch!
        }
        stretcher.setTransposeSemitones((float) pitchSemitones);
        float* source[] = { inputScratch.getWritePointer(0), inputScratch.getWritePointer(1) };
        float* destination[] = { output.getWritePointer(0, start), output.getWritePointer(1, start) };
        stretcher.process(source, run, destination, run);
        start += run;
        length -= run;
    }
}

void FormantSlideAudioProcessor::processBlock(juce::AudioBuffer<float>& output, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    output.clear();
    const juce::ScopedTryLock lock(sampleLock);
    if (!lock.isLocked() || scratchCapacity <= 0) { midi.clear(); return; }
    int cursor = 0;
    for (const auto metadata : midi)
    {
        const int eventPosition = juce::jlimit(0, output.getNumSamples(), metadata.samplePosition);
        if (eventPosition > cursor) renderSpan(output, cursor, eventPosition - cursor);
        onMidi(metadata.getMessage());
        cursor = eventPosition;
    }
    if (cursor < output.getNumSamples()) renderSpan(output, cursor, output.getNumSamples() - cursor);
    midi.clear();
}

void FormantSlideAudioProcessor::rebuildSlicesLocked()
{
    slices.clear();
    const int total = sample.getNumSamples();
    if (total < 2) return;
    const bool equal = parameters.getRawParameterValue("sliceMode")->load() > 0.5f;
    if (equal)
    {
        const int count = juce::jlimit(1, juce::jmin(64, total / 2), (int) parameters.getRawParameterValue("sliceCount")->load());
        for (int i = 0; i < count; ++i)
        {
            const int a = (int) ((int64_t) i * total / count);
            const int b = (int) ((int64_t) (i + 1) * total / count);
            if (b - a >= 2) slices.push_back({ a, b });
        }
    }
    else
    {
        // Detect groups of audio separated by at least 28 ms of near-silence.
        const int window = juce::jmax(1, (int) (sourceRate * 0.003));
        const int minGap = juce::jmax(1, (int) (sourceRate * 0.028));
        const int minSlice = juce::jmax(2, (int) (sourceRate * 0.042));
        const int padding = juce::jmax(0, (int) (sourceRate * 0.003));
        float peak = 0.0f;
        for (int c = 0; c < sample.getNumChannels(); ++c)
            peak = juce::jmax(peak, sample.getMagnitude(c, 0, total));
        const float threshold = juce::jmax(0.0007f, peak * 0.017f);
        int segmentStart = -1, lastActiveEnd = 0;
        for (int start = 0; start < total; start += window)
        {
            const int end = juce::jmin(total, start + window);
            float magnitude = 0.0f;
            for (int c = 0; c < sample.getNumChannels(); ++c)
                magnitude = juce::jmax(magnitude, sample.getMagnitude(c, start, end - start));
            if (magnitude >= threshold)
            {
                if (segmentStart < 0) segmentStart = start;
                lastActiveEnd = end;
            }
            else if (segmentStart >= 0 && start - lastActiveEnd >= minGap)
            {
                if (lastActiveEnd - segmentStart >= minSlice)
                    slices.push_back({ juce::jmax(0, segmentStart - padding), juce::jmin(total, lastActiveEnd + padding) });
                segmentStart = -1;
            }
        }
        if (segmentStart >= 0 && lastActiveEnd - segmentStart >= minSlice)
            slices.push_back({ juce::jmax(0, segmentStart - padding), juce::jmin(total, lastActiveEnd + padding) });
        if (slices.empty()) slices.push_back({ 0, total });
    }
    playing = false;
}

void FormantSlideAudioProcessor::rebuildSlices()
{
    const juce::ScopedLock lock(sampleLock);
    rebuildSlicesLocked();
}

bool FormantSlideAudioProcessor::loadWav(const juce::File& file)
{
    if (!file.existsAsFile() || !file.hasFileExtension("wav")) return false;
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
    if (!reader || reader->lengthInSamples < 2 || reader->lengthInSamples > 44100LL * 60 * 10 || reader->numChannels < 1) return false;
    juce::AudioBuffer<float> newSample((int) juce::jmin((unsigned int) 2, reader->numChannels), (int) reader->lengthInSamples);
    if (!reader->read(&newSample, 0, newSample.getNumSamples(), 0, true, true)) return false;
    const juce::ScopedLock lock(sampleLock);
    playing = false;
    sample = std::move(newSample);
    sourceRate = reader->sampleRate;
    loadedFile = file;
    rebuildSlicesLocked();
    return true;
}

juce::String FormantSlideAudioProcessor::getLoadedName() const
{
    const juce::ScopedLock lock(sampleLock);
    return loadedFile.existsAsFile() ? loadedFile.getFileName() : "Drop a chromatic WAV";
}

juce::File FormantSlideAudioProcessor::getLoadedFile() const
{
    const juce::ScopedLock lock(sampleLock);
    return loadedFile;
}

int FormantSlideAudioProcessor::getSliceCount() const
{
    const juce::ScopedLock lock(sampleLock);
    return (int) slices.size();
}

std::vector<juce::Range<float>> FormantSlideAudioProcessor::getSliceFractions() const
{
    const juce::ScopedLock lock(sampleLock);
    std::vector<juce::Range<float>> result;
    const float total = (float) juce::jmax(1, sample.getNumSamples());
    result.reserve(slices.size());
    for (auto s : slices) result.emplace_back(s.start / total, s.end / total);
    return result;
}

void FormantSlideAudioProcessor::getStateInformation(juce::MemoryBlock& dest)
{
    auto state = parameters.copyState();
    state.setProperty("samplePath", getLoadedFile().getFullPathName(), nullptr);
    std::unique_ptr<juce::XmlElement> xml(state.createXml());
    copyXmlToBinary(*xml, dest);
}

void FormantSlideAudioProcessor::setStateInformation(const void* data, int size)
{
    std::unique_ptr<juce::XmlElement> xml(getXmlFromBinary(data, size));
    if (!xml || !xml->hasTagName(parameters.state.getType())) return;
    auto restored = juce::ValueTree::fromXml(*xml);
    const juce::File path(restored.getProperty("samplePath").toString());
    parameters.replaceState(restored);
    if (path.existsAsFile()) loadWav(path);
}

juce::AudioProcessorEditor* FormantSlideAudioProcessor::createEditor() { return new FormantSlideAudioProcessorEditor(*this); }
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new FormantSlideAudioProcessor(); }

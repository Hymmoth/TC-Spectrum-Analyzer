#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
    juce::NormalisableRange<float> logRange (float lo, float hi)
    {
        return { lo, hi,
                 [] (float start, float end, float v) { return start * std::pow (end / start, v); },
                 [] (float start, float end, float x) { return std::log (x / start) / std::log (end / start); } };
    }
}

float TCSpectrumAnalyzerProcessor::fallSecondsFromNormalised (float v) noexcept
{
    if (v >= 0.995f)
        return 0.0f; // infinite
    return 0.05f * std::pow (600.0f, v / 0.995f); // 50 ms ... 30 s
}

juce::AudioProcessorValueTreeState::ParameterLayout TCSpectrumAnalyzerProcessor::createLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { ParamIDs::detect, 1 }, "Detect",
                                                        StringArray { "Peak", "RMS" }, 0));

    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamIDs::response, 1 }, "Response",
        logRange (10.0f, 3000.0f), 160.0f,
        AudioParameterFloatAttributes().withLabel ("ms")
            .withStringFromValueFunction ([] (float v, int) { return v >= 1000.0f ? String (v / 1000.0f, 2) + " s" : String (roundToInt (v)) + " ms"; })
            .withValueFromStringFunction ([] (const String& s) { auto v = s.getFloatValue(); return s.containsIgnoreCase ("s") && ! s.containsIgnoreCase ("ms") ? v * 1000.0f : v; })));

    layout.add (std::make_unique<AudioParameterBool> (ParameterID { ParamIDs::peakHold, 1 }, "Peak Hold", true));

    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamIDs::fall, 1 }, "Peak Fall",
        NormalisableRange<float> (0.0f, 1.0f), 1.0f,
        AudioParameterFloatAttributes()
            .withStringFromValueFunction ([] (float v, int)
            {
                const float s = fallSecondsFromNormalised (v);
                if (s <= 0.0f) return String (CharPointer_UTF8 ("\xe2\x88\x9e"));
                return s < 1.0f ? String (roundToInt (s * 1000.0f)) + " ms" : String (s, s < 10.0f ? 1 : 0) + " s";
            })));

    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { ParamIDs::lfRes, 1 }, "LF Res",
                                                        StringArray { "10 Hz", "20 Hz", "40 Hz" }, 1));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { ParamIDs::weighting, 1 }, "Weighting",
                                                        StringArray { "Flat", "A", "B", "C" }, 0));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { ParamIDs::show, 1 }, "Show",
                                                        StringArray { "Channels", "Sum", "Channels + Sum" }, 0));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { ParamIDs::kScale, 1 }, "K-Scale",
                                                        StringArray { "K-12", "K-14", "K-20" }, 1));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { ParamIDs::meter, 1 }, "Show Meter", true));

    return layout;
}

TCSpectrumAnalyzerProcessor::TCSpectrumAnalyzerProcessor()
    : AudioProcessor (BusesProperties()
                        .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                        .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "TCSA", createLayout())
{
    for (auto* id : { ParamIDs::detect, ParamIDs::response, ParamIDs::lfRes })
    {
        apvts.addParameterListener (id, this);
        parameterChanged (id, apvts.getRawParameterValue (id)->load());
    }
    updateChannelNames();
}

TCSpectrumAnalyzerProcessor::~TCSpectrumAnalyzerProcessor()
{
    for (auto* id : { ParamIDs::detect, ParamIDs::response, ParamIDs::lfRes })
        apvts.removeParameterListener (id, this);
    analyzer.release();
}

void TCSpectrumAnalyzerProcessor::parameterChanged (const juce::String& id, float v)
{
    if (id == ParamIDs::detect)        analyzer.detectRms.store ((int) v);
    else if (id == ParamIDs::response) analyzer.responseMs.store (v);
    else if (id == ParamIDs::lfRes)    analyzer.lfResHz.store (v < 0.5f ? 10 : (v < 1.5f ? 20 : 40));
}

bool TCSpectrumAnalyzerProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& in  = layouts.getMainInputChannelSet();
    const auto& out = layouts.getMainOutputChannelSet();
    if (in.isDisabled() || in != out)
        return false;
    return in.size() >= 1 && in.size() <= Analyzer::maxChannels;
}

void TCSpectrumAnalyzerProcessor::updateChannelNames()
{
    juce::StringArray names;
    if (auto* bus = getBus (true, 0))
    {
        const auto set = bus->getCurrentLayout();
        for (int i = 0; i < set.size(); ++i)
        {
            auto n = juce::AudioChannelSet::getAbbreviatedChannelTypeName (set.getTypeOfChannel (i));
            if (n.isEmpty() || n == "?") n = juce::String (i + 1);
            if (n.equalsIgnoreCase ("lfe") || n.equalsIgnoreCase ("lfe1")) n = "LFE";
            names.add (n);
        }
    }
    if (names.isEmpty())
        names = { "L", "R" };

    {
        const juce::SpinLock::ScopedLockType sl (namesLock);
        if (names == channelNames)
            return;
        channelNames = names;
    }
    channelLayoutVersion.fetch_add (1);
}

juce::StringArray TCSpectrumAnalyzerProcessor::getChannelNames() const
{
    const juce::SpinLock::ScopedLockType sl (namesLock);
    return channelNames;
}

void TCSpectrumAnalyzerProcessor::processorLayoutsChanged()
{
    updateChannelNames();
}

void TCSpectrumAnalyzerProcessor::prepareToPlay (double sampleRate, int)
{
    updateChannelNames();
    analyzer.prepare (sampleRate, getTotalNumInputChannels());
}

void TCSpectrumAnalyzerProcessor::releaseResources()
{
    analyzer.release();
}

void TCSpectrumAnalyzerProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    // audio passes through untouched; we only copy it for analysis
    analyzer.push (buffer);
}

juce::AudioProcessorEditor* TCSpectrumAnalyzerProcessor::createEditor()
{
    return new TCSpectrumAnalyzerEditor (*this);
}

void TCSpectrumAnalyzerProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    state.setProperty ("hiddenChannels", (int) hiddenChannels.load(), nullptr);
    state.setProperty ("editorWidth", editorWidth.load(), nullptr);
    state.setProperty ("editorHeight", editorHeight.load(), nullptr);
    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void TCSpectrumAnalyzerProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
    {
        auto state = juce::ValueTree::fromXml (*xml);
        if (state.hasType (apvts.state.getType()))
        {
            hiddenChannels.store ((juce::uint32) (int) state.getProperty ("hiddenChannels", 0));
            editorWidth.store ((int) state.getProperty ("editorWidth", 1100));
            editorHeight.store ((int) state.getProperty ("editorHeight", 640));
            apvts.replaceState (state);
        }
    }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new TCSpectrumAnalyzerProcessor();
}

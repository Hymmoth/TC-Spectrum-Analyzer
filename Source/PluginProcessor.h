#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "Analyzer.h"

namespace ParamIDs
{
    inline constexpr const char* detect    = "detect";
    inline constexpr const char* response  = "response";
    inline constexpr const char* peakHold  = "peakHold";
    inline constexpr const char* fall      = "fall";
    inline constexpr const char* lfRes     = "lfRes";
    inline constexpr const char* weighting = "weighting";
    inline constexpr const char* show      = "show";
    inline constexpr const char* kScale    = "kScale";
    inline constexpr const char* meter     = "meterVisible";
}

class TCSpectrumAnalyzerProcessor : public juce::AudioProcessor,
                                    private juce::AudioProcessorValueTreeState::Listener
{
public:
    TCSpectrumAnalyzerProcessor();
    ~TCSpectrumAnalyzerProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;
    void processorLayoutsChanged() override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "TC Spectrum Analyzer"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    // channel names of the current bus ("L", "R", "C", "LFE", ...)
    juce::StringArray getChannelNames() const;
    int getChannelLayoutVersion() const noexcept { return channelLayoutVersion.load(); }

    // fall time in seconds for the spectrum peak hold, 0 = infinite
    static float fallSecondsFromNormalised (float v) noexcept;

    juce::AudioProcessorValueTreeState apvts;
    Analyzer analyzer;

    // UI state stored with the project (not automatable)
    std::atomic<juce::uint32> hiddenChannels { 0 };
    std::atomic<int> editorWidth { 1100 }, editorHeight { 640 };

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    void parameterChanged (const juce::String& id, float newValue) override;
    void updateChannelNames();

    mutable juce::SpinLock namesLock;
    juce::StringArray channelNames;
    std::atomic<int> channelLayoutVersion { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TCSpectrumAnalyzerProcessor)
};

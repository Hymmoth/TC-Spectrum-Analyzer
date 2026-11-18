#pragma once

#include "PluginProcessor.h"
#include "Theme.h"

// K-System meter (Bob Katz): AES17 RMS bar (600 ms window) + true-peak line, one bar per channel.
class MeterView : public juce::Component
{
public:
    explicit MeterView (TCSpectrumAnalyzerProcessor&);

    void tick (double dtSeconds);
    void resetPeaks();
    void refreshColours();

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseUp (const juce::MouseEvent&) override;

private:
    float dbToY (float dbfs) const noexcept;

    TCSpectrumAnalyzerProcessor& proc;
    Analyzer& analyzer;
    juce::SharedResourcePointer<GlobalSettings> settings;
    SegmentedControl kButtons;

    juce::Rectangle<int> header, scaleArea, barsArea, labelArea, tpBox, rmsBox;

    juce::StringArray names;
    std::vector<juce::Colour> colours;
    int seenChannelVersion = -1, seenColourVersion = -1;

    static constexpr float range = 60.0f; // dB shown below 0 dBFS
    std::array<float, Analyzer::maxChannels> rms {}, peak {}, peakHoldTime {};
    std::atomic<float>* kParam = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MeterView)
};

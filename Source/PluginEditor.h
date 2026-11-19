#pragma once

#include "PluginProcessor.h"
#include "SpectrumView.h"
#include "MeterView.h"
#include "Theme.h"

class TCSpectrumAnalyzerEditor : public juce::AudioProcessorEditor
{
public:
    explicit TCSpectrumAnalyzerEditor (TCSpectrumAnalyzerProcessor&);
    ~TCSpectrumAnalyzerEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    struct Group
    {
        juce::String caption;
        juce::Component* a = nullptr;
        juce::Component* b = nullptr;
        int widthA = 0, widthB = 0;
        juce::Rectangle<int> captionArea;
        juce::Label* valueLabel = nullptr; // optional value shown next to the caption
    };

    void tick();
    void showColourMenu();
    void layoutBottom (juce::Rectangle<int> area);
    int bottomRowsNeeded (int width) const;

    TCSpectrumAnalyzerProcessor& proc;
    TCLookAndFeel lnf;

    SpectrumView spectrum;
    MeterView meter;

    juce::TextButton freezeButton { "Freeze" }, resetButton { "Reset Peaks" },
                     coloursButton { "Colors..." }, meterButton { "Meter" };
    std::unique_ptr<juce::ButtonParameterAttachment> meterAttachment;

    SegmentedControl detect, lfRes, weighting, show;
    juce::Slider response, fall;
    juce::TextButton peakHoldButton;
    juce::Label responseValue, fallValue;
    std::unique_ptr<juce::SliderParameterAttachment> responseAttachment, fallAttachment;
    std::unique_ptr<juce::ButtonParameterAttachment> holdAttachment;

    std::vector<Group> groups;
    juce::Rectangle<int> topBar, badgeArea;
    juce::String busText;
    int seenChannelVersion = -1;

    double lastTick = 0.0;
    juce::VBlankAttachment vblank;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TCSpectrumAnalyzerEditor)
};

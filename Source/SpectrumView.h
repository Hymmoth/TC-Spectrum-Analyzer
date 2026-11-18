#pragma once

#include "PluginProcessor.h"
#include "Theme.h"

class SpectrumView : public juce::Component
{
public:
    explicit SpectrumView (TCSpectrumAnalyzerProcessor&);
    ~SpectrumView() override;

    // called once per frame by the editor
    void tick (double dtSeconds);

    void setFrozen (bool f) { frozen = f; }
    void resetHold();
    void resetView();
    void refreshColours();

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    class Chip;

    void rebuildChips();
    void rebuildWeights();
    float freqToX (float f) const noexcept;
    float xToFreq (float x) const noexcept;
    float dbToY (float db) const noexcept;
    float yToDb (float y) const noexcept;
    juce::Path makeCurve (const float* db, int n, bool closed) const;
    static float weightingDb (int type, double f);

    TCSpectrumAnalyzerProcessor& proc;
    Analyzer& analyzer;
    juce::SharedResourcePointer<GlobalSettings> settings;

    juce::OwnedArray<Chip> chips;
    juce::TextButton resetViewButton { "Reset View" };
    juce::Rectangle<int> chipRow, plot, freqLabels, dbLabels;

    juce::StringArray names;
    std::vector<juce::Colour> colours;
    int seenChannelVersion = -1, seenLayoutVersion = -1, seenWeighting = -1, seenColourVersion = -1;

    int numBands = 0;
    std::array<float, Analyzer::maxBands> freqs {};
    std::array<float, Analyzer::maxBands> weights {};      // linear power gain
    std::array<std::array<float, Analyzer::maxBands>, Analyzer::maxChannels> curDb {}, holdDb {};
    std::array<float, Analyzer::maxBands> sumDb {}, sumHoldDb {};

    bool frozen = false;
    float dbTop = 0.0f, dbBottom = -80.0f;
    float fLo = 16.0f, fHi = 22000.0f;

    juce::Point<float> mousePos;
    bool mouseInPlot = false;
    juce::Point<float> dragStart;
    float dragDbTop = 0, dragDbBottom = 0, dragFLo = 0, dragFHi = 0;

    std::atomic<float>* showParam = nullptr;
    std::atomic<float>* holdParam = nullptr;
    std::atomic<float>* fallParam = nullptr;
    std::atomic<float>* weightParam = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpectrumView)
};

#include "MeterView.h"

MeterView::MeterView (TCSpectrumAnalyzerProcessor& p)
    : proc (p), analyzer (p.analyzer), kButtons (*p.apvts.getParameter (ParamIDs::kScale))
{
    kParam = proc.apvts.getRawParameterValue (ParamIDs::kScale);
    addAndMakeVisible (kButtons);
    rms.fill (-200.0f);
    peak.fill (-200.0f);
    peakHoldTime.fill (0.0f);
}

void MeterView::refreshColours()
{
    colours.clear();
    for (int i = 0; i < names.size(); ++i)
        colours.push_back (settings->getChannelColour (names[i], i));
    seenColourVersion = settings->getVersion();
}

void MeterView::resetPeaks()
{
    peak.fill (-200.0f);
    peakHoldTime.fill (0.0f);
}

void MeterView::tick (double dt)
{
    if (proc.getChannelLayoutVersion() != seenChannelVersion)
    {
        seenChannelVersion = proc.getChannelLayoutVersion();
        names = proc.getChannelNames();
        refreshColours();
        resetPeaks();
    }
    if (settings->getVersion() != seenColourVersion)
        refreshColours();

    const int numCh = juce::jmin (names.size(), analyzer.getNumChannels());
    for (int ch = 0; ch < numCh; ++ch)
    {
        rms[(size_t) ch] = analyzer.getRmsDb (ch);
        const float tp = analyzer.takeTruePeakDb (ch);
        float& pk = peak[(size_t) ch];
        float& ht = peakHoldTime[(size_t) ch];
        if (tp >= pk) { pk = tp; ht = 2.0f; }          // hold 2 s
        else if ((ht -= (float) dt) <= 0.0f)
            pk = juce::jmax (-200.0f, pk - (float) (20.0 * dt)); // then fall 20 dB/s
    }
    repaint();
}

float MeterView::dbToY (float dbfs) const noexcept
{
    const float t = juce::jlimit (0.0f, 1.0f, -dbfs / range);
    return (float) barsArea.getY() + t * (float) barsArea.getHeight();
}

void MeterView::resized()
{
    auto r = getLocalBounds().reduced (10, 8);
    header = r.removeFromTop (26);
    kButtons.setBounds (header.removeFromRight (156));
    r.removeFromTop (12);

    auto readouts = r.removeFromBottom (42);
    tpBox  = readouts.removeFromLeft (readouts.getWidth() / 2 - 3);
    readouts.removeFromLeft (6);
    rmsBox = readouts;
    r.removeFromBottom (6);

    labelArea = r.removeFromBottom (16);
    scaleArea = r.removeFromLeft (30);
    r.removeFromLeft (4);
    barsArea = r.withTrimmedTop (6);
    labelArea = labelArea.withLeft (barsArea.getX()).withRight (barsArea.getRight());
}

void MeterView::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();
    g.setColour (Theme::panel);
    g.fillRoundedRectangle (bounds, 6.0f);
    g.setColour (Theme::panelBorder);
    g.drawRoundedRectangle (bounds.reduced (0.5f), 6.0f, 1.0f);

    g.setColour (Theme::textBright);
    g.setFont (Theme::font (13.0f, true));
    g.drawText ("K-Meter", header, juce::Justification::centredLeft);

    const int kIdx = juce::roundToInt (kParam->load());
    const float N = kIdx == 0 ? 12.0f : (kIdx == 1 ? 14.0f : 20.0f);

    // ---- scale (K units) ----
    g.setFont (Theme::mono (10.5f));
    for (float k : { N, 8.0f, 4.0f, 0.0f, -4.0f, -8.0f, -12.0f, -20.0f, -30.0f, -40.0f })
    {
        if (k > N || k - N < -range) continue;
        const float y = dbToY (k - N);
        g.setColour (k == 0.0f ? Theme::textBright : Theme::textDim);
        g.drawText ((k > 0 ? "+" : "") + juce::String ((int) k), scaleArea.getX(), (int) y - 7, scaleArea.getWidth() - 2, 14, juce::Justification::centredRight);
        g.setColour (Theme::gridMinor);
        g.fillRect ((float) barsArea.getX(), std::round (y), (float) barsArea.getWidth(), 1.0f);
    }

    // ---- bars ----
    const int numCh = juce::jmin (names.size(), (int) colours.size(), analyzer.getNumChannels());
    if (numCh > 0)
    {
        const float gap = numCh > 8 ? 3.0f : 4.0f;
        const float bw = juce::jmin (28.0f, ((float) barsArea.getWidth() - gap * (float) (numCh - 1)) / (float) numCh);
        const float bottom = (float) barsArea.getBottom();
        const float yZero = dbToY (-N), yPlus4 = dbToY (-N + 4.0f);

        for (int ch = 0; ch < numCh; ++ch)
        {
            const float x = (float) barsArea.getX() + (float) ch * (bw + gap);
            g.setColour (juce::Colour (0xff222529));
            g.fillRect (x, (float) barsArea.getY(), bw, (float) barsArea.getHeight());

            const float yl = dbToY (rms[(size_t) ch]);
            g.setColour (Theme::meterGreen);
            const float gTop = juce::jmax (yl, yZero);
            g.fillRect (x, gTop, bw, bottom - gTop);
            if (yl < yZero)
            {
                g.setColour (Theme::meterYellow);
                const float yTop = juce::jmax (yl, yPlus4);
                g.fillRect (x, yTop, bw, yZero - yTop);
            }
            if (yl < yPlus4)
            {
                g.setColour (Theme::meterRed);
                g.fillRect (x, yl, bw, yPlus4 - yl);
            }

            if (peak[(size_t) ch] > -range)
            {
                g.setColour (peak[(size_t) ch] > -0.1f ? Theme::meterRed : juce::Colour (0xfff0f2f5));
                g.fillRect (x, dbToY (peak[(size_t) ch]) - 1.0f, bw, 2.0f);
            }

            g.setColour (colours[(size_t) ch]);
            g.setFont (Theme::font (bw < 14.0f ? 9.0f : 11.0f, true));
            g.drawFittedText (names[ch], juce::Rectangle<int> ((int) x - 4, labelArea.getY(), (int) bw + 8, labelArea.getHeight()),
                              juce::Justification::centred, 1, 0.6f);
        }
    }

    // ---- readouts (click to reset) ----
    auto drawBox = [&g] (juce::Rectangle<int> r, const juce::String& title, const juce::String& value, juce::Colour c)
    {
        g.setColour (juce::Colour (0xff212428));
        g.fillRoundedRectangle (r.toFloat(), 4.0f);
        auto inner = r.reduced (8, 4);
        g.setColour (Theme::textDim);
        g.setFont (Theme::font (10.5f));
        g.drawText (title, inner.removeFromTop (14), juce::Justification::centredLeft);
        g.setColour (c);
        g.setFont (Theme::mono (14.0f));
        g.drawText (value, inner, juce::Justification::centredLeft);
    };

    const float tpMax = analyzer.getTruePeakMaxDb();
    const float rmsMax = analyzer.getRmsMaxDb();
    const auto tpText = tpMax > -150.0f ? juce::String (tpMax, 1) + " dBTP" : juce::String ("-inf");
    const float rmsK = rmsMax + N;
    const auto rmsText = rmsMax > -150.0f ? (rmsK > 0 ? "+" : "") + juce::String (rmsK, 1) + " K" : juce::String ("-inf");
    drawBox (tpBox, "True Peak Max", tpText, tpMax > -0.1f ? Theme::meterRed : juce::Colour (0xffffb27a));
    drawBox (rmsBox, "RMS Max", rmsText, juce::Colour (0xff8fd3a8));
}

void MeterView::mouseUp (const juce::MouseEvent& e)
{
    if (tpBox.contains (e.getPosition()) || rmsBox.contains (e.getPosition()))
    {
        analyzer.resetMeterMax();
        resetPeaks();
    }
}

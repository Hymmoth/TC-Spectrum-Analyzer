#include "SpectrumView.h"
#include <cmath>

namespace
{
    inline float powToDb (float p) noexcept { return p > 1.0e-20f ? 10.0f * std::log10 (p) : -200.0f; }

    juce::String freqText (float f)
    {
        if (f >= 1000.0f) return juce::String (f / 1000.0f, f >= 10000.0f ? 1 : 2) + " kHz";
        return juce::String (juce::roundToInt (f)) + " Hz";
    }
}

// ---------------------------------------------------------------------------
class SpectrumView::Chip : public juce::Button
{
public:
    Chip (TCSpectrumAnalyzerProcessor& p, int idx, const juce::String& label)
        : juce::Button (label), proc (p), index (idx)
    {
        onClick = [this]
        {
            const auto bit = (juce::uint32) 1 << index;
            proc.hiddenChannels.store (proc.hiddenChannels.load() ^ bit);
            repaint();
        };
    }

    bool isShown() const { return (proc.hiddenChannels.load() & ((juce::uint32) 1 << index)) == 0; }

    void paintButton (juce::Graphics& g, bool over, bool) override
    {
        auto r = getLocalBounds().toFloat().reduced (0.5f);
        const bool on = isShown();
        g.setColour (on ? juce::Colour (0xff25292e) : juce::Colours::transparentBlack);
        g.fillRoundedRectangle (r, r.getHeight() * 0.5f);
        g.setColour (over ? Theme::textDim : juce::Colour (0xff3d434b));
        g.drawRoundedRectangle (r, r.getHeight() * 0.5f, 1.0f);

        const float d = 8.0f;
        g.setColour (on ? colour : juce::Colour (0xff4a4f56));
        g.fillEllipse (r.getX() + 9.0f, r.getCentreY() - d * 0.5f, d, d);

        g.setColour (on ? Theme::textBright : Theme::textDim);
        g.setFont (Theme::font (12.0f, true));
        g.drawText (getButtonText(), r.withTrimmedLeft (22.0f).withTrimmedRight (8.0f), juce::Justification::centredLeft);
    }

    int idealWidth() const
    {
        return 34 + juce::roundToInt (juce::GlyphArrangement::getStringWidth (Theme::font (12.0f, true), getButtonText()));
    }

    juce::Colour colour;

private:
    TCSpectrumAnalyzerProcessor& proc;
    int index;
};

// ---------------------------------------------------------------------------
// Static background (panel, grid, labels), cached as an image and redrawn only when the view changes.
class SpectrumView::Grid : public juce::Component
{
public:
    explicit Grid (SpectrumView& o) : owner (o)
    {
        setInterceptsMouseClicks (false, false);
        setBufferedToImage (true);
        setOpaque (true);
    }
    void paint (juce::Graphics& g) override { owner.paintGrid (g); }
private:
    SpectrumView& owner;
};

// ---------------------------------------------------------------------------
SpectrumView::SpectrumView (TCSpectrumAnalyzerProcessor& p)
    : proc (p), analyzer (p.analyzer)
{
    showParam   = proc.apvts.getRawParameterValue (ParamIDs::show);
    holdParam   = proc.apvts.getRawParameterValue (ParamIDs::peakHold);
    fallParam   = proc.apvts.getRawParameterValue (ParamIDs::fall);
    weightParam = proc.apvts.getRawParameterValue (ParamIDs::weighting);
    detectParam = proc.apvts.getRawParameterValue (ParamIDs::detect);
    responseParam = proc.apvts.getRawParameterValue (ParamIDs::response);

    setOpaque (true);
    grid = std::make_unique<Grid> (*this);
    addAndMakeVisible (*grid);

    resetViewButton.onClick = [this] { resetView(); };
    addAndMakeVisible (resetViewButton);

    for (auto& c : holdDb) c.fill (-200.0f);
    for (auto& c : curDb)  c.fill (-200.0f);
    for (auto& c : disp)   c.fill (0.0f);
    sumDisp.fill (0.0f);
    sumDb.fill (-200.0f);
    sumHoldDb.fill (-200.0f);
    weights.fill (1.0f);
}

SpectrumView::~SpectrumView() = default;

void SpectrumView::resetHold()
{
    for (auto& c : holdDb) c.fill (-200.0f);
    sumHoldDb.fill (-200.0f);
}

void SpectrumView::resetView()
{
    dbTop = 0.0f; dbBottom = -80.0f;
    fLo = 16.0f; fHi = 22000.0f;
    viewChanged();
}

void SpectrumView::viewChanged()
{
    if (grid != nullptr)
        grid->repaint();
    repaint();
}

void SpectrumView::refreshColours()
{
    colours.clear();
    for (int i = 0; i < names.size(); ++i)
        colours.push_back (settings->getChannelColour (names[i], i));
    for (int i = 0; i < chips.size(); ++i)
    {
        chips[i]->colour = colours[(size_t) i];
        chips[i]->repaint();
    }
    seenColourVersion = settings->getVersion();
}

void SpectrumView::rebuildChips()
{
    chips.clear();
    for (int i = 0; i < names.size(); ++i)
        addAndMakeVisible (chips.add (new Chip (proc, i, names[i])));
    refreshColours();
    resized();
}

float SpectrumView::weightingDb (int type, double f)
{
    if (type == 0) return 0.0f;
    const double f2 = f * f;
    const double c1 = 20.6 * 20.6, c4 = 12194.0 * 12194.0;
    if (type == 1) // A
    {
        const double r = (c4 * f2 * f2) / ((f2 + c1) * std::sqrt ((f2 + 107.7 * 107.7) * (f2 + 737.9 * 737.9)) * (f2 + c4));
        return (float) (20.0 * std::log10 (r) + 2.0);
    }
    if (type == 2) // B
    {
        const double r = (c4 * f2 * f) / ((f2 + c1) * std::sqrt (f2 + 158.5 * 158.5) * (f2 + c4));
        return (float) (20.0 * std::log10 (r) + 0.17);
    }
    const double r = (c4 * f2) / ((f2 + c1) * (f2 + c4)); // C
    return (float) (20.0 * std::log10 (r) + 0.06);
}

void SpectrumView::rebuildWeights()
{
    seenWeighting = juce::roundToInt (weightParam->load());
    for (int b = 0; b < numBands; ++b)
        weights[(size_t) b] = std::pow (10.0f, weightingDb (seenWeighting, freqs[(size_t) b]) / 10.0f);
}

void SpectrumView::tick (double dt)
{
    if (proc.getChannelLayoutVersion() != seenChannelVersion)
    {
        seenChannelVersion = proc.getChannelLayoutVersion();
        names = proc.getChannelNames();
        rebuildChips();
        resetHold();
    }

    if (settings->getVersion() != seenColourVersion)
        refreshColours();

    if (analyzer.getLayoutVersion() != seenLayoutVersion)
    {
        seenLayoutVersion = analyzer.getLayoutVersion();
        numBands = analyzer.copyBandFrequencies (freqs);
        rebuildWeights();
        resetHold();
    }

    if (juce::roundToInt (weightParam->load()) != seenWeighting)
    {
        rebuildWeights();
        resetHold();
    }

    if (! frozen)
    {
        const int numCh = juce::jmin (names.size(), analyzer.getNumChannels());
        const float fallSec = TCSpectrumAnalyzerProcessor::fallSecondsFromNormalised (fallParam->load());
        const float fallDb = fallSec > 0.0f ? (float) (20.0 / fallSec * dt) : 0.0f; // 20 dB per "fall" time
        const auto hidden = proc.hiddenChannels.load();

        // ballistics, applied every frame so the motion is smooth whatever the FFT update rate
        const bool rms = juce::roundToInt (detectParam->load()) == 1;
        const double tau = juce::jmax (0.001, (double) responseParam->load() * 0.001);
        const float peakDecay = (float) std::exp (-2.0 * dt / tau);   // power domain
        const float rmsCoeff  = (float) (1.0 - std::exp (-dt / tau));
        const float scale = rms ? 1.0f : 2.0f;                         // Peak: a sine reads its peak level

        std::array<float, Analyzer::maxBands> sp {}, smx {};

        for (int ch = 0; ch < numCh; ++ch)
        {
            const bool shown = (hidden & ((juce::uint32) 1 << ch)) == 0;
            for (int b = 0; b < numBands; ++b)
            {
                const float w = weights[(size_t) b] * scale;
                const float p  = analyzer.getBandPower (ch, b) * w;
                const float mx = analyzer.takeBandMax (ch, b) * w;
                float& d = disp[(size_t) ch][(size_t) b];
                if (rms) d += rmsCoeff * (p - d);
                else     d = juce::jmax (mx, d * peakDecay);
                curDb[(size_t) ch][(size_t) b] = powToDb (d);
                float& h = holdDb[(size_t) ch][(size_t) b];
                h = juce::jmax (h - fallDb, powToDb (mx), -200.0f);
                if (shown) { sp[(size_t) b] += d; smx[(size_t) b] += mx; }
            }
        }

        for (int b = 0; b < numBands; ++b)
        {
            sumDb[(size_t) b] = powToDb (sp[(size_t) b]);
            float& h = sumHoldDb[(size_t) b];
            h = juce::jmax (h - fallDb, powToDb (smx[(size_t) b]), -200.0f);
        }
    }

    repaint (plot.expanded (2));
    if (mouseInPlot)
        repaint (chipRow);
}

// ---------------------------------------------------------------------------
float SpectrumView::freqToX (float f) const noexcept
{
    return (float) plot.getX() + (float) plot.getWidth() * std::log (f / fLo) / std::log (fHi / fLo);
}

float SpectrumView::xToFreq (float x) const noexcept
{
    return fLo * std::pow (fHi / fLo, (x - (float) plot.getX()) / (float) plot.getWidth());
}

float SpectrumView::dbToY (float db) const noexcept
{
    return (float) plot.getY() + (float) plot.getHeight() * (dbTop - db) / (dbTop - dbBottom);
}

float SpectrumView::yToDb (float y) const noexcept
{
    return dbTop - (y - (float) plot.getY()) / (float) plot.getHeight() * (dbTop - dbBottom);
}

void SpectrumView::resized()
{
    auto r = getLocalBounds().reduced (10, 8);
    chipRow = r.removeFromTop (26);
    r.removeFromTop (8);
    dbLabels = r.removeFromLeft (34);
    freqLabels = r.removeFromBottom (18);
    plot = r;

    grid->setBounds (getLocalBounds());

    auto row = chipRow;
    resetViewButton.setBounds (row.removeFromRight (84));
    for (auto* c : chips)
    {
        c->setBounds (row.removeFromLeft (c->idealWidth()));
        row.removeFromLeft (6);
    }
}

juce::Path SpectrumView::makeCurve (const float* db, int n, bool closed) const
{
    juce::Path p;
    const float bottom = (float) plot.getBottom() + 2.0f;
    float firstX = 0, lastX = 0;
    for (int b = 0; b < n; ++b)
    {
        const float x = freqToX (freqs[(size_t) b]);
        const float y = juce::jlimit ((float) plot.getY() - 2000.0f, bottom, dbToY (db[b]));
        if (b == 0) { p.startNewSubPath (x, y); firstX = x; }
        else          p.lineTo (x, y);
        lastX = x;
    }
    if (closed && n > 1)
    {
        p.lineTo (lastX, bottom);
        p.lineTo (firstX, bottom);
        p.closeSubPath();
    }
    return p;
}

void SpectrumView::paintGrid (juce::Graphics& g)
{
    g.fillAll (Theme::window);
    auto bounds = getLocalBounds().toFloat();
    g.setColour (Theme::panel);
    g.fillRoundedRectangle (bounds, 6.0f);
    g.setColour (Theme::panelBorder);
    g.drawRoundedRectangle (bounds.reduced (0.5f), 6.0f, 1.0f);

    g.setColour (Theme::plotBg);
    g.fillRect (plot);

    // ---- grid ----
    const float span = dbTop - dbBottom;
    float step = 20.0f;
    for (float s : { 1.0f, 2.0f, 3.0f, 6.0f, 10.0f, 20.0f })
        if (span / s <= 10.0f) { step = s; break; }

    g.setFont (Theme::mono (10.5f));
    for (float d = std::floor (dbTop / step) * step; d >= dbBottom - 0.01f; d -= step)
    {
        const float y = dbToY (d);
        g.setColour (std::fmod (std::abs (d), step * 2.0f) < 0.01f ? Theme::gridMajor : Theme::gridMinor);
        g.fillRect ((float) plot.getX(), std::round (y), (float) plot.getWidth(), 1.0f);
        g.setColour (Theme::textDim);
        g.drawText (juce::String ((int) d), dbLabels.getX(), (int) y - 7, dbLabels.getWidth() - 6, 14, juce::Justification::centredRight);
    }

    static const std::pair<float, const char*> fl[] = { { 20.f, "20" }, { 31.5f, "31" }, { 63.f, "63" }, { 125.f, "125" },
        { 250.f, "250" }, { 500.f, "500" }, { 1000.f, "1k" }, { 2000.f, "2k" }, { 4000.f, "4k" }, { 8000.f, "8k" }, { 16000.f, "16k" } };
    for (auto& [f, t] : fl)
    {
        if (f < fLo || f > fHi) continue;
        const float x = freqToX (f);
        g.setColour (Theme::gridMinor.brighter (0.15f));
        g.fillRect (std::round (x), (float) plot.getY(), 1.0f, (float) plot.getHeight());
        g.setColour (Theme::textDim);
        g.drawText (t, (int) x - 20, freqLabels.getY() + 2, 40, 14, juce::Justification::centred);
    }
}

void SpectrumView::paint (juce::Graphics&) {}

void SpectrumView::paintOverChildren (juce::Graphics& g)
{
    // ---- curves ----
    {
        juce::Graphics::ScopedSaveState ss (g);
        g.reduceClipRegion (plot);

        const int mode = juce::roundToInt (showParam->load()); // 0 channels, 1 sum, 2 both
        const bool hold = holdParam->load() > 0.5f;
        const int numCh = juce::jmin (names.size(), (int) colours.size());
        const auto hidden = proc.hiddenChannels.load();
        int visible = 0;
        for (int ch = 0; ch < numCh; ++ch)
            if ((hidden & ((juce::uint32) 1 << ch)) == 0) ++visible;

        const float dashes[] = { 3.0f, 3.0f };

        if (mode != 0 && numBands > 1)
        {
            g.setColour (Theme::sumColour.withAlpha (0.14f));
            g.fillPath (makeCurve (sumDb.data(), numBands, true));
            g.setColour (Theme::sumColour);
            g.strokePath (makeCurve (sumDb.data(), numBands, false), juce::PathStrokeType (1.6f, juce::PathStrokeType::curved));
            if (hold)
            {
                juce::Path dashed;
                juce::PathStrokeType (1.0f).createDashedStroke (dashed, makeCurve (sumHoldDb.data(), numBands, false), dashes, 2);
                g.setColour (Theme::sumColour.withAlpha (0.7f));
                g.fillPath (dashed);
            }
        }

        if (mode != 1 && numBands > 1)
        {
            for (int ch = 0; ch < numCh; ++ch)
            {
                if ((hidden & ((juce::uint32) 1 << ch)) != 0) continue;
                const auto col = colours[(size_t) ch];
                if (mode == 0 && visible <= 2)
                {
                    g.setColour (col.withAlpha (0.10f));
                    g.fillPath (makeCurve (curDb[(size_t) ch].data(), numBands, true));
                }
                g.setColour (col);
                g.strokePath (makeCurve (curDb[(size_t) ch].data(), numBands, false), juce::PathStrokeType (1.5f, juce::PathStrokeType::curved));
                if (hold)
                {
                    juce::Path dashed;
                    juce::PathStrokeType (1.0f).createDashedStroke (dashed, makeCurve (holdDb[(size_t) ch].data(), numBands, false), dashes, 2);
                    g.setColour (col.withAlpha (0.65f));
                    g.fillPath (dashed);
                }
            }
        }

        if (mouseInPlot)
        {
            g.setColour (juce::Colours::white.withAlpha (0.25f));
            g.fillRect (std::round (mousePos.x), (float) plot.getY(), 1.0f, (float) plot.getHeight());
        }

        if (frozen)
        {
            auto r = juce::Rectangle<float> ((float) plot.getRight() - 74.0f, (float) plot.getY() + 8.0f, 64.0f, 18.0f);
            g.setColour (juce::Colour (0xcc3c3014));
            g.fillRoundedRectangle (r, 4.0f);
            g.setColour (juce::Colour (0xffffd27a));
            g.setFont (Theme::font (11.0f, true));
            g.drawText ("FREEZE", r, juce::Justification::centred);
        }
    }

    if (mouseInPlot)
    {
        const auto txt = freqText (xToFreq (mousePos.x)) + "  " + juce::String (yToDb (mousePos.y), 1) + " dB";
        g.setColour (Theme::textDim);
        g.setFont (Theme::mono (11.5f));
        g.drawText (txt, chipRow.withTrimmedRight (94), juce::Justification::centredRight);
    }
}

// ---------------------------------------------------------------------------
void SpectrumView::mouseMove (const juce::MouseEvent& e)
{
    mousePos = e.position;
    mouseInPlot = plot.toFloat().contains (e.position);
}

void SpectrumView::mouseExit (const juce::MouseEvent&)
{
    mouseInPlot = false;
    repaint (chipRow);
}

void SpectrumView::mouseDown (const juce::MouseEvent& e)
{
    dragStart = e.position;
    dragDbTop = dbTop; dragDbBottom = dbBottom;
    dragFLo = fLo; dragFHi = fHi;
}

void SpectrumView::mouseDrag (const juce::MouseEvent& e)
{
    if (! plot.toFloat().contains (dragStart))
        return;

    const float dy = (e.position.y - dragStart.y) / (float) plot.getHeight() * (dragDbTop - dragDbBottom);
    float top = dragDbTop + dy, bottom = dragDbBottom + dy;
    if (top > 24.0f)     { bottom -= top - 24.0f; top = 24.0f; }
    if (bottom < -160.f) { top += -160.f - bottom; bottom = -160.f; }
    dbTop = top; dbBottom = bottom;

    const float ratio = std::log (dragFHi / dragFLo);
    const float shift = -(e.position.x - dragStart.x) / (float) plot.getWidth() * ratio;
    float lo = dragFLo * std::exp (shift), hi = dragFHi * std::exp (shift);
    if (lo < 10.0f)    { hi *= 10.0f / lo; lo = 10.0f; }
    if (hi > 24000.0f) { lo *= 24000.0f / hi; hi = 24000.0f; }
    fLo = lo; fHi = hi;

    mousePos = e.position;
    viewChanged();
}

void SpectrumView::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (plot.toFloat().contains (e.position))
        resetHold();
}

void SpectrumView::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    if (! plot.toFloat().contains (e.position))
        return;

    const bool freqZoom = e.mods.isCommandDown() || e.mods.isCtrlDown() || std::abs (w.deltaX) > std::abs (w.deltaY);
    const float delta = std::abs (w.deltaX) > std::abs (w.deltaY) ? w.deltaX : w.deltaY;
    const float factor = std::exp (-delta * 0.8f);

    if (freqZoom)
    {
        const float fm = xToFreq (e.position.x);
        float lo = fm * std::pow (fLo / fm, factor);
        float hi = fm * std::pow (fHi / fm, factor);
        if (hi / lo < 2.0f) return;
        fLo = juce::jmax (10.0f, lo);
        fHi = juce::jmin (24000.0f, hi);
    }
    else
    {
        const float dm = yToDb (e.position.y);
        float top = dm + (dbTop - dm) * factor;
        float bottom = dm + (dbBottom - dm) * factor;
        if (top - bottom < 12.0f || top - bottom > 140.0f) return;
        dbTop = juce::jmin (24.0f, top);
        dbBottom = juce::jmax (-160.0f, bottom);
    }
    viewChanged();
}

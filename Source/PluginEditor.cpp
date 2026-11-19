#include "PluginEditor.h"

namespace
{
    class ColourPicker : public juce::ColourSelector, private juce::ChangeListener
    {
    public:
        ColourPicker (const juce::String& n, juce::Colour c)
            : juce::ColourSelector (juce::ColourSelector::showColourspace | juce::ColourSelector::showSliders), name (n)
        {
            setCurrentColour (c, juce::dontSendNotification);
            setColour (juce::ColourSelector::backgroundColourId, Theme::topBar);
            setSize (300, 280);
            addChangeListener (this);
        }

        ~ColourPicker() override { removeChangeListener (this); }

    private:
        void changeListenerCallback (juce::ChangeBroadcaster*) override
        {
            settings->setChannelColour (name, getCurrentColour());
        }

        juce::String name;
        juce::SharedResourcePointer<GlobalSettings> settings;
    };

    juce::RangedAudioParameter& param (TCSpectrumAnalyzerProcessor& p, const char* id)
    {
        return *p.apvts.getParameter (id);
    }
}

MainPanel::MainPanel (TCSpectrumAnalyzerProcessor& p)
    : proc (p),
      spectrum (p), meter (p),
      detect (param (p, ParamIDs::detect)),
      lfRes (param (p, ParamIDs::lfRes)),
      weighting (param (p, ParamIDs::weighting)),
      show (param (p, ParamIDs::show)),
      vblank (this, [this] { tick(); })
{
    setLookAndFeel (&lnf);

    addAndMakeVisible (spectrum);
    addAndMakeVisible (meter);

    for (auto* b : { &freezeButton, &resetButton, &coloursButton, &meterButton })
        addAndMakeVisible (b);

    freezeButton.setClickingTogglesState (true);
    freezeButton.onClick = [this] { spectrum.setFrozen (freezeButton.getToggleState()); };
    resetButton.onClick = [this]
    {
        spectrum.resetHold();
        meter.resetPeaks();
        proc.analyzer.resetMeterMax();
    };
    coloursButton.onClick = [this] { showColourMenu(); };
    meterButton.setClickingTogglesState (true);
    meterAttachment = std::make_unique<juce::ButtonParameterAttachment> (param (p, ParamIDs::meter), meterButton);
    meterButton.onStateChange = [this]
    {
        if (meter.isVisible() != meterButton.getToggleState())
        {
            if (onBaseSizeChanged) onBaseSizeChanged();
            resized();
        }
    };

    // sliders
    for (auto* s : { &response, &fall })
    {
        s->setSliderStyle (juce::Slider::LinearHorizontal);
        s->setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
        addAndMakeVisible (s);
    }
    responseAttachment = std::make_unique<juce::SliderParameterAttachment> (param (p, ParamIDs::response), response);
    fallAttachment     = std::make_unique<juce::SliderParameterAttachment> (param (p, ParamIDs::fall), fall);
    response.setDoubleClickReturnValue (true, 160.0);

    for (auto* l : { &responseValue, &fallValue })
    {
        l->setFont (Theme::mono (12.0f));
        l->setColour (juce::Label::textColourId, Theme::textBright);
        l->setJustificationType (juce::Justification::centredLeft);
        l->setInterceptsMouseClicks (false, false);
        addAndMakeVisible (l);
    }
    auto updateValues = [this]
    {
        auto& rp = param (proc, ParamIDs::response);
        auto& fp = param (proc, ParamIDs::fall);
        responseValue.setText (rp.getCurrentValueAsText(), juce::dontSendNotification);
        fallValue.setText (fp.getCurrentValueAsText(), juce::dontSendNotification);
    };
    response.onValueChange = updateValues;
    fall.onValueChange = updateValues;
    updateValues();

    peakHoldButton.setClickingTogglesState (true);
    holdAttachment = std::make_unique<juce::ButtonParameterAttachment> (param (p, ParamIDs::peakHold), peakHoldButton);
    auto updateHold = [this]
    {
        peakHoldButton.setButtonText (peakHoldButton.getToggleState() ? "ON" : "OFF");
        fall.setEnabled (peakHoldButton.getToggleState());
        fall.setAlpha (peakHoldButton.getToggleState() ? 1.0f : 0.4f);
    };
    peakHoldButton.onStateChange = updateHold;
    updateHold();
    addAndMakeVisible (peakHoldButton);

    for (auto* c : { (juce::Component*) &detect, (juce::Component*) &lfRes, (juce::Component*) &weighting, (juce::Component*) &show })
        addAndMakeVisible (c);

    groups = {
        { "Detect",     &detect,         nullptr, 116, 0,   {}, nullptr },
        { "Response",   &response,       nullptr, 130, 0, {}, &responseValue },
        { "Peak Hold",  &peakHoldButton, &fall,   48, 110, {}, &fallValue },
        { "LF Res",     &lfRes,          nullptr, 168, 0,   {}, nullptr },
        { "Weighting",  &weighting,      nullptr, 150, 0,   {}, nullptr },
        { "Show",       &show,           nullptr, 250, 0,   {}, nullptr },
    };

    proc.analyzer.spectrumActive.store (true);
    setSize (getBaseWidth(), baseHeight);
    lastTick = juce::Time::getMillisecondCounterHiRes();
}

MainPanel::~MainPanel()
{
    proc.analyzer.spectrumActive.store (false);
    setLookAndFeel (nullptr);
}

void MainPanel::tick()
{
    const double now = juce::Time::getMillisecondCounterHiRes();
    const double dt = juce::jlimit (0.0, 0.25, (now - lastTick) * 0.001);
    lastTick = now;

    if (proc.getChannelLayoutVersion() != seenChannelVersion)
    {
        seenChannelVersion = proc.getChannelLayoutVersion();
        juce::String layoutName = "Custom";
        int n = 0;
        if (auto* bus = proc.getBus (true, 0))
        {
            const auto set = bus->getCurrentLayout();
            n = set.size();
            layoutName = set.getDescription();
        }
        const auto dot = juce::String::fromUTF8 ("  \xc2\xb7  ");
        busText = "Bus: " + layoutName + dot + "auto-detected" + dot + juce::String (n) + " ch";
        repaint (topBar);
    }

    spectrum.tick (dt);
    if (meter.isVisible())
        meter.tick (dt);
}

void MainPanel::showColourMenu()
{
    juce::PopupMenu m;
    const auto names = proc.getChannelNames();
    juce::SharedResourcePointer<GlobalSettings> settings;
    for (int i = 0; i < names.size(); ++i)
        m.addColouredItem (i + 1, names[i], settings->getChannelColour (names[i], i));
    m.addSeparator();
    m.addItem (1000, "Reset to defaults");

    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&coloursButton).withParentComponent (this),
        [this, names] (int result)
        {
            juce::SharedResourcePointer<GlobalSettings> s;
            if (result == 1000) { s->resetColours(); return; }
            const int idx = result - 1;
            if (idx < 0 || idx >= names.size()) return;
            auto picker = std::make_unique<ColourPicker> (names[idx], s->getChannelColour (names[idx], idx));
            juce::CallOutBox::launchAsynchronously (std::move (picker), coloursButton.getScreenBounds(), nullptr);
        });
}

int MainPanel::bottomRowsNeeded (int width) const
{
    int rows = 1, x = 0;
    for (auto& g : groups)
    {
        const int w = g.widthA + (g.b ? g.widthB + 8 : 0);
        if (x > 0 && x + w > width) { ++rows; x = 0; }
        x += w + (getWidth() < 900 ? 12 : 22);
    }
    return rows;
}

void MainPanel::layoutBottom (juce::Rectangle<int> area)
{
    const int rowH = 52;
    int x = area.getX(), y = area.getY();
    for (auto& g : groups)
    {
        const int w = g.widthA + (g.b ? g.widthB + 8 : 0);
        if (x > area.getX() && x + w > area.getRight()) { x = area.getX(); y += rowH; }

        g.captionArea = { x, y + 4, w, 16 };
        const int cy = y + 22;
        g.a->setBounds (x, cy, g.widthA, 28);
        if (g.b) g.b->setBounds (x + g.widthA + 8, cy, g.widthB, 28);

        if (g.valueLabel)
        {
            const int captionW = juce::roundToInt (juce::GlyphArrangement::getStringWidth (Theme::font (11.5f), g.caption)) + 8;
            g.valueLabel->setBounds (x + captionW, y + 2, 90, 20);
        }
        x += w + (getWidth() < 900 ? 12 : 22);
    }
}

void MainPanel::resized()
{

    auto r = getLocalBounds();
    topBar = r.removeFromTop (44);

    const bool compact = getWidth() < 900;
    auto t = topBar.reduced (compact ? 8 : 14, 8);
    const int gap = compact ? 4 : 8;
    meterButton.setBounds (t.removeFromRight (compact ? 52 : 64));
    t.removeFromRight (gap);
    coloursButton.setBounds (t.removeFromRight (compact ? 64 : 80));
    t.removeFromRight (gap);
    resetButton.setBounds (t.removeFromRight (compact ? 86 : 100));
    t.removeFromRight (gap);
    freezeButton.setBounds (t.removeFromRight (compact ? 60 : 72));
    badgeArea = t;

    auto content = r.reduced (compact ? 8 : 14, 0);
    const int rows = bottomRowsNeeded (content.getWidth());
    auto bottom = content.removeFromBottom (52 * rows + 6);
    content.removeFromTop (compact ? 8 : 12);
    content.removeFromBottom (2);

    const bool showMeter = meterButton.getToggleState();
    meter.setVisible (showMeter);
    if (showMeter)
    {
        meter.setBounds (content.removeFromRight (220));
        content.removeFromRight (compact ? 8 : 12);
    }
    spectrum.setBounds (content);

    layoutBottom (bottom.withTrimmedTop (4));
}

void MainPanel::paint (juce::Graphics& g)
{
    g.fillAll (Theme::window);

    g.setColour (Theme::topBar);
    g.fillRect (topBar);
    g.setColour (Theme::panelBorder);
    g.fillRect (topBar.getX(), topBar.getBottom() - 1, topBar.getWidth(), 1);

    auto t = badgeArea.withTrimmedLeft (getWidth() < 900 ? 6 : 14);
    g.setColour (Theme::textBright);
    const float titleSize = getWidth() < 700 ? 12.5f : 15.0f;
    g.setFont (Theme::font (titleSize, true));
    const juce::String title (getWidth() < 640 ? "TC SPECTRUM" : "TC SPECTRUM ANALYZER");
    const int titleW = juce::jmin (t.getWidth(), juce::roundToInt (juce::GlyphArrangement::getStringWidth (Theme::font (titleSize, true), title)) + 4);
    g.drawText (title, t.removeFromLeft (titleW), juce::Justification::centredLeft);

    t.removeFromLeft (14);
    g.setFont (Theme::font (12.0f));
    const int bw = juce::jmin (t.getWidth() - 10,
                               juce::roundToInt (juce::GlyphArrangement::getStringWidth (Theme::font (12.0f), busText)) + 22);
    if (bw > 40)
    {
        auto badge = t.removeFromLeft (bw).withSizeKeepingCentre (bw, 22).toFloat();
        g.setColour (Theme::badgeBg);
        g.fillRoundedRectangle (badge, 11.0f);
        g.setColour (Theme::badgeText);
        g.drawFittedText (busText, badge.toNearestInt().reduced (10, 0), juce::Justification::centred, 1, 0.8f);
    }

    g.setFont (Theme::font (11.5f));
    g.setColour (Theme::textDim);
    for (auto& grp : groups)
        g.drawText (grp.caption, grp.captionArea, juce::Justification::centredLeft);
}

// ---------------------------------------------------------------------------
TCSpectrumAnalyzerEditor::TCSpectrumAnalyzerEditor (TCSpectrumAnalyzerProcessor& p)
    : AudioProcessorEditor (&p), proc (p), panel (p)
{
    addAndMakeVisible (panel);
    panel.onBaseSizeChanged = [this] { applyBaseSize (currentScale); };

    setResizable (true, true);
    const float savedScale = (float) proc.editorWidth.load() / 1000.0f; // stored as width at 1000 px base
    applyBaseSize (juce::jlimit (0.56f, 2.6f, savedScale > 0.1f ? savedScale : 1.0f));
}

TCSpectrumAnalyzerEditor::~TCSpectrumAnalyzerEditor() = default;

void TCSpectrumAnalyzerEditor::applyBaseSize (float scale)
{
    const int bw = panel.getBaseWidth(), bh = MainPanel::baseHeight;
    if (auto* c = getConstrainer())
    {
        c->setFixedAspectRatio ((double) bw / (double) bh);
        c->setSizeLimits (juce::roundToInt (bw * 0.56f), juce::roundToInt (bh * 0.56f),
                          juce::roundToInt (bw * 2.6f), juce::roundToInt (bh * 2.6f));
    }
    setSize (juce::roundToInt (bw * scale), juce::roundToInt (bh * scale));
}

void TCSpectrumAnalyzerEditor::resized()
{
    const int bw = panel.getBaseWidth();
    currentScale = (float) getWidth() / (float) bw;
    proc.editorWidth.store (juce::roundToInt (currentScale * 1000.0f));
    proc.editorHeight.store (getHeight());
    panel.setTransform (juce::AffineTransform::scale (currentScale));
    panel.setBounds (0, 0, bw, MainPanel::baseHeight);
}

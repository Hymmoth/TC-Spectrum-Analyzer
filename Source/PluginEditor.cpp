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

TCSpectrumAnalyzerEditor::TCSpectrumAnalyzerEditor (TCSpectrumAnalyzerProcessor& p)
    : AudioProcessorEditor (&p), proc (p),
      spectrum (p), meter (p),
      detect (param (p, ParamIDs::detect)),
      lfRes (param (p, ParamIDs::lfRes)),
      weighting (param (p, ParamIDs::weighting)),
      show (param (p, ParamIDs::show)),
      vblank (this, [this] { tick(); })
{
    setLookAndFeel (&lnf);

    glContext.setComponentPaintingEnabled (true);
    glContext.setContinuousRepainting (false);
    glContext.attachTo (*this);

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
    meterButton.onStateChange = [this] { if (meter.isVisible() != meterButton.getToggleState()) resized(); };

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
        { "Response",   &response,       nullptr, 150, 0, {}, &responseValue },
        { "Peak Hold",  &peakHoldButton, &fall,   52, 130, {}, &fallValue },
        { "LF Res",     &lfRes,          nullptr, 186, 0,   {}, nullptr },
        { "Weighting",  &weighting,      nullptr, 168, 0,   {}, nullptr },
        { "Show",       &show,           nullptr, 290, 0,   {}, nullptr },
    };

    proc.analyzer.spectrumActive.store (true);

    setResizable (true, true);
    setResizeLimits (820, 480, 2800, 1600);
    setSize (juce::jlimit (820, 2800, proc.editorWidth.load()), juce::jlimit (480, 1600, proc.editorHeight.load()));
    lastTick = juce::Time::getMillisecondCounterHiRes();
}

TCSpectrumAnalyzerEditor::~TCSpectrumAnalyzerEditor()
{
    proc.analyzer.spectrumActive.store (false);
    glContext.detach();
    setLookAndFeel (nullptr);
}

void TCSpectrumAnalyzerEditor::tick()
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

void TCSpectrumAnalyzerEditor::showColourMenu()
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

int TCSpectrumAnalyzerEditor::bottomRowsNeeded (int width) const
{
    int rows = 1, x = 0;
    for (auto& g : groups)
    {
        const int w = g.widthA + (g.b ? g.widthB + 8 : 0);
        if (x > 0 && x + w > width) { ++rows; x = 0; }
        x += w + 22;
    }
    return rows;
}

void TCSpectrumAnalyzerEditor::layoutBottom (juce::Rectangle<int> area)
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
        x += w + 22;
    }
}

void TCSpectrumAnalyzerEditor::resized()
{
    proc.editorWidth.store (getWidth());
    proc.editorHeight.store (getHeight());

    auto r = getLocalBounds();
    topBar = r.removeFromTop (44);

    auto t = topBar.reduced (14, 8);
    meterButton.setBounds (t.removeFromRight (64));
    t.removeFromRight (8);
    coloursButton.setBounds (t.removeFromRight (80));
    t.removeFromRight (8);
    resetButton.setBounds (t.removeFromRight (100));
    t.removeFromRight (8);
    freezeButton.setBounds (t.removeFromRight (72));
    badgeArea = t;

    auto content = r.reduced (14, 0);
    const int rows = bottomRowsNeeded (content.getWidth());
    auto bottom = content.removeFromBottom (52 * rows + 10);
    content.removeFromTop (12);
    content.removeFromBottom (4);

    const bool showMeter = meterButton.getToggleState();
    meter.setVisible (showMeter);
    if (showMeter)
    {
        meter.setBounds (content.removeFromRight (240));
        content.removeFromRight (12);
    }
    spectrum.setBounds (content);

    layoutBottom (bottom.withTrimmedTop (4));
}

void TCSpectrumAnalyzerEditor::paint (juce::Graphics& g)
{
    g.fillAll (Theme::window);

    g.setColour (Theme::topBar);
    g.fillRect (topBar);
    g.setColour (Theme::panelBorder);
    g.fillRect (topBar.getX(), topBar.getBottom() - 1, topBar.getWidth(), 1);

    auto t = badgeArea.withTrimmedLeft (14);
    g.setColour (Theme::textBright);
    g.setFont (Theme::font (15.0f, true));
    const juce::String title ("TC SPECTRUM ANALYZER");
    const int titleW = juce::roundToInt (juce::GlyphArrangement::getStringWidth (Theme::font (15.0f, true), title)) + 4;
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

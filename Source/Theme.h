#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_data_structures/juce_data_structures.h>

namespace Theme
{
    inline const juce::Colour window      { 0xff1d1f22 };
    inline const juce::Colour topBar      { 0xff24272b };
    inline const juce::Colour panel       { 0xff17191b };
    inline const juce::Colour panelBorder { 0xff2c3035 };
    inline const juce::Colour plotBg      { 0xff121315 };
    inline const juce::Colour gridMajor   { 0xff2b3036 };
    inline const juce::Colour gridMinor   { 0xff202327 };
    inline const juce::Colour text        { 0xffc9cdd2 };
    inline const juce::Colour textBright  { 0xffe8eaed };
    inline const juce::Colour textDim     { 0xff9aa1aa };
    inline const juce::Colour buttonBg    { 0xff2b2f34 };
    inline const juce::Colour buttonLine  { 0xff3a3f46 };
    inline const juce::Colour selBg       { 0xff3569b5 };
    inline const juce::Colour selLine     { 0xff4f8fe6 };
    inline const juce::Colour badgeBg     { 0xff2b3a4f };
    inline const juce::Colour badgeText   { 0xffa9c9f5 };
    inline const juce::Colour meterGreen  { 0xff3fae6a };
    inline const juce::Colour meterYellow { 0xffe0c341 };
    inline const juce::Colour meterRed    { 0xffe2513f };
    inline const juce::Colour sumColour   { 0xffe6e9ee };

    inline juce::Font font (float size, bool bold = false)
    {
        return juce::Font (juce::FontOptions (size, bold ? juce::Font::bold : juce::Font::plain));
    }

    inline juce::Font mono (float size)
    {
        return juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), size, juce::Font::plain));
    }
}

// ---------------------------------------------------------------------------
// Global settings shared by every instance (channel colours).
// Stored in ~/Library/Application Support/TC Spectrum Analyzer/
class GlobalSettings
{
public:
    GlobalSettings()
    {
        juce::PropertiesFile::Options o;
        o.applicationName = "TC Spectrum Analyzer";
        o.filenameSuffix = ".settings";
        o.folderName = "TC Spectrum Analyzer";
        o.osxLibrarySubFolder = "Application Support";
        props.setStorageParameters (o);
    }

    static juce::Colour defaultColour (const juce::String& name, int index)
    {
        static const std::pair<const char*, juce::uint32> named[] = {
            { "L", 0xff4ea1ff }, { "R", 0xffff8a3d }, { "C", 0xffe6cf4a }, { "LFE", 0xffa78bfa },
            { "Ls", 0xff34d1b2 }, { "Rs", 0xffff5c8a }, { "Lrs", 0xff7fd858 }, { "Rrs", 0xffe879f9 },
            { "Lss", 0xff7fd858 }, { "Rss", 0xffe879f9 }, { "Ltf", 0xff60e0f0 }, { "Rtf", 0xfff5a3a3 },
            { "Ltr", 0xffb8c4ff }, { "Rtr", 0xffffd08a }, { "Ltm", 0xff9ee6c8 }, { "Rtm", 0xffffb3d9 } };
        for (auto& n : named)
            if (name.equalsIgnoreCase (n.first))
                return juce::Colour (n.second);

        static const juce::uint32 palette[] = { 0xff4ea1ff, 0xffff8a3d, 0xffe6cf4a, 0xffa78bfa, 0xff34d1b2, 0xffff5c8a,
                                                0xff7fd858, 0xffe879f9, 0xff60e0f0, 0xfff5a3a3, 0xffb8c4ff, 0xffffd08a,
                                                0xff9ee6c8, 0xffffb3d9, 0xffd4d4d4, 0xffc0a080 };
        return juce::Colour (palette[(size_t) index % 16]);
    }

    juce::Colour getChannelColour (const juce::String& name, int index)
    {
        if (auto* f = props.getUserSettings())
        {
            const auto s = f->getValue ("colour_" + name);
            if (s.isNotEmpty())
                return juce::Colour::fromString (s);
        }
        return defaultColour (name, index);
    }

    void setChannelColour (const juce::String& name, juce::Colour c)
    {
        if (auto* f = props.getUserSettings())
        {
            f->setValue ("colour_" + name, c.toString());
            f->saveIfNeeded();
        }
        ++version;
    }

    void resetColours()
    {
        if (auto* f = props.getUserSettings())
        {
            for (auto& k : f->getAllProperties().getAllKeys())
                if (k.startsWith ("colour_"))
                    f->removeValue (k);
            f->saveIfNeeded();
        }
        ++version;
    }

    int getVersion() const noexcept { return version.load(); }

private:
    juce::ApplicationProperties props;
    std::atomic<int> version { 0 };
};

// ---------------------------------------------------------------------------
class TCLookAndFeel : public juce::LookAndFeel_V4
{
public:
    TCLookAndFeel()
    {
        setColour (juce::TextButton::textColourOffId, Theme::text);
        setColour (juce::TextButton::textColourOnId, juce::Colours::white);
        setColour (juce::PopupMenu::backgroundColourId, Theme::topBar);
        setColour (juce::PopupMenu::textColourId, Theme::text);
        setColour (juce::PopupMenu::highlightedBackgroundColourId, Theme::selBg);
        setColour (juce::Slider::trackColourId, Theme::selLine);
        setColour (juce::Slider::backgroundColourId, Theme::buttonBg);
        setColour (juce::Slider::thumbColourId, Theme::textBright);
    }

    void drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour&, bool over, bool down) override
    {
        auto r = b.getLocalBounds().toFloat().reduced (0.5f);
        const bool on = b.getToggleState();
        auto fill = on ? Theme::selBg : Theme::buttonBg;
        if (over) fill = fill.brighter (0.08f);
        if (down) fill = fill.brighter (0.15f);
        g.setColour (fill);
        g.fillRoundedRectangle (r, 4.0f);
        g.setColour (on ? Theme::selLine : Theme::buttonLine);
        g.drawRoundedRectangle (r, 4.0f, 1.0f);
    }

    juce::Font getTextButtonFont (juce::TextButton&, int h) override
    {
        return Theme::font (juce::jmin (13.0f, h * 0.48f));
    }

    void drawLinearSlider (juce::Graphics& g, int x, int y, int w, int h, float pos, float, float,
                           juce::Slider::SliderStyle, juce::Slider&) override
    {
        const float cy = (float) y + (float) h * 0.5f;
        juce::Rectangle<float> track ((float) x, cy - 2.0f, (float) w, 4.0f);
        g.setColour (Theme::buttonBg);
        g.fillRoundedRectangle (track, 2.0f);
        g.setColour (Theme::selLine);
        g.fillRoundedRectangle (track.withRight (pos), 2.0f);
        g.setColour (Theme::textBright);
        g.fillEllipse (pos - 6.0f, cy - 6.0f, 12.0f, 12.0f);
    }
};

// ---------------------------------------------------------------------------
// Row of buttons bound to a choice parameter.
class SegmentedControl : public juce::Component
{
public:
    SegmentedControl (juce::RangedAudioParameter& p, juce::StringArray labels = {})
        : param (p),
          attachment (p, [this] (float v) { setSelected (juce::roundToInt (v)); })
    {
        if (labels.isEmpty())
            if (auto* c = dynamic_cast<juce::AudioParameterChoice*> (&p))
                labels = c->choices;

        for (int i = 0; i < labels.size(); ++i)
        {
            auto* b = buttons.add (new juce::TextButton (labels[i]));
            b->setClickingTogglesState (false);
            b->onClick = [this, i] { attachment.setValueAsCompleteGesture ((float) i); };
            addAndMakeVisible (b);
        }
        attachment.sendInitialUpdate();
    }

    void resized() override
    {
        auto r = getLocalBounds();
        const int n = buttons.size();
        if (n == 0) return;
        const int gap = 2;
        const int w = (r.getWidth() - gap * (n - 1)) / n;
        for (int i = 0; i < n; ++i)
        {
            buttons[i]->setBounds (r.removeFromLeft (i == n - 1 ? r.getWidth() : w));
            r.removeFromLeft (gap);
        }
    }

    int getIdealWidth (int perButton) const { return buttons.size() * perButton + (buttons.size() - 1) * 2; }

private:
    void setSelected (int idx)
    {
        for (int i = 0; i < buttons.size(); ++i)
            buttons[i]->setToggleState (i == idx, juce::dontSendNotification);
    }

    juce::RangedAudioParameter& param;
    juce::OwnedArray<juce::TextButton> buttons;
    juce::ParameterAttachment attachment;
};

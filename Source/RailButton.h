#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "Theme.h"

// A button of the icon rail on the left of the window: a round-cornered square with a line icon drawn as a path.
class RailButton : public juce::Button
{
public:
    enum class Icon { Menu, Add, Plugins, Tuner, Audio, Mixer, Folder };

    RailButton (Icon i, const juce::String& tip, bool accentFill = false) : juce::Button (tip), icon (i), accent (accentFill)
    {
        setTooltip (tip);
    }

    void paintButton (juce::Graphics& g, bool highlighted, bool down) override
    {
        auto r = getLocalBounds().toFloat().reduced (3.0f);
        juce::Colour fill = accent ? Theme::orange : on ? Theme::orange.withAlpha (0.4f) : Theme::panel;   // (on: the panel it opens is showing)
        if (down)             fill = fill.darker (0.25f);
        else if (highlighted) fill = fill.brighter (0.15f);
        g.setColour (fill);
        g.fillRoundedRectangle (r, 10.0f);

        g.setColour (accent ? juce::Colours::white : (highlighted ? Theme::text : Theme::iconIdle));
        const auto c = r.getCentre();
        const float s = 8.0f;   // half size of the icon
        juce::Path p;
        switch (icon)
        {
            case Icon::Menu:
                for (int i = -1; i <= 1; ++i)
                {
                    p.startNewSubPath (c.x - s, c.y + (float) i * 6.0f);
                    p.lineTo (c.x + s, c.y + (float) i * 6.0f);
                }
                break;
            case Icon::Add:
                p.startNewSubPath (c.x - s, c.y);  p.lineTo (c.x + s, c.y);
                p.startNewSubPath (c.x, c.y - s);  p.lineTo (c.x, c.y + s);
                break;
            case Icon::Plugins:   // a plug: two prongs, a body and a cable
                p.startNewSubPath (c.x - 4.0f, c.y - s);  p.lineTo (c.x - 4.0f, c.y - 2.0f);
                p.startNewSubPath (c.x + 4.0f, c.y - s);  p.lineTo (c.x + 4.0f, c.y - 2.0f);
                p.addRoundedRectangle (c.x - 7.0f, c.y - 2.0f, 14.0f, 8.0f, 3.0f);
                p.startNewSubPath (c.x, c.y + 6.0f);  p.lineTo (c.x, c.y + s + 1.0f);
                break;
            case Icon::Tuner:     // a gauge with a needle
                p.addArc (c.x - s - 1.0f, c.y - s + 1.0f, 2.0f * s + 2.0f, 2.0f * s + 2.0f, -1.9f, 1.9f, true);
                p.startNewSubPath (c.x, c.y + 2.0f);  p.lineTo (c.x + 4.0f, c.y - 5.0f);
                break;
            case Icon::Mixer:     // level bars of different height
                for (int i = -1; i <= 1; ++i)
                {
                    const float x = c.x + (float) i * 7.0f, h = i == 0 ? 15.0f : i < 0 ? 9.0f : 12.0f;
                    p.startNewSubPath (x, c.y + 8.0f);
                    p.lineTo (x, c.y + 8.0f - h);
                }
                break;
            case Icon::Folder:    // a folder
                p.startNewSubPath (c.x - s, c.y - 6.0f);  p.lineTo (c.x - 2.0f, c.y - 6.0f);  p.lineTo (c.x, c.y - 3.0f);  p.lineTo (c.x + s, c.y - 3.0f);
                p.lineTo (c.x + s, c.y + 7.0f);  p.lineTo (c.x - s, c.y + 7.0f);  p.closeSubPath();
                break;
            case Icon::Audio:     // sliders
                for (int i = -1; i <= 1; ++i)
                {
                    const float x = c.x + (float) i * 7.0f;
                    p.startNewSubPath (x, c.y - s);  p.lineTo (x, c.y + s);
                    p.addEllipse (x - 2.2f, c.y + (float) (-i * 3), 4.4f, 4.4f);
                }
                break;
        }
        g.strokePath (p, juce::PathStrokeType (1.9f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

public:
    void setOn (bool shouldBeOn) { if (on != shouldBeOn) { on = shouldBeOn; repaint(); } }

private:
    Icon icon;
    bool accent;
    bool on = false;
};

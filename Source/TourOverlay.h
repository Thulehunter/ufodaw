#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "Theme.h"

// A quick tour of the window: the screen dims, one part is lit up, and a card explains it (Back / Next / Skip). Used on the first start
// and from File > Quick tour. Esc skips it; Enter and the arrow keys step through.
class TourOverlay : public juce::Component
{
public:
    struct Step
    {
        juce::String title, text;
        std::function<juce::Rectangle<int>()> target;   // in the overlay's coordinates; empty rectangle = no highlight (a card in the middle)
    };

    TourOverlay (std::vector<Step> stepList, std::function<void (bool finished)> onClose)
        : steps (std::move (stepList)), closed (std::move (onClose))
    {
        setInterceptsMouseClicks (true, true);
        setWantsKeyboardFocus (true);

        titleLabel.setFont (juce::FontOptions (18.0f, juce::Font::bold));
        titleLabel.setColour (juce::Label::textColourId, Theme::yellow);
        addAndMakeVisible (titleLabel);

        textLabel.setFont (juce::FontOptions (14.0f));
        textLabel.setColour (juce::Label::textColourId, Theme::text);
        textLabel.setMinimumHorizontalScale (1.0f);
        addAndMakeVisible (textLabel);

        counter.setFont (juce::FontOptions (12.0f));
        counter.setColour (juce::Label::textColourId, Theme::textFaint);
        addAndMakeVisible (counter);

        backButton.onClick = [this] { go (-1); };
        nextButton.onClick = [this] { go (1); };
        skipButton.onClick = [this] { close (false); };
        nextButton.setColour (juce::TextButton::buttonColourId, Theme::orange);
        nextButton.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
        for (auto* b : { &backButton, &nextButton, &skipButton })
        {
            b->setWantsKeyboardFocus (false);
            addAndMakeVisible (*b);
        }
        show (0);
    }

    void paint (juce::Graphics& g) override
    {
        const auto hole = currentHole();
        juce::Path dim;
        dim.addRectangle (getLocalBounds().toFloat());
        if (! hole.isEmpty())
            dim.addRoundedRectangle (hole.toFloat(), 10.0f);
        dim.setUsingNonZeroWinding (false);   // (even-odd: the hole is left clear)
        g.setColour (juce::Colour (0xff0a0605).withAlpha (0.78f));
        g.fillPath (dim);

        if (! hole.isEmpty())
        {
            g.setColour (Theme::yellow);
            g.drawRoundedRectangle (hole.toFloat(), 10.0f, 2.5f);
        }

        // the card
        const auto card = cardBounds.toFloat();
        g.setColour (juce::Colours::black.withAlpha (0.35f));
        g.fillRoundedRectangle (card.translated (0.0f, 4.0f), 14.0f);
        g.setColour (Theme::panel);
        g.fillRoundedRectangle (card, 14.0f);
        g.setColour (Theme::gridLineStrong);
        g.drawRoundedRectangle (card, 14.0f, 1.0f);
    }

    void resized() override { layoutCard(); }

    void mouseDown (const juce::MouseEvent&) override {}   // (the tour takes the clicks: the program stays as it is)

    bool keyPressed (const juce::KeyPress& k) override
    {
        if (k == juce::KeyPress::escapeKey)                                    { close (false); return true; }
        if (k == juce::KeyPress::returnKey || k == juce::KeyPress::rightKey)   { go (1); return true; }
        if (k == juce::KeyPress::leftKey)                                      { go (-1); return true; }
        return true;   // (nothing else gets through while the tour is open)
    }

    void parentHierarchyChanged() override
    {
        if (getParentComponent() != nullptr)
            juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<TourOverlay> (this)] { if (safe != nullptr) safe->grabKeyboardFocus(); });
    }

private:
    juce::Rectangle<int> currentHole() const
    {
        if (! juce::isPositiveAndBelow (index, (int) steps.size()) || ! steps[(size_t) index].target)
            return {};
        return steps[(size_t) index].target().expanded (6, 6).getIntersection (getLocalBounds());
    }

    void show (int i)
    {
        index = juce::jlimit (0, (int) steps.size() - 1, i);
        titleLabel.setText (steps[(size_t) index].title, juce::dontSendNotification);
        textLabel.setText (steps[(size_t) index].text, juce::dontSendNotification);
        counter.setText (juce::String (index + 1) + " / " + juce::String ((int) steps.size()), juce::dontSendNotification);
        backButton.setEnabled (index > 0);
        nextButton.setButtonText (index == (int) steps.size() - 1 ? "Done" : "Next");
        layoutCard();
        repaint();
    }

    void go (int delta)
    {
        if (index + delta >= (int) steps.size())
        {
            close (true);
            return;
        }
        show (index + delta);
    }

    void close (bool finished)
    {
        auto callback = closed;
        juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<TourOverlay> (this), callback, finished]
        {
            if (callback)
                callback (finished);   // the owner removes this overlay
        });
        setVisible (false);
    }

    // The card sits next to the lit part (below it, above it, or beside it), inside the window; in the middle when nothing is lit.
    void layoutCard()
    {
        if (getWidth() <= 0 || steps.empty())
            return;
        constexpr int w = 380, h = 190;
        const auto hole = currentHole();
        const auto area = getLocalBounds().reduced (16);
        juce::Rectangle<int> card (w, h);
        if (hole.isEmpty())
        {
            card.setCentre (area.getCentre());
        }
        else if (hole.getBottom() + 14 + h <= getHeight() - 16)
        {
            card.setPosition (juce::jlimit (16, juce::jmax (16, getWidth() - w - 16), hole.getX()), hole.getBottom() + 14);
        }
        else if (hole.getY() - 14 - h >= 16)
        {
            card.setPosition (juce::jlimit (16, juce::jmax (16, getWidth() - w - 16), hole.getX()), hole.getY() - 14 - h);
        }
        else if (hole.getRight() + 14 + w <= getWidth() - 16)
        {
            card.setPosition (hole.getRight() + 14, juce::jlimit (16, juce::jmax (16, getHeight() - h - 16), hole.getY()));
        }
        else
        {
            card.setPosition (juce::jmax (16, hole.getX() - 14 - w), juce::jlimit (16, juce::jmax (16, getHeight() - h - 16), hole.getY()));
        }
        cardBounds = card;

        auto r = card.reduced (18, 14);
        titleLabel.setBounds (r.removeFromTop (26));
        auto bottom = r.removeFromBottom (30);
        r.removeFromBottom (6);
        textLabel.setBounds (r);
        counter.setBounds (bottom.removeFromLeft (60));
        nextButton.setBounds (bottom.removeFromRight (82));
        bottom.removeFromRight (6);
        backButton.setBounds (bottom.removeFromRight (70));
        bottom.removeFromRight (6);
        skipButton.setBounds (bottom.removeFromRight (110));
    }

    std::vector<Step> steps;
    std::function<void (bool)> closed;
    int index = 0;
    juce::Rectangle<int> cardBounds;
    juce::Label titleLabel, textLabel, counter;
    juce::TextButton backButton { "Back" }, nextButton { "Next" }, skipButton { "Skip the tour" };
};

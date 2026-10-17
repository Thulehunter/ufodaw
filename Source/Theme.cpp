#include "Theme.h"

WarmLookAndFeel::WarmLookAndFeel()
{
    using namespace Theme;

    setColour (juce::ResizableWindow::backgroundColourId, background);
    setColour (juce::DocumentWindow::textColourId, text);

    setColour (juce::TextButton::buttonColourId, buttonOff);
    setColour (juce::TextButton::buttonOnColourId, orange);
    setColour (juce::TextButton::textColourOffId, text);
    setColour (juce::TextButton::textColourOnId, background);

    setColour (juce::ToggleButton::textColourId, text);
    setColour (juce::ToggleButton::tickColourId, orange);
    setColour (juce::ToggleButton::tickDisabledColourId, textFaint);

    setColour (juce::Label::textColourId, text);
    setColour (juce::Label::outlineColourId, juce::Colours::transparentBlack);

    setColour (juce::Slider::thumbColourId, yellow);
    setColour (juce::Slider::trackColourId, orange);
    setColour (juce::Slider::backgroundColourId, border);
    setColour (juce::Slider::textBoxTextColourId, text);
    setColour (juce::Slider::textBoxBackgroundColourId, laneBg);
    setColour (juce::Slider::textBoxOutlineColourId, border);

    setColour (juce::ComboBox::backgroundColourId, panelRaised);
    setColour (juce::ComboBox::textColourId, text);
    setColour (juce::ComboBox::outlineColourId, border);
    setColour (juce::ComboBox::arrowColourId, orange);

    setColour (juce::PopupMenu::backgroundColourId, panel);
    setColour (juce::PopupMenu::textColourId, text);
    setColour (juce::PopupMenu::headerTextColourId, yellow);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, orange);
    setColour (juce::PopupMenu::highlightedTextColourId, background);

    setColour (juce::ScrollBar::thumbColourId, buttonDown);
    setColour (juce::TextEditor::backgroundColourId, laneBg);
    setColour (juce::TextEditor::textColourId, text);
    setColour (juce::TextEditor::outlineColourId, border);
    setColour (juce::TextEditor::focusedOutlineColourId, orange);
    setColour (juce::AlertWindow::backgroundColourId, panel);
    setColour (juce::AlertWindow::textColourId, text);
    setColour (juce::AlertWindow::outlineColourId, border);
    setColour (juce::TooltipWindow::backgroundColourId, panelRaised);
    setColour (juce::TooltipWindow::textColourId, text);
    setColour (juce::TooltipWindow::outlineColourId, orange);
    setColour (juce::ListBox::backgroundColourId, laneBg);
    setColour (juce::ListBox::textColourId, text);
}

void WarmLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& button, const juce::Colour& backgroundColour,
                                            bool highlighted, bool down)
{
    auto bounds = button.getLocalBounds().toFloat().reduced (0.5f);
    auto base = backgroundColour.withMultipliedAlpha (button.isEnabled() ? 1.0f : 0.45f);
    if (down)             base = base.darker (0.25f);
    else if (highlighted) base = base.brighter (0.15f);

    g.setGradientFill (juce::ColourGradient (base.brighter (0.12f), 0.0f, bounds.getY(),
                                             base.darker (0.12f), 0.0f, bounds.getBottom(), false));
    g.fillRoundedRectangle (bounds, 5.0f);

    g.setColour (Theme::border.withAlpha (0.9f));
    g.drawRoundedRectangle (bounds, 5.0f, 1.0f);
}

void WarmLookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& button, bool highlighted, bool)
{
    const float size = juce::jmin (18.0f, (float) button.getHeight() - 6.0f);
    const juce::Rectangle<float> box (4.0f, ((float) button.getHeight() - size) * 0.5f, size, size);
    const bool on = button.getToggleState();

    g.setColour (on ? Theme::orange : (highlighted ? Theme::buttonHover : Theme::buttonOff));
    g.fillRoundedRectangle (box, 4.0f);
    g.setColour (Theme::border);
    g.drawRoundedRectangle (box, 4.0f, 1.0f);

    if (on)
    {
        juce::Path tick;
        tick.startNewSubPath (box.getX() + size * 0.24f, box.getCentreY());
        tick.lineTo (box.getX() + size * 0.43f, box.getBottom() - size * 0.27f);
        tick.lineTo (box.getRight() - size * 0.2f, box.getY() + size * 0.27f);
        g.setColour (Theme::background);
        g.strokePath (tick, juce::PathStrokeType (2.2f));
    }

    g.setColour (button.findColour (juce::ToggleButton::textColourId).withMultipliedAlpha (button.isEnabled() ? 1.0f : 0.5f));
    g.setFont (juce::FontOptions (14.0f));
    g.drawText (button.getButtonText(), button.getLocalBounds().withTrimmedLeft ((int) size + 12),
                juce::Justification::centredLeft, true);
}

void WarmLookAndFeel::drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height, float sliderPos,
                                        float minSliderPos, float maxSliderPos, juce::Slider::SliderStyle style, juce::Slider& slider)
{
    if (style != juce::Slider::LinearHorizontal || ! slider.isEnabled())
    {
        if (style == juce::Slider::LinearHorizontal)   // disabled (e.g. automated) horizontal slider: dimmed track only
        {
            const float cy = (float) y + (float) height * 0.5f;
            g.setColour (Theme::border);
            g.fillRoundedRectangle ((float) x, cy - 2.0f, (float) width, 4.0f, 2.0f);
            g.setColour (Theme::yellowDim.withAlpha (0.55f));
            g.fillRoundedRectangle ((float) x, cy - 2.0f, juce::jmax (0.0f, sliderPos - (float) x), 4.0f, 2.0f);
            g.fillEllipse (sliderPos - 5.0f, cy - 5.0f, 10.0f, 10.0f);
            return;
        }
        LookAndFeel_V4::drawLinearSlider (g, x, y, width, height, sliderPos, minSliderPos, maxSliderPos, style, slider);
        return;
    }

    const float cy = (float) y + (float) height * 0.5f;
    g.setColour (Theme::border);
    g.fillRoundedRectangle ((float) x, cy - 2.5f, (float) width, 5.0f, 2.5f);

    g.setGradientFill (juce::ColourGradient (Theme::red, (float) x, cy, Theme::yellow, (float) (x + width), cy, false));
    g.fillRoundedRectangle ((float) x, cy - 2.5f, juce::jmax (0.0f, sliderPos - (float) x), 5.0f, 2.5f);

    const float r = slider.isMouseOverOrDragging() ? 7.5f : 6.5f;
    g.setColour (Theme::background);
    g.fillEllipse (sliderPos - r - 1.5f, cy - r - 1.5f, 2.0f * r + 3.0f, 2.0f * r + 3.0f);
    g.setColour (Theme::yellow);
    g.fillEllipse (sliderPos - r, cy - r, 2.0f * r, 2.0f * r);
}

void WarmLookAndFeel::drawComboBox (juce::Graphics& g, int width, int height, bool, int, int, int, int, juce::ComboBox& box)
{
    const auto bounds = juce::Rectangle<float> (0.0f, 0.0f, (float) width, (float) height).reduced (0.5f);
    g.setColour (box.findColour (juce::ComboBox::backgroundColourId));
    g.fillRoundedRectangle (bounds, 5.0f);
    g.setColour (box.findColour (juce::ComboBox::outlineColourId));
    g.drawRoundedRectangle (bounds, 5.0f, 1.0f);

    juce::Path arrow;
    const float cx = (float) width - 14.0f, cy = (float) height * 0.5f;
    arrow.addTriangle (cx - 4.5f, cy - 2.5f, cx + 4.5f, cy - 2.5f, cx, cy + 3.0f);
    g.setColour (box.findColour (juce::ComboBox::arrowColourId).withMultipliedAlpha (box.isEnabled() ? 1.0f : 0.4f));
    g.fillPath (arrow);
}

void WarmLookAndFeel::drawPopupMenuBackground (juce::Graphics& g, int width, int height)
{
    g.fillAll (Theme::panel);
    g.setColour (Theme::orange.withAlpha (0.7f));
    g.drawRect (0, 0, width, height, 1);
}

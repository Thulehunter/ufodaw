#include "FxPanel.h"

namespace
{
    constexpr int rowHeight = 28;
    constexpr int titleHeight = 30;
    constexpr int gap = 14;
    constexpr int maxRows = 5;   // the compressor has the most parameters
}

FxPanel::FxPanel (Track& t, std::function<void()> onBeginEdit) : track (t), beginEdit (std::move (onBeginEdit))
{
    auto& fx = track.fx;

    auto& eq = addSection ("EQ", fx.eqOn);
    addRow (eq, "Low",      -18.0, 18.0, 0.0,    " dB", 1, fx.eqLow);
    addRow (eq, "Mid",      -18.0, 18.0, 0.0,    " dB", 1, fx.eqMid);
    addRow (eq, "Mid freq", 200.0, 5000.0, 1000.0, " Hz", 0, fx.eqMidFreq);
    addRow (eq, "High",     -18.0, 18.0, 0.0,    " dB", 1, fx.eqHigh);

    auto& comp = addSection ("Compressor", fx.compOn);
    addRow (comp, "Threshold", -60.0, 0.0, 0.0,   " dB", 1, fx.compThreshold);
    addRow (comp, "Ratio",     1.0, 20.0, 4.0,    " :1", 1, fx.compRatio);
    addRow (comp, "Attack",    1.0, 200.0, 20.0,  " ms", 0, fx.compAttack);
    addRow (comp, "Release",   10.0, 1000.0, 150.0, " ms", 0, fx.compRelease);
    addRow (comp, "Makeup",    0.0, 24.0, 0.0,    " dB", 1, fx.compMakeup);

    auto& delay = addSection ("Delay", fx.delayOn);
    addRow (delay, "Time",     20.0, 1500.0, 400.0, " ms", 0, fx.delayTime);
    addRow (delay, "Feedback", 0.0, 0.9, 0.0,     "", 2, fx.delayFeedback);
    addRow (delay, "Mix",      0.0, 1.0, 0.0,     "", 2, fx.delayMix);

    auto& reverb = addSection ("Reverb", fx.reverbOn);
    addRow (reverb, "Size",    0.0, 1.0, 0.0, "", 2, fx.reverbSize);
    addRow (reverb, "Damping", 0.0, 1.0, 0.0, "", 2, fx.reverbDamping);
    addRow (reverb, "Mix",     0.0, 1.0, 0.0, "", 2, fx.reverbMix);

    setSize (740, 2 * (titleHeight + maxRows * rowHeight) + 3 * gap - 30);
}

FxPanel::Section& FxPanel::addSection (const juce::String& title, std::atomic<bool>& enabled)
{
    auto s = std::make_unique<Section>();
    s->title = title;
    s->toggle.setButtonText (title);
    s->toggle.setToggleState (enabled, juce::dontSendNotification);
    s->toggle.onClick = [this, &enabled, &toggle = s->toggle]
    {
        if (beginEdit) beginEdit();
        enabled = toggle.getToggleState();
    };
    addAndMakeVisible (s->toggle);

    sections.push_back (std::move (s));
    return *sections.back();
}

void FxPanel::addRow (Section& s, const juce::String& name, double lo, double hi, double mid,
                      const juce::String& suffix, int decimals, std::atomic<float>& param)
{
    auto row = std::make_unique<Row>();
    row->label.setText (name, juce::dontSendNotification);
    row->label.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.8f));
    addAndMakeVisible (row->label);

    auto& slider = row->slider;
    slider.setSliderStyle (juce::Slider::LinearHorizontal);
    slider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 70, 20);
    slider.setRange (lo, hi);
    if (mid > lo && mid < hi)
        slider.setSkewFactorFromMidPoint (mid);
    slider.setTextValueSuffix (suffix);
    slider.setNumDecimalPlacesToDisplay (decimals);
    slider.setValue (param.load(), juce::dontSendNotification);
    slider.setDoubleClickReturnValue (true, param.load());
    slider.onDragStart = [this] { if (beginEdit) beginEdit(); };
    slider.onValueChange = [&param, &slider] { param = (float) slider.getValue(); };
    addAndMakeVisible (slider);

    s.rows.push_back (std::move (row));
}

void FxPanel::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff24272e));

    g.setColour (juce::Colour (0xff15171b));
    for (int i = 0; i < (int) sections.size(); ++i)
    {
        const auto b = getLocalBounds().reduced (gap / 2);
        const int colW = b.getWidth() / 2, rowGroupH = titleHeight + maxRows * rowHeight + gap;
        g.drawRoundedRectangle (juce::Rectangle<int> (b.getX() + (i % 2) * colW + 2, b.getY() + (i / 2) * rowGroupH,
                                                      colW - 4, titleHeight + (int) sections[(size_t) i]->rows.size() * rowHeight + 6).toFloat(),
                                6.0f, 1.0f);
    }
}

void FxPanel::resized()
{
    const auto b = getLocalBounds().reduced (gap / 2);
    const int colW = b.getWidth() / 2, rowGroupH = titleHeight + maxRows * rowHeight + gap;

    for (int i = 0; i < (int) sections.size(); ++i)
    {
        auto& s = *sections[(size_t) i];
        auto area = juce::Rectangle<int> (b.getX() + (i % 2) * colW + 2, b.getY() + (i / 2) * rowGroupH, colW - 4, rowGroupH)
                        .reduced (10, 0);

        s.toggle.setBounds (area.removeFromTop (titleHeight).reduced (0, 3));
        for (auto& row : s.rows)
        {
            auto r = area.removeFromTop (rowHeight);
            row->label.setBounds (r.removeFromLeft (70));
            row->slider.setBounds (r);
        }
    }
}

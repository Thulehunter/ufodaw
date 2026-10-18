#include "FxPanel.h"
#include "Theme.h"

namespace
{
    constexpr int rowHeight = 28;
    constexpr int titleHeight = 30;
    constexpr int gap = 14;
    constexpr int maxRows = 6;   // the compressor has the most parameters (and the box of its sidechain)
}

FxPanel::FxPanel (Track& t, std::function<void()> onBeginEdit, Engine* e) : track (t), engine (e), beginEdit (std::move (onBeginEdit))
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
    compSection = &comp;

    if (engine != nullptr && ! track.bus)
    {
        sideLabel.setText ("Listens to", juce::dontSendNotification);
        sideLabel.setColour (juce::Label::textColourId, Theme::text.withAlpha (0.8f));
        addAndMakeVisible (sideLabel);

        sideBox.addItem ("Its own signal", 1);
        int selected = 1;
        for (int i = 0; i < engine->getNumTracks(); ++i)
            if (auto* other = engine->getTrack (i); other != nullptr && other != &track && ! other->bus)
            {
                sideTracks.push_back (other);
                sideBox.addItem ("Sidechain: " + other->name, (int) sideTracks.size() + 1);
                if (track.sidechainSource.load() == other)
                    selected = (int) sideTracks.size() + 1;
            }
        sideBox.setSelectedId (selected, juce::dontSendNotification);
        sideBox.setTooltip ("Sidechain: the compressor of this track listens to another track and lowers this one when that one is loud "
                            "(a bass that ducks on the kick). Picking a track turns the compressor on with settings for that.");
        sideBox.onChange = [this]
        {
            if (beginEdit) beginEdit();
            const int id = sideBox.getSelectedId();
            auto* source = id >= 2 && (size_t) id - 2 < sideTracks.size() ? sideTracks[(size_t) id - 2] : nullptr;
            engine->setSidechain (track, source);
            if (source != nullptr && ! track.fx.compOn.load() && compSection != nullptr)   // a compressor that ducks: fast attack, a release that breathes
            {
                track.fx.compOn = true;
                track.fx.compThreshold = -30.0f;
                track.fx.compRatio = 6.0f;
                track.fx.compAttack = 3.0f;
                track.fx.compRelease = 160.0f;
                track.fx.compMakeup = 0.0f;
                compSection->toggle.setToggleState (true, juce::dontSendNotification);
                const double values[] = { -30.0, 6.0, 3.0, 160.0, 0.0 };
                for (size_t k = 0; k < 5 && k < compSection->rows.size(); ++k)
                    compSection->rows[k]->slider.setValue (values[k], juce::dontSendNotification);
            }
        };
        addAndMakeVisible (sideBox);
    }

    auto& delay = addSection ("Delay", fx.delayOn);
    addRow (delay, "Time",     20.0, 1500.0, 400.0, " ms", 0, fx.delayTime);
    addRow (delay, "Feedback", 0.0, 0.9, 0.0,     "", 2, fx.delayFeedback);
    addRow (delay, "Mix",      0.0, 1.0, 0.0,     "", 2, fx.delayMix);

    auto& reverb = addSection ("Reverb", fx.reverbOn);
    addRow (reverb, "Size",    0.0, 1.0, 0.0, "", 2, fx.reverbSize);
    addRow (reverb, "Damping", 0.0, 1.0, 0.0, "", 2, fx.reverbDamping);
    addRow (reverb, "Mix",     0.0, 1.0, 0.0, "", 2, fx.reverbMix);

    // below the effects: the channel's width
    monoToggle.setButtonText ("Mono: play this channel in mono (left and right summed)");
    monoToggle.setTooltip ("Sums left and right after the effects and plugins. Handy to check how a mix or a source sounds in mono, "
                           "or to centre a stereo recording. On the master it checks the whole mix.");
    monoToggle.setToggleState (fx.mono, juce::dontSendNotification);
    monoToggle.onClick = [this]
    {
        if (beginEdit) beginEdit();
        track.fx.mono = monoToggle.getToggleState();
    };
    addAndMakeVisible (monoToggle);

    setSize (740, 2 * (titleHeight + maxRows * rowHeight) + 3 * gap - 30 + 34);
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
    row->label.setColour (juce::Label::textColourId, Theme::text.withAlpha (0.8f));
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
    g.fillAll (Theme::panel);

    g.setColour (Theme::laneBg);
    for (int i = 0; i < (int) sections.size(); ++i)
    {
        const auto b = getLocalBounds().reduced (gap / 2);
        const int colW = b.getWidth() / 2, rowGroupH = titleHeight + maxRows * rowHeight + gap;
        g.drawRoundedRectangle (juce::Rectangle<int> (b.getX() + (i % 2) * colW + 2, b.getY() + (i / 2) * rowGroupH,
                                                      colW - 4, titleHeight + ((int) sections[(size_t) i]->rows.size() + (sections[(size_t) i].get() == compSection && sideBox.isVisible() ? 1 : 0)) * rowHeight + 6).toFloat(),
                                6.0f, 1.0f);
    }
}

void FxPanel::resized()
{
    const auto b = getLocalBounds().reduced (gap / 2);
    const int colW = b.getWidth() / 2, rowGroupH = titleHeight + maxRows * rowHeight + gap;
    monoToggle.setBounds (b.getX() + 12, b.getBottom() - 30, b.getWidth() - 24, 26);

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
        if (&s == compSection && sideBox.getNumItems() > 0)
        {
            auto r = area.removeFromTop (rowHeight);
            sideLabel.setBounds (r.removeFromLeft (70));
            sideBox.setBounds (r.reduced (0, 2));
        }
    }
}

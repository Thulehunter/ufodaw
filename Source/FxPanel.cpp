#include "FxPanel.h"
#include <complex>
#include "Theme.h"

namespace
{
    constexpr int rowHeight = 28;
    constexpr int titleHeight = 30;
    constexpr int gap = 14;
    constexpr int topRows = 20;      // EQ and compressor: the tall sections
    constexpr int bottomRows = 9;    // delay and reverb
    constexpr int numBands = 6;
    constexpr double previewRate = 48000.0;

    // The six bands of the EQ: which parameters they edit
    struct BandInfo
    {
        const char* name;
        std::atomic<float>* gain;     // null: no gain (the filters)
        std::atomic<float>* freq;
        std::atomic<float>* q;        // null: fixed
        float freqLo, freqHi, freqOff; // freqOff: the value at which the band does nothing (filters only; else -1)
        juce::Colour colour;
    };

    BandInfo bandInfo (FxParams& p, int b)
    {
        switch (b)
        {
            case 0:  return { "High-pass",  nullptr,   &p.eqHpFreq,   nullptr,    20.0f,   2000.0f,  20.0f,    juce::Colour (0xffe0604a) };
            case 1:  return { "Low shelf",  &p.eqLow,  &p.eqLowFreq,  nullptr,    20.0f,   1000.0f,  -1.0f,    juce::Colour (0xfff08a3c) };
            case 2:  return { "Mid 1",      &p.eqMid,  &p.eqMidFreq,  &p.eqMidQ,  60.0f,   12000.0f, -1.0f,    juce::Colour (0xfff5b83a) };
            case 3:  return { "Mid 2",      &p.eqMid2, &p.eqMid2Freq, &p.eqMid2Q, 60.0f,   12000.0f, -1.0f,    juce::Colour (0xffd9d65a) };
            case 4:  return { "High shelf", &p.eqHigh, &p.eqHighFreq, nullptr,    1000.0f, 20000.0f, -1.0f,    juce::Colour (0xfff0a070) };
            default: return { "Low-pass",   nullptr,   &p.eqLpFreq,   nullptr,    1000.0f, 20000.0f, 20000.0f, juce::Colour (0xffd05a6a) };
        }
    }

    // Three columns: the EQ, the compressor, and the delay above the reverb
    juce::Rectangle<int> sectionRect (const juce::Rectangle<int>& b, int i)
    {
        const int colW = b.getWidth() / 3;
        const int column = i < 3 ? i : 2;
        const int y = i == 3 ? b.getY() + titleHeight + bottomRows * rowHeight + gap : b.getY();
        return { b.getX() + column * colW + 2, y, colW - 4, titleHeight + (i < 2 ? topRows : bottomRows) * rowHeight + 6 };
    }

    juce::String hzText (float f) { return f >= 1000.0f ? juce::String (f / 1000.0f, f >= 10000.0f ? 1 : 2) + " kHz" : juce::String ((int) std::round (f)) + " Hz"; }
}

// The frequency response of the EQ, 20 Hz - 20 kHz and +-18 dB, with a node for each band: drag it sideways for the frequency and up and down for the gain,
// scroll over a peak for its Q, double-click to reset the band. The curve is computed from the same coefficients the audio uses.
class EqCurveView : public juce::Component, private juce::Timer
{
public:
    EqCurveView (FxParams& p, std::function<void()> beginEdit, std::function<void (int)> select, std::function<void()> changed)
        : fx (p), onBegin (std::move (beginEdit)), onSelect (std::move (select)), onChanged (std::move (changed))
    {
        startTimerHz (15);
    }

    void setSelected (int b) { selected = b; repaint(); }

    void paint (juce::Graphics& g) override
    {
        const auto area = getLocalBounds().toFloat();
        g.setColour (Theme::laneBg);
        g.fillRoundedRectangle (area, 5.0f);
        const auto plot = plotArea();

        g.setFont (juce::FontOptions (9.5f));
        for (int db : { -12, -6, 0, 6, 12 })
        {
            const float y = yOf ((float) db, plot);
            g.setColour (Theme::text.withAlpha (db == 0 ? 0.25f : 0.08f));
            g.drawHorizontalLine ((int) y, plot.getX(), plot.getRight());
            g.setColour (Theme::textFaint);
            g.drawText (juce::String (db), (int) area.getX() + 1, (int) y - 5, 22, 10, juce::Justification::right);
        }
        for (double f : { 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0 })
        {
            const float x = xOf (f, plot);
            g.setColour (Theme::text.withAlpha (f == 100.0 || f == 1000.0 || f == 10000.0 ? 0.1f : 0.05f));
            g.drawVerticalLine ((int) x, plot.getY(), plot.getBottom());
            if (f == 100.0 || f == 1000.0 || f == 10000.0 || f == 50.0 || f == 500.0 || f == 5000.0)
            {
                g.setColour (Theme::textFaint);
                g.drawText (f >= 1000.0 ? juce::String ((int) (f / 1000.0)) + "k" : juce::String ((int) f), (int) x - 14, (int) plot.getBottom() + 1, 28, 10, juce::Justification::centred);
            }
        }

        EqCoef coefs[eqNumSections];
        eqDesign (fx, previewRate, coefs);
        juce::Path curve;
        const int points = (int) plot.getWidth();
        for (int i = 0; i <= points; ++i)
        {
            const double f = frequencyOf ((double) i / points);
            const float y = yOf (juce::jlimit (-18.0f, 18.0f, (float) eqResponseDb (coefs, previewRate, f)), plot);
            if (i == 0) curve.startNewSubPath (plot.getX(), y); else curve.lineTo (plot.getX() + (float) i, y);
        }
        const auto colour = fx.eqOn.load() ? Theme::yellow : Theme::textFaint;
        juce::Path filled (curve);
        filled.lineTo (plot.getRight(), yOf (0.0f, plot));
        filled.lineTo (plot.getX(), yOf (0.0f, plot));
        filled.closeSubPath();
        g.setColour (colour.withAlpha (0.18f));
        g.fillPath (filled);
        g.setColour (colour);
        g.strokePath (curve, juce::PathStrokeType (1.8f));

        // the nodes
        for (int b = 0; b < numBands; ++b)
        {
            const auto pos = nodeOf (b, plot);
            const auto info = bandInfo (fx, b);
            const bool active = b == selected;
            const bool engaged = info.gain != nullptr ? std::abs (info.gain->load()) > 0.01f : std::abs (info.freq->load() - info.freqOff) > 1.0f;
            if (active)
            {
                g.setColour (info.colour.withAlpha (0.35f));
                g.fillEllipse (pos.x - 10.0f, pos.y - 10.0f, 20.0f, 20.0f);
            }
            g.setColour (engaged || active ? info.colour : info.colour.withAlpha (0.45f));
            g.fillEllipse (pos.x - 6.5f, pos.y - 6.5f, 13.0f, 13.0f);
            g.setColour (Theme::background);
            g.setFont (juce::FontOptions (9.0f, juce::Font::bold));
            g.drawText (juce::String (b + 1), (int) pos.x - 8, (int) pos.y - 6, 16, 12, juce::Justification::centred);
        }

        // what the selected band says
        const auto info = bandInfo (fx, selected);
        juce::String text = juce::String (selected + 1) + "  " + info.name + "   " + hzText (info.freq->load());
        if (info.gain != nullptr)
            text += "   " + juce::String (info.gain->load(), 1) + " dB";
        if (info.q != nullptr)
            text += "   Q " + juce::String (info.q->load(), 2);
        g.setColour (info.colour);
        g.setFont (juce::FontOptions (11.5f, juce::Font::bold));
        g.drawText (text, area.reduced (30.0f, 5.0f).toNearestInt(), juce::Justification::topLeft);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        const int b = hit (e.position);
        if (b < 0)
            return;
        dragging = b;
        select (b);
        began = false;
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragging < 0)
            return;
        if (! began)
        {
            if (onBegin) onBegin();
            began = true;
        }
        const auto plot = plotArea();
        const auto info = bandInfo (fx, dragging);
        const double f = frequencyOf (juce::jlimit (0.0, 1.0, (double) ((e.position.x - plot.getX()) / plot.getWidth())));
        info.freq->store ((float) juce::jlimit ((double) info.freqLo, (double) info.freqHi, f));
        if (info.gain != nullptr)
        {
            const float db = (plotCentre (plot) - e.position.y) / (plot.getHeight() * 0.5f) * 18.0f;
            info.gain->store (std::round (juce::jlimit (-18.0f, 18.0f, db) * 10.0f) / 10.0f);
        }
        if (onChanged) onChanged();
        repaint();
    }

    void mouseUp (const juce::MouseEvent&) override { dragging = -1; }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        const int b = hit (e.position);
        if (b < 0)
            return;
        if (onBegin) onBegin();
        const auto info = bandInfo (fx, b);
        if (info.gain != nullptr) info.gain->store (0.0f);
        else info.freq->store (info.freqOff);
        if (info.q != nullptr) info.q->store (1.0f);
        select (b);
        if (onChanged) onChanged();
        repaint();
    }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        int b = hit (e.position, 24.0f);
        if (b < 0)
            b = selected;
        const auto info = bandInfo (fx, b);
        if (info.q == nullptr)
            return;
        if (onBegin) onBegin();
        select (b);
        info.q->store (juce::jlimit (0.2f, 10.0f, info.q->load() * std::exp (w.deltaY * 1.5f)));
        if (onChanged) onChanged();
        repaint();
    }

private:
    void select (int b)
    {
        selected = b;
        if (onSelect) onSelect (b);
    }

    juce::Rectangle<float> plotArea() const { return getLocalBounds().toFloat().reduced (24.0f, 6.0f).withTrimmedBottom (10.0f); }
    static float plotCentre (const juce::Rectangle<float>& plot) { return plot.getCentreY(); }

    juce::Point<float> nodeOf (int b, const juce::Rectangle<float>& plot) const
    {
        const auto info = bandInfo (fx, b);
        return { xOf (info.freq->load(), plot), yOf (info.gain != nullptr ? info.gain->load() : 0.0f, plot) };
    }

    int hit (juce::Point<float> p, float radius = 13.0f) const
    {
        const auto plot = plotArea();
        int best = -1;
        float bestDistance = radius;
        for (int b = 0; b < numBands; ++b)
        {
            const float d = nodeOf (b, plot).getDistanceFrom (p);
            if (d <= bestDistance && (d < bestDistance || b == selected))
            {
                best = b;
                bestDistance = d;
            }
        }
        return best;
    }

    static double frequencyOf (double position) { return 20.0 * std::pow (1000.0, position); }
    static float xOf (double hz, const juce::Rectangle<float>& plot)
    {
        return plot.getX() + (float) (std::log (juce::jmax (20.0, hz) / 20.0) / std::log (1000.0)) * plot.getWidth();
    }
    static float yOf (float db, const juce::Rectangle<float>& plot) { return plot.getCentreY() - (db / 18.0f) * plot.getHeight() * 0.5f; }
    void timerCallback() override { repaint(); }

    FxParams& fx;
    std::function<void()> onBegin;
    std::function<void (int)> onSelect;
    std::function<void()> onChanged;
    int selected = 2, dragging = -1;
    bool began = false;
};

// The compressor's curve (input level against output level, with the knee and the makeup) and a meter of the gain reduction at this moment.
class CompView : public juce::Component, private juce::Timer
{
public:
    explicit CompView (const FxParams& p) : fx (p) { startTimerHz (30); }

    void paint (juce::Graphics& g) override
    {
        const auto area = getLocalBounds().toFloat();
        g.setColour (Theme::laneBg);
        g.fillRoundedRectangle (area, 5.0f);

        const float side = juce::jmin (area.getHeight() - 12.0f, area.getWidth() * 0.45f);
        const auto plot = juce::Rectangle<float> (area.getX() + 10.0f, area.getY() + 6.0f, side, side);
        g.setColour (Theme::text.withAlpha (0.06f));
        g.fillRect (plot);
        for (int db = -48; db <= -12; db += 12)
        {
            g.setColour (Theme::text.withAlpha (0.08f));
            g.drawVerticalLine ((int) xOf ((float) db, plot), plot.getY(), plot.getBottom());
            g.drawHorizontalLine ((int) yOf ((float) db, plot), plot.getX(), plot.getRight());
        }
        g.setColour (Theme::text.withAlpha (0.2f));
        g.drawLine (plot.getX(), plot.getBottom(), plot.getRight(), plot.getY(), 1.0f);   // no compression

        const float threshold = fx.compThreshold, ratio = juce::jmax (1.0f, fx.compRatio.load()), knee = fx.compKnee, slope = 1.0f - 1.0f / ratio;
        const float makeup = fx.compMakeup.load() + (fx.compAuto.load() ? -threshold * slope * 0.5f : 0.0f);
        juce::Path curve;
        for (int i = 0; i <= (int) plot.getWidth(); ++i)
        {
            const float in = -60.0f + 60.0f * (float) i / plot.getWidth();
            const float over = in - threshold;
            float gr = 0.0f;
            if (slope > 0.0f)
            {
                if (knee > 0.01f && 2.0f * std::abs (over) <= knee) { const float x = over + knee * 0.5f; gr = slope * x * x / (2.0f * knee); }
                else if (over > 0.0f) gr = over * slope;
            }
            const float y = yOf (juce::jlimit (-60.0f, 0.0f, in - gr + makeup), plot);
            if (i == 0) curve.startNewSubPath (plot.getX(), y); else curve.lineTo (plot.getX() + (float) i, y);
        }
        const auto colour = fx.compOn.load() ? Theme::yellow : Theme::textFaint;
        g.setColour (colour);
        g.strokePath (curve, juce::PathStrokeType (1.8f));
        g.setColour (Theme::orange.withAlpha (0.7f));
        g.drawVerticalLine ((int) xOf (threshold, plot), plot.getY(), plot.getBottom());
        g.setColour (Theme::textFaint);
        g.setFont (juce::FontOptions (9.5f));
        g.drawText ("in", (int) plot.getRight() - 14, (int) plot.getBottom() - 11, 14, 10, juce::Justification::right);
        g.drawText ("out", (int) plot.getX() + 2, (int) plot.getY(), 24, 10, juce::Justification::left);

        // the gain reduction meter: 0 at the top, 24 dB at the bottom
        const auto meter = juce::Rectangle<float> (plot.getRight() + 14.0f, plot.getY(), 16.0f, plot.getHeight());
        g.setColour (Theme::text.withAlpha (0.06f));
        g.fillRect (meter);
        const float h = meter.getHeight() * juce::jlimit (0.0f, 1.0f, shown / 24.0f);
        g.setColour (Theme::orange);
        g.fillRect (meter.withHeight (h));
        g.setColour (Theme::textFaint);
        g.drawRect (meter, 1.0f);

        g.setColour (Theme::text);
        g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        g.drawText ((shown > 0.0f ? "-" : "") + juce::String (shown, 1) + " dB", juce::Rectangle<float> (meter.getRight() + 10.0f, plot.getY(), area.getRight() - meter.getRight() - 14.0f, 18.0f).toNearestInt(), juce::Justification::centredLeft);
        g.setColour (Theme::textFaint);
        g.setFont (juce::FontOptions (11.0f));
        g.drawFittedText ("gain reduction\nnow. Grey curve = off, the orange line is the threshold.",
                          juce::Rectangle<float> (meter.getRight() + 10.0f, plot.getY() + 20.0f, area.getRight() - meter.getRight() - 14.0f, plot.getHeight() - 20.0f).toNearestInt(),
                          juce::Justification::topLeft, 4);
    }

private:
    static float xOf (float db, const juce::Rectangle<float>& plot) { return plot.getX() + (db + 60.0f) / 60.0f * plot.getWidth(); }
    static float yOf (float db, const juce::Rectangle<float>& plot) { return plot.getBottom() - (db + 60.0f) / 60.0f * plot.getHeight(); }

    void timerCallback() override
    {
        const float now = fx.compOn.load() ? fx.compGr.load() : 0.0f;
        shown = juce::jmax (now, shown * 0.88f);   // (falls back smoothly)
        if (shown < 0.05f) shown = 0.0f;
        repaint();
    }

    const FxParams& fx;
    float shown = 0.0f;
};

namespace
{
    // Presets: values in the order of the pointer lists in applyEqPreset / applyCompPreset
    struct EqPreset { const char* name; float v[12]; };   // hp, lp, low, lowF, m1, m1F, m1Q, m2, m2F, m2Q, high, highF
    const EqPreset eqPresetList[] = {
        { "Flat (reset)",       {  20, 20000,  0, 120,  0, 1000, 1.0,  0, 4000, 1.0,  0, 6000 } },
        { "Vocal presence",     {  90, 20000,  0, 120, -2,  300, 1.0,  3, 4500, 0.9,  2.5f, 10000 } },
        { "Clean up lows",      {  80, 20000,  0, 120,  0, 1000, 1.0,  0, 4000, 1.0,  0, 6000 } },
        { "Mud cut",            {  30, 20000,  0, 120, -4,  250, 1.2,  0, 4000, 1.0,  0, 6000 } },
        { "Warm",               {  20, 20000,  3, 140,  1.5f, 400, 0.8, 0, 4000, 1.0, -2, 8000 } },
        { "Bright and airy",    {  20, 20000,  0, 120,  0, 1000, 1.0,  2, 5000, 0.8,  4, 9000 } },
        { "Kick tight",         {  30, 20000,  4,  60, -4,  350, 1.2,  3, 4000, 1.5,  0, 6000 } },
        { "Telephone",          { 400,  3400,  0, 120,  4, 1500, 1.0,  0, 4000, 1.0,  0, 6000 } },
    };

    struct CompPreset { const char* name; float v[10]; };   // threshold, ratio, knee, attack, release, makeup, mix, rms, sc filter, auto makeup
    const CompPreset compPresetList[] = {
        { "Default",            { -18, 3,  6, 10, 120, 0, 1.0f, 0.3f,  20, 0 } },
        { "Vocal",              { -20, 3,  8,  8, 120, 0, 1.0f, 0.5f,  80, 1 } },
        { "Drums: punch",       { -18, 4,  3, 30, 100, 0, 1.0f, 0.2f,  20, 1 } },
        { "Bass: level it",     { -24, 4,  6, 15, 150, 0, 1.0f, 0.6f,  20, 1 } },
        { "Bus glue",           { -14, 2, 10, 30, 200, 0, 1.0f, 0.5f,  60, 1 } },
        { "Parallel smash",     { -30, 10, 4,  2, 120, 0, 0.4f, 0.2f, 100, 1 } },
        { "Soft limiter",       {  -6, 20, 2, 0.3f, 80, 0, 1.0f, 0.0f,  20, 0 } },
        { "Ducking (sidechain)", { -30, 6, 6,  3, 160, 0, 1.0f, 0.0f,  20, 0 } },
    };

    struct DelayPreset { const char* name; float v[7]; bool ping; };   // time, feedback, mix, R/L, low cut, high cut, wobble
    const DelayPreset delayPresetList[] = {
        { "Clean echo",     { 350, 0.35f, 0.30f, 1.0f,  20, 20000, 0.0f }, false },
        { "Slapback",       { 110, 0.15f, 0.25f, 1.0f, 100,  8000, 0.0f }, false },
        { "Tape echo",      { 380, 0.45f, 0.30f, 1.0f, 150,  4500, 0.35f }, false },
        { "Ping-pong",      { 320, 0.50f, 0.30f, 1.0f, 100,  9000, 0.0f }, true },
        { "Dub",            { 450, 0.70f, 0.35f, 1.0f, 250,  3000, 0.2f }, false },
        { "Wide stereo",    { 280, 0.35f, 0.25f, 1.5f, 120,  8000, 0.1f }, false },
    };

    struct ReverbPreset { const char* name; float v[8]; };   // size, decay, pre-delay, damping, low cut, width, motion, mix
    const ReverbPreset reverbPresetList[] = {
        { "Default",        { 0.5f, 2.0f,  0, 0.5f,  20, 1.0f, 0.3f, 0.25f } },
        { "Ambience",       { 0.3f, 0.5f,  0, 0.35f, 200, 1.0f, 0.2f, 0.15f } },
        { "Small room",     { 0.25f, 0.7f, 5, 0.5f,  80, 0.8f, 0.2f, 0.20f } },
        { "Plate",          { 0.5f, 1.8f, 15, 0.25f, 120, 1.0f, 0.4f, 0.25f } },
        { "Hall",           { 0.8f, 3.2f, 30, 0.45f, 100, 1.0f, 0.35f, 0.25f } },
        { "Cathedral",      { 1.0f, 7.0f, 50, 0.6f,  150, 1.0f, 0.5f, 0.30f } },
    };
}

FxPanel::FxPanel (Track& t, std::function<void()> onBeginEdit, Engine* e) : track (t), engine (e), beginEdit (std::move (onBeginEdit))
{
    auto& fx = track.fx;

    // ---- EQ: the curve, then the sliders of the selected band, then presets
    auto& eq = addSection ("EQ", fx.eqOn);
    eqSection = &eq;
    eqCurve = std::make_unique<EqCurveView> (fx, [this] { if (beginEdit) beginEdit(); },
                                             [this] (int b) { selectBand (b); },
                                             [this]
                                             {
                                                 turnOn (*eqSection, track.fx.eqOn);
                                                 syncFromParams();
                                             });
    addAndMakeVisible (*eqCurve);

    const char* bandNames[3] = { "Gain", "Freq", "Q" };
    for (int i = 0; i < 3; ++i)
    {
        auto& row = bandRows[i];
        row.label.setText (bandNames[i], juce::dontSendNotification);
        row.label.setColour (juce::Label::textColourId, Theme::text.withAlpha (0.8f));
        addAndMakeVisible (row.label);
        row.slider.setSliderStyle (juce::Slider::LinearHorizontal);
        row.slider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 70, 20);
        row.slider.onDragStart = [this] { if (beginEdit) beginEdit(); };
        row.slider.onValueChange = [this, i]
        {
            if (bandRows[i].param == nullptr)
                return;
            *bandRows[i].param = (float) bandRows[i].slider.getValue();
            turnOn (*eqSection, track.fx.eqOn);
        };
        addAndMakeVisible (row.slider);
    }
    selectBand (2);

    eqPresets.setTextWhenNothingSelected ("EQ presets...");
    for (int i = 0; i < (int) std::size (eqPresetList); ++i)
        eqPresets.addItem (eqPresetList[i].name, i + 1);
    eqPresets.onChange = [this]
    {
        const int id = eqPresets.getSelectedId();
        if (id > 0)
            applyEqPreset (id - 1);
        eqPresets.setSelectedId (0, juce::dontSendNotification);
    };
    addAndMakeVisible (eqPresets);
    eqReset.setTooltip ("Flat: all bands back to nothing");
    eqReset.onClick = [this] { applyEqPreset (0); };
    addAndMakeVisible (eqReset);

    // ---- compressor
    auto& comp = addSection ("Compressor", fx.compOn);
    addRow (comp, "Threshold", -60.0, 0.0, 0.0,     " dB", 1, fx.compThreshold);
    addRow (comp, "Ratio",     1.0, 20.0, 4.0,      " :1", 1, fx.compRatio);
    addRow (comp, "Knee",      0.0, 24.0, 0.0,      " dB", 1, fx.compKnee);
    addRow (comp, "Attack",    0.1, 200.0, 20.0,    " ms", 1, fx.compAttack);
    addRow (comp, "Release",   5.0, 2000.0, 150.0,  " ms", 0, fx.compRelease);
    addRow (comp, "Makeup",    0.0, 24.0, 0.0,      " dB", 1, fx.compMakeup);
    addRow (comp, "Mix",       0.0, 1.0, 0.0,       "", 2, fx.compMix);
    addRow (comp, "Detector",  0.0, 1.0, 0.0,       "", 2, fx.compRms);
    addRow (comp, "SC filter", 20.0, 500.0, 100.0,  " Hz", 0, fx.compScHpf);
    compSection = &comp;
    comp.rows[2]->slider.setTooltip ("Knee: 0 = the compression starts abruptly at the threshold, more = it eases in over this many dB (gentler, more natural)");
    comp.rows[6]->slider.setTooltip ("Mix of the compressed and the original signal: 1 = fully compressed, in between = parallel compression (keeps the punch)");
    comp.rows[7]->slider.setTooltip ("What the compressor listens to: 0 = peaks (fast, catches every transient), 1 = RMS (average loudness, smoother and more musical)");
    comp.rows[8]->slider.setTooltip ("A high-pass filter in front of the detector (20 Hz = off): low bass then doesn't make the compressor pump");

    autoToggle.setToggleState (fx.compAuto, juce::dontSendNotification);
    autoToggle.setTooltip ("Makeup that follows the threshold and ratio, so the level stays about the same while you set the compressor (the Makeup slider adds to it)");
    autoToggle.onClick = [this]
    {
        if (beginEdit) beginEdit();
        track.fx.compAuto = autoToggle.getToggleState();
    };
    addAndMakeVisible (autoToggle);

    compPresets.setTextWhenNothingSelected ("Compressor presets...");
    for (int i = 0; i < (int) std::size (compPresetList); ++i)
        compPresets.addItem (compPresetList[i].name, i + 1);
    compPresets.onChange = [this]
    {
        const int id = compPresets.getSelectedId();
        if (id > 0)
            applyCompPreset (id - 1);
        compPresets.setSelectedId (0, juce::dontSendNotification);
    };
    addAndMakeVisible (compPresets);

    compView = std::make_unique<CompView> (fx);
    addAndMakeVisible (*compView);

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
            if (source != nullptr && ! track.fx.compOn.load())   // a compressor that ducks: fast attack, a release that breathes
            {
                applyCompPreset ((int) std::size (compPresetList) - 1, false);
                turnOn (*compSection, track.fx.compOn);
            }
        };
        addAndMakeVisible (sideBox);
    }

    auto& delay = addSection ("Delay", fx.delayOn);
    delaySection = &delay;
    addRow (delay, "Time",     20.0, 1500.0, 400.0, " ms", 0, fx.delayTime);
    addRow (delay, "Feedback", 0.0, 0.9, 0.0,     "", 2, fx.delayFeedback);
    addRow (delay, "Mix",      0.0, 1.0, 0.0,     "", 2, fx.delayMix);
    addRow (delay, "R / L",    0.25, 2.0, 1.0,    " x", 2, fx.delayRatio);
    addRow (delay, "Low cut",  20.0, 2000.0, 200.0, " Hz", 0, fx.delayLowCut);
    addRow (delay, "High cut", 1000.0, 20000.0, 6000.0, " Hz", 0, fx.delayHighCut);
    addRow (delay, "Wobble",   0.0, 1.0, 0.0,     "", 2, fx.delayMod);
    delay.rows[3]->slider.setTooltip ("Time of the right echo compared to the left one: 1 = the same, 1.5 = a wide, dotted feel");
    delay.rows[4]->slider.setTooltip ("Cuts the lows in every repeat (20 Hz = off), so the echoes don't make the mix muddy");
    delay.rows[5]->slider.setTooltip ("Cuts the highs in every repeat (20 kHz = off): each echo gets darker, like tape");
    delay.rows[6]->slider.setTooltip ("A slow wobble of the time: tape or chorus-like movement");

    pingToggle.setToggleState (fx.delayPing, juce::dontSendNotification);
    pingToggle.setTooltip ("Ping-pong: the echoes bounce between left and right");
    pingToggle.onClick = [this]
    {
        if (beginEdit) beginEdit();
        track.fx.delayPing = pingToggle.getToggleState();
    };
    addAndMakeVisible (pingToggle);

    if (engine != nullptr)
    {
        delaySync.setTextWhenNothingSelected ("Sync to tempo...");
        const char* names[] = { "1/2", "1/4 dotted", "1/4", "1/8 dotted", "1/8", "1/8 triplet", "1/16 dotted", "1/16", "1/16 triplet" };
        for (int i = 0; i < 9; ++i)
            delaySync.addItem (names[i], i + 1);
        delaySync.setTooltip ("Sets the delay time to a note length of the project tempo");
        delaySync.onChange = [this]
        {
            static const double quarters[9] = { 2.0, 1.5, 1.0, 0.75, 0.5, 1.0 / 3.0, 0.375, 0.25, 1.0 / 6.0 };
            const int id = delaySync.getSelectedId();
            delaySync.setSelectedId (0, juce::dontSendNotification);
            if (id < 1 || engine == nullptr)
                return;
            if (beginEdit) beginEdit();
            const double ms = 60000.0 / engine->getTempoMap().bpmAt (0.0) * quarters[id - 1];
            track.fx.delayTime = (float) juce::jlimit (20.0, 1500.0, ms);
            turnOn (*delaySection, track.fx.delayOn);
            syncFromParams();
        };
        addAndMakeVisible (delaySync);
    }

    delayPresets.setTextWhenNothingSelected ("Delay presets...");
    for (int i = 0; i < (int) std::size (delayPresetList); ++i)
        delayPresets.addItem (delayPresetList[i].name, i + 1);
    delayPresets.onChange = [this]
    {
        const int id = delayPresets.getSelectedId();
        if (id > 0)
            applyDelayPreset (id - 1);
        delayPresets.setSelectedId (0, juce::dontSendNotification);
    };
    addAndMakeVisible (delayPresets);

    auto& reverb = addSection ("Reverb", fx.reverbOn);
    reverbSection = &reverb;
    addRow (reverb, "Size",      0.0, 1.0, 0.0,   "", 2, fx.reverbSize);
    addRow (reverb, "Decay",     0.2, 10.0, 2.0,  " s", 1, fx.reverbDecay);
    addRow (reverb, "Pre-delay", 0.0, 200.0, 40.0, " ms", 0, fx.reverbPreDelay);
    addRow (reverb, "Damping",   0.0, 1.0, 0.0,   "", 2, fx.reverbDamping);
    addRow (reverb, "Low cut",   20.0, 1000.0, 150.0, " Hz", 0, fx.reverbLowCut);
    addRow (reverb, "Width",     0.0, 1.0, 0.0,   "", 2, fx.reverbWidth);
    addRow (reverb, "Motion",    0.0, 1.0, 0.0,   "", 2, fx.reverbMod);
    addRow (reverb, "Mix",       0.0, 1.0, 0.0,   "", 2, fx.reverbMix);
    reverb.rows[0]->slider.setTooltip ("The size of the room: bigger = the reflections are further apart");
    reverb.rows[1]->slider.setTooltip ("How long the tail lasts (the time it takes to fade by 60 dB)");
    reverb.rows[2]->slider.setTooltip ("A pause before the reverb starts: keeps the source clear and in front");
    reverb.rows[3]->slider.setTooltip ("How fast the highs die in the tail: more = darker, softer");
    reverb.rows[4]->slider.setTooltip ("Cuts the lows from the reverb so it doesn't muddy the mix");
    reverb.rows[5]->slider.setTooltip ("0 = the reverb in mono, 1 = wide stereo");
    reverb.rows[6]->slider.setTooltip ("A slow movement in the tail: smoother, less metallic ringing");

    reverbPresets.setTextWhenNothingSelected ("Reverb presets...");
    for (int i = 0; i < (int) std::size (reverbPresetList); ++i)
        reverbPresets.addItem (reverbPresetList[i].name, i + 1);
    reverbPresets.onChange = [this]
    {
        const int id = reverbPresets.getSelectedId();
        if (id > 0)
            applyReverbPreset (id - 1);
        reverbPresets.setSelectedId (0, juce::dontSendNotification);
    };
    addAndMakeVisible (reverbPresets);

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

    setSize (1200, 2 * (titleHeight + bottomRows * rowHeight + 6) + gap + gap + 34 + 8);
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
    row->param = &param;
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

void FxPanel::turnOn (Section& s, std::atomic<bool>& flag)
{
    if (! flag.load())
    {
        flag = true;
        s.toggle.setToggleState (true, juce::dontSendNotification);
    }
}

// The three sliders under the curve edit one band at a time (the one picked in the curve)
void FxPanel::selectBand (int b)
{
    band = juce::jlimit (0, numBands - 1, b);
    const auto info = bandInfo (track.fx, band);
    std::atomic<float>* params[3] = { info.gain, info.freq, info.q };
    const double lo[3] = { -18.0, (double) info.freqLo, 0.2 };
    const double hi[3] = { 18.0, (double) info.freqHi, 10.0 };
    const double mid[3] = { 0.0, std::sqrt ((double) info.freqLo * (double) info.freqHi), 1.0 };
    const char* suffix[3] = { " dB", " Hz", "" };
    const int decimals[3] = { 1, 0, 2 };

    for (int i = 0; i < 3; ++i)
    {
        auto& row = bandRows[i];
        row.param = nullptr;   // (so the changes below don't write anything)
        row.slider.setRange (lo[i], hi[i]);
        row.slider.setSkewFactor (1.0);
        if (mid[i] > lo[i] && mid[i] < hi[i] && i > 0)
            row.slider.setSkewFactorFromMidPoint (mid[i]);
        row.slider.setTextValueSuffix (suffix[i]);
        row.slider.setNumDecimalPlacesToDisplay (decimals[i]);
        row.slider.setEnabled (params[i] != nullptr);
        row.label.setEnabled (params[i] != nullptr);
        if (params[i] != nullptr)
        {
            row.slider.setValue (params[i]->load(), juce::dontSendNotification);
            row.slider.setDoubleClickReturnValue (true, i == 0 ? 0.0 : i == 2 ? 1.0 : (double) info.freq->load());
        }
        else
            row.slider.setValue (i == 2 ? 1.0 : 0.0, juce::dontSendNotification);   // (nothing to set: shows a neutral value, greyed out)
        row.param = params[i];
    }
    bandRows[0].label.setText (info.gain != nullptr ? juce::String (band + 1) + " Gain" : juce::String (band + 1) + " ---", juce::dontSendNotification);
    if (auto* curve = dynamic_cast<EqCurveView*> (eqCurve.get()))
        curve->setSelected (band);
}

void FxPanel::syncFromParams()
{
    for (auto& s : sections)
    {
        for (auto& row : s->rows)
            if (row->param != nullptr)
                row->slider.setValue (row->param->load(), juce::dontSendNotification);
        s->toggle.setToggleState (s.get() == eqSection ? track.fx.eqOn.load() : s.get() == compSection ? track.fx.compOn.load() : s->toggle.getToggleState(),
                                  juce::dontSendNotification);
    }
    for (auto& row : bandRows)
        if (row.param != nullptr)
            row.slider.setValue (row.param->load(), juce::dontSendNotification);
    autoToggle.setToggleState (track.fx.compAuto, juce::dontSendNotification);
    pingToggle.setToggleState (track.fx.delayPing, juce::dontSendNotification);
}

void FxPanel::applyDelayPreset (int index)
{
    if (beginEdit) beginEdit();
    auto& fx = track.fx;
    std::atomic<float>* targets[7] = { &fx.delayTime, &fx.delayFeedback, &fx.delayMix, &fx.delayRatio, &fx.delayLowCut, &fx.delayHighCut, &fx.delayMod };
    for (int i = 0; i < 7; ++i)
        targets[i]->store (delayPresetList[index].v[i]);
    fx.delayPing = delayPresetList[index].ping;
    if (index > 0)
        turnOn (*delaySection, fx.delayOn);
    syncFromParams();
}

void FxPanel::applyReverbPreset (int index)
{
    if (beginEdit) beginEdit();
    auto& fx = track.fx;
    std::atomic<float>* targets[8] = { &fx.reverbSize, &fx.reverbDecay, &fx.reverbPreDelay, &fx.reverbDamping, &fx.reverbLowCut, &fx.reverbWidth, &fx.reverbMod, &fx.reverbMix };
    for (int i = 0; i < 8; ++i)
        targets[i]->store (reverbPresetList[index].v[i]);
    if (index > 0)
        turnOn (*reverbSection, fx.reverbOn);
    syncFromParams();
}

void FxPanel::applyEqPreset (int index)
{
    if (beginEdit) beginEdit();
    auto& fx = track.fx;
    std::atomic<float>* targets[12] = { &fx.eqHpFreq, &fx.eqLpFreq, &fx.eqLow, &fx.eqLowFreq, &fx.eqMid, &fx.eqMidFreq, &fx.eqMidQ,
                                        &fx.eqMid2, &fx.eqMid2Freq, &fx.eqMid2Q, &fx.eqHigh, &fx.eqHighFreq };
    for (int i = 0; i < 12; ++i)
        targets[i]->store (eqPresetList[index].v[i]);
    if (index > 0)
        turnOn (*eqSection, fx.eqOn);
    selectBand (band);
    syncFromParams();
}

void FxPanel::applyCompPreset (int index, bool withUndo)
{
    if (withUndo && beginEdit) beginEdit();
    auto& fx = track.fx;
    std::atomic<float>* targets[9] = { &fx.compThreshold, &fx.compRatio, &fx.compKnee, &fx.compAttack, &fx.compRelease, &fx.compMakeup,
                                       &fx.compMix, &fx.compRms, &fx.compScHpf };
    const auto& preset = compPresetList[index];
    for (int i = 0; i < 9; ++i)
        targets[i]->store (preset.v[i]);
    fx.compAuto = preset.v[9] > 0.5f;
    if (index > 0)
        turnOn (*compSection, fx.compOn);
    syncFromParams();
}

void FxPanel::paint (juce::Graphics& g)
{
    g.fillAll (Theme::panel);

    g.setColour (Theme::laneBg);
    const auto b = getLocalBounds().reduced (gap / 2);
    for (int i = 0; i < (int) sections.size(); ++i)
        g.drawRoundedRectangle (sectionRect (b, i).withHeight (titleHeight + (i < 2 ? topRows : bottomRows) * rowHeight + 6).toFloat(), 6.0f, 1.0f);
}

void FxPanel::resized()
{
    const auto b = getLocalBounds().reduced (gap / 2);
    monoToggle.setBounds (b.getX() + 12, b.getBottom() - 30, b.getWidth() - 24, 26);

    for (int i = 0; i < (int) sections.size(); ++i)
    {
        auto& s = *sections[(size_t) i];
        auto area = sectionRect (b, i).reduced (10, 0);

        s.toggle.setBounds (area.removeFromTop (titleHeight).reduced (0, 3));
        for (auto& row : s.rows)
        {
            auto r = area.removeFromTop (rowHeight);
            row->label.setBounds (r.removeFromLeft (70));
            row->slider.setBounds (r);
        }
        if (&s == eqSection)
        {
            eqCurve->setBounds (area.removeFromTop (area.getHeight() - 4 * rowHeight - 6).reduced (0, 3));   // (all the room left over, the sliders and presets under it)
            for (auto& row : bandRows)
            {
                auto r = area.removeFromTop (rowHeight);
                row.label.setBounds (r.removeFromLeft (70));
                row.slider.setBounds (r);
            }
            auto r = area.removeFromTop (rowHeight).reduced (0, 2);
            eqReset.setBounds (r.removeFromRight (60));
            r.removeFromRight (6);
            eqPresets.setBounds (r);
        }
        if (&s == delaySection)
        {
            auto r = area.removeFromTop (rowHeight).reduced (0, 2);
            pingToggle.setBounds (r.removeFromLeft (r.getWidth() / 2 - 3));
            r.removeFromLeft (6);
            delaySync.setBounds (r);
            delayPresets.setBounds (area.removeFromTop (rowHeight).reduced (0, 2));
        }
        if (&s == reverbSection)
            reverbPresets.setBounds (area.removeFromTop (rowHeight).reduced (0, 2));
        if (&s == compSection)
        {
            auto r = area.removeFromTop (rowHeight).reduced (0, 2);
            autoToggle.setBounds (r.removeFromLeft (r.getWidth() / 2 - 3));
            r.removeFromLeft (6);
            compPresets.setBounds (r);
            if (sideBox.getNumItems() > 0)
            {
                auto sr = area.removeFromTop (rowHeight);
                sideLabel.setBounds (sr.removeFromLeft (70));
                sideBox.setBounds (sr.reduced (0, 2));
            }
            compView->setBounds (area.removeFromTop (area.getHeight() - 6).reduced (0, 3));
        }
    }
}

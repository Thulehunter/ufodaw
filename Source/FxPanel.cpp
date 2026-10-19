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
    juce::String hzText (float f) { return f >= 1000.0f ? juce::String (f / 1000.0f, f >= 10000.0f ? 1 : 2) + " kHz" : juce::String ((int) std::round (f)) + " Hz"; }
}

// The frequency response of the EQ, 20 Hz - 20 kHz and +-18 dB, with a node for each band: drag it sideways for the frequency and up and down for the gain,
// scroll over a peak for its Q, double-click to reset the band. The curve is computed from the same coefficients the audio uses.
class EqCurveView : public juce::Component, private juce::Timer
{
public:
    EqCurveView (FxParams& p, const SpectrumTap* spectrumTap, std::function<void()> beginEdit, std::function<void (int)> select, std::function<void()> changed)
        : fx (p), tap (spectrumTap), onBegin (std::move (beginEdit)), onSelect (std::move (select)), onChanged (std::move (changed))
    {
        fx.analyse = tap != nullptr;   // (the audio thread keeps a copy of the sound while this is open)
        bars.assign ((size_t) numBars, -120.0f);
        startTimerHz (20);
    }

    ~EqCurveView() override { fx.analyse = false; }

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

        // the spectrum of the sound (after the EQ) behind the curve: -80 dB at the bottom, -10 dB at the top
        if (tap != nullptr)
        {
            juce::Path spectrum;
            spectrum.startNewSubPath (plot.getX(), plot.getBottom());
            for (int b = 0; b < numBars; ++b)
            {
                const float level = juce::jlimit (0.0f, 1.0f, (bars[(size_t) b] + 80.0f) / 70.0f);
                spectrum.lineTo (plot.getX() + plot.getWidth() * ((float) b + 0.5f) / (float) numBars, plot.getBottom() - level * plot.getHeight());
            }
            spectrum.lineTo (plot.getRight(), plot.getBottom());
            spectrum.closeSubPath();
            g.setColour (Theme::orange.withAlpha (0.22f));
            g.fillPath (spectrum);
            g.setColour (Theme::orange.withAlpha (0.5f));
            g.strokePath (spectrum, juce::PathStrokeType (1.0f));
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
    void timerCallback() override
    {
        if (tap != nullptr)
        {
            constexpr int n = 4096;
            float samples[n];
            std::vector<float> db;
            if (tap->readLatest (samples, n))
                spectrumDb (samples, n, db);
            else
                db.assign (n / 2, -120.0f);

            const double binHz = lastRate / n;
            for (int b = 0; b < numBars; ++b)
            {
                const double f0 = frequencyOf ((double) b / numBars), f1 = frequencyOf ((double) (b + 1) / numBars);
                const int i0 = juce::jlimit (1, n / 2 - 1, (int) std::floor (f0 / binHz)), i1 = juce::jlimit (i0, n / 2 - 1, (int) std::ceil (f1 / binHz));
                float m = -120.0f;
                for (int i = i0; i <= i1; ++i)
                    m = juce::jmax (m, db[(size_t) i]);
                bars[(size_t) b] = m > bars[(size_t) b] ? m : juce::jmax (m, bars[(size_t) b] - 3.0f);   // (rises at once, falls smoothly)
            }
        }
        repaint();
    }

    static constexpr int numBars = 120;
    std::vector<float> bars;
public:
    double lastRate = 48000.0;
private:
    FxParams& fx;
    const SpectrumTap* tap = nullptr;
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
    eqCurve = std::make_unique<EqCurveView> (fx, track.fxState != nullptr ? &track.fxState->getSpectrumTap() : nullptr,
                                             [this] { if (beginEdit) beginEdit(); },
                                             [this] (int b) { selectBand (b); },
                                             [this]
                                             {
                                                 turnOn (*eqSection, track.fx.eqOn);
                                                 syncFromParams();
                                             });
    addAndMakeVisible (*eqCurve);
    if (engine != nullptr)
        static_cast<EqCurveView*> (eqCurve.get())->lastRate = engine->getSampleRate();

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


    // ---- page 2: gate, de-esser, multiband compressor, limiter
    auto& gate = addSection ("Gate", fx.gateOn);
    gate.page = 1;
    addRow (gate, "Threshold", -80.0, 0.0, -40.0, " dB", 1, fx.gateThreshold);
    addRow (gate, "Hysteresis", 0.0, 12.0, 4.0,   " dB", 1, fx.gateHyst);
    addRow (gate, "Range",     0.0, 80.0, 40.0,   " dB", 0, fx.gateRange);
    addRow (gate, "Attack",    0.1, 50.0, 5.0,    " ms", 1, fx.gateAttack);
    addRow (gate, "Hold",      0.0, 500.0, 80.0,  " ms", 0, fx.gateHold);
    addRow (gate, "Release",   10.0, 1000.0, 150.0, " ms", 0, fx.gateRelease);
    addRow (gate, "SC low cut", 20.0, 2000.0, 200.0, " Hz", 0, fx.gateScHpf);
    addRow (gate, "SC high cut", 1000.0, 20000.0, 6000.0, " Hz", 0, fx.gateScLpf);
    gate.rows[0]->slider.setTooltip ("The gate opens when the sound goes over this level (cuts noise and bleed between notes)");
    gate.rows[1]->slider.setTooltip ("It closes this many dB lower than where it opens: no chattering around the threshold");
    gate.rows[2]->slider.setTooltip ("How far the gate turns the sound down when it is closed");
    gate.rows[6]->slider.setTooltip ("A filter in front of the detector (20 Hz = off): the gate ignores sound below this frequency (rumble, kick bleed)");
    gate.rows[7]->slider.setTooltip ("A filter in front of the detector (20 kHz = off): the gate ignores sound above this frequency (hats bleeding into a snare)");
    addMeter (gate, [&fx] { return fx.gateGr.load(); }, "closed", 60.0f);
    addPresets (gate, fx.gateOn, {
        { "Default",        { { &fx.gateThreshold, -50.0f }, { &fx.gateHyst, 4.0f }, { &fx.gateRange, 40.0f }, { &fx.gateAttack, 1.0f }, { &fx.gateHold, 50.0f }, { &fx.gateRelease, 150.0f }, { &fx.gateScHpf, 20.0f }, { &fx.gateScLpf, 20000.0f } }, {} },
        { "Tight drums",    { { &fx.gateThreshold, -35.0f }, { &fx.gateHyst, 5.0f }, { &fx.gateRange, 60.0f }, { &fx.gateAttack, 0.3f }, { &fx.gateHold, 30.0f }, { &fx.gateRelease, 80.0f }, { &fx.gateScHpf, 80.0f }, { &fx.gateScLpf, 20000.0f } }, {} },
        { "Vocal, noise between phrases", { { &fx.gateThreshold, -45.0f }, { &fx.gateHyst, 6.0f }, { &fx.gateRange, 20.0f }, { &fx.gateAttack, 3.0f }, { &fx.gateHold, 120.0f }, { &fx.gateRelease, 250.0f }, { &fx.gateScHpf, 120.0f }, { &fx.gateScLpf, 20000.0f } }, {} },
        { "Guitar, no hum", { { &fx.gateThreshold, -48.0f }, { &fx.gateHyst, 4.0f }, { &fx.gateRange, 45.0f }, { &fx.gateAttack, 1.0f }, { &fx.gateHold, 60.0f }, { &fx.gateRelease, 120.0f }, { &fx.gateScHpf, 200.0f }, { &fx.gateScLpf, 8000.0f } }, {} },
    });

    auto& deess = addSection ("De-esser", fx.deEssOn);
    deess.page = 1;
    addRow (deess, "Frequency", 2000.0, 14000.0, 6000.0, " Hz", 0, fx.deEssFreq);
    addRow (deess, "Width",     0.3, 6.0, 1.5,    "", 1, fx.deEssQ);
    addRow (deess, "Threshold", -60.0, 0.0, -30.0, " dB", 1, fx.deEssThreshold);
    addRow (deess, "Amount",    0.0, 20.0, 8.0,   " dB", 1, fx.deEssAmount);
    deess.rows[0]->slider.setTooltip ("Where the harsh 's' sounds are (usually 5-9 kHz)");
    deess.rows[1]->slider.setTooltip ("Q of the band the de-esser listens to (and, in split mode, lowers): a lower number is a wider band");
    deess.rows[3]->slider.setTooltip ("The most the de-esser takes off");
    addToggle (deess, fx.deEssWide, "Whole sound", "Off: only the 's' band is turned down (natural). On: the whole sound is turned down, like a compressor that hears only the band");
    addToggle (deess, fx.deEssListen, "Listen", "Hear only the band the de-esser listens to: sweep the frequency until the 's' sounds are loudest", true);
    addMeter (deess, [&fx] { return fx.deEssGr.load(); }, "reduction", 20.0f);
    addPresets (deess, fx.deEssOn, {
        { "Default",        { { &fx.deEssFreq, 6500.0f }, { &fx.deEssQ, 1.5f }, { &fx.deEssThreshold, -28.0f }, { &fx.deEssAmount, 8.0f } }, { { &fx.deEssWide, false } } },
        { "Male vocal",     { { &fx.deEssFreq, 5500.0f }, { &fx.deEssQ, 1.5f }, { &fx.deEssThreshold, -28.0f }, { &fx.deEssAmount, 8.0f } }, { { &fx.deEssWide, false } } },
        { "Female vocal",   { { &fx.deEssFreq, 7500.0f }, { &fx.deEssQ, 1.5f }, { &fx.deEssThreshold, -28.0f }, { &fx.deEssAmount, 9.0f } }, { { &fx.deEssWide, false } } },
        { "Harsh cymbals",  { { &fx.deEssFreq, 9500.0f }, { &fx.deEssQ, 0.8f }, { &fx.deEssThreshold, -30.0f }, { &fx.deEssAmount, 6.0f } }, { { &fx.deEssWide, false } } },
        { "Strong (whole sound)", { { &fx.deEssFreq, 6500.0f }, { &fx.deEssQ, 1.2f }, { &fx.deEssThreshold, -32.0f }, { &fx.deEssAmount, 12.0f } }, { { &fx.deEssWide, true } } },
    });

    auto& mb = addSection ("Multiband compressor", fx.mbOn);
    mb.page = 1;
    addRow (mb, "Low split",  40.0, 1000.0, 200.0, " Hz", 0, fx.mbLowFreq);
    addRow (mb, "High split", 300.0, 12000.0, 2500.0, " Hz", 0, fx.mbHighFreq);
    addRow (mb, "Low thr",    -60.0, 0.0, 0.0,    " dB", 1, fx.mbLowThr);
    addRow (mb, "Mid thr",    -60.0, 0.0, 0.0,    " dB", 1, fx.mbMidThr);
    addRow (mb, "High thr",   -60.0, 0.0, 0.0,    " dB", 1, fx.mbHighThr);
    addRow (mb, "Ratio",      1.0, 10.0, 3.0,     " :1", 1, fx.mbRatio);
    addRow (mb, "Attack",     0.5, 100.0, 10.0,   " ms", 1, fx.mbAttack);
    addRow (mb, "Release",    5.0, 1000.0, 150.0, " ms", 0, fx.mbRelease);
    addRow (mb, "Low gain",   -12.0, 12.0, 0.0,   " dB", 1, fx.mbLowGain);
    addRow (mb, "Mid gain",   -12.0, 12.0, 0.0,   " dB", 1, fx.mbMidGain);
    addRow (mb, "High gain",  -12.0, 12.0, 0.0,   " dB", 1, fx.mbHighGain);
    addRow (mb, "Makeup",     0.0, 12.0, 0.0,     " dB", 1, fx.mbMakeup);
    mb.rows[0]->slider.setTooltip ("Three bands: low, mid and high, compressed separately. The splits are 24 dB/oct and add up flat again.");
    mb.rows[8]->slider.setTooltip ("The level of the band after its compression (a tone control per band)");
    addMeter (mb, [&fx] { return fx.mbGr[0].load(); }, "low", 20.0f);
    addMeter (mb, [&fx] { return fx.mbGr[1].load(); }, "mid", 20.0f);
    addMeter (mb, [&fx] { return fx.mbGr[2].load(); }, "high", 20.0f);
    addPresets (mb, fx.mbOn, {
        { "Default",        { { &fx.mbLowFreq, 200.0f }, { &fx.mbHighFreq, 2500.0f }, { &fx.mbLowThr, -24.0f }, { &fx.mbMidThr, -24.0f }, { &fx.mbHighThr, -24.0f }, { &fx.mbRatio, 3.0f }, { &fx.mbAttack, 8.0f }, { &fx.mbRelease, 120.0f }, { &fx.mbLowGain, 0.0f }, { &fx.mbMidGain, 0.0f }, { &fx.mbHighGain, 0.0f }, { &fx.mbMakeup, 0.0f } }, {} },
        { "Mastering glue", { { &fx.mbLowFreq, 120.0f }, { &fx.mbHighFreq, 3500.0f }, { &fx.mbLowThr, -22.0f }, { &fx.mbMidThr, -20.0f }, { &fx.mbHighThr, -22.0f }, { &fx.mbRatio, 2.0f }, { &fx.mbAttack, 20.0f }, { &fx.mbRelease, 200.0f }, { &fx.mbLowGain, 0.0f }, { &fx.mbMidGain, 0.0f }, { &fx.mbHighGain, 0.0f }, { &fx.mbMakeup, 1.5f } }, {} },
        { "Tight low end",  { { &fx.mbLowFreq, 150.0f }, { &fx.mbHighFreq, 4000.0f }, { &fx.mbLowThr, -28.0f }, { &fx.mbMidThr, 0.0f }, { &fx.mbHighThr, 0.0f }, { &fx.mbRatio, 4.0f }, { &fx.mbAttack, 25.0f }, { &fx.mbRelease, 120.0f }, { &fx.mbLowGain, 0.0f }, { &fx.mbMidGain, 0.0f }, { &fx.mbHighGain, 0.0f }, { &fx.mbMakeup, 0.0f } }, {} },
        { "Vocal air",      { { &fx.mbLowFreq, 200.0f }, { &fx.mbHighFreq, 5000.0f }, { &fx.mbLowThr, -30.0f }, { &fx.mbMidThr, -22.0f }, { &fx.mbHighThr, -30.0f }, { &fx.mbRatio, 3.0f }, { &fx.mbAttack, 8.0f }, { &fx.mbRelease, 100.0f }, { &fx.mbLowGain, 0.0f }, { &fx.mbMidGain, 0.0f }, { &fx.mbHighGain, 2.5f }, { &fx.mbMakeup, 0.0f } }, {} },
        { "Loud and even",  { { &fx.mbLowFreq, 160.0f }, { &fx.mbHighFreq, 3000.0f }, { &fx.mbLowThr, -26.0f }, { &fx.mbMidThr, -26.0f }, { &fx.mbHighThr, -28.0f }, { &fx.mbRatio, 4.0f }, { &fx.mbAttack, 6.0f }, { &fx.mbRelease, 100.0f }, { &fx.mbLowGain, 0.0f }, { &fx.mbMidGain, 0.0f }, { &fx.mbHighGain, 0.0f }, { &fx.mbMakeup, 3.0f } }, {} },
    });

    auto& limiter = addSection ("Limiter", fx.limOn);
    limiter.page = 1;
    addRow (limiter, "Gain",      0.0, 18.0, 0.0,   " dB", 1, fx.limGain);
    addRow (limiter, "Ceiling",   -12.0, 0.0, 0.0,  " dB", 1, fx.limCeiling);
    addRow (limiter, "Release",   5.0, 500.0, 80.0, " ms", 0, fx.limRelease);
    addRow (limiter, "Lookahead", 0.0, 10.0, 2.0,   " ms", 1, fx.limLookahead);
    limiter.rows[0]->slider.setTooltip ("Pushes the sound into the limiter: louder, never above the ceiling");
    limiter.rows[3]->slider.setTooltip ("The limiter looks this far ahead, so it can turn the sound down before a peak arrives (no clipping, no pumping). The track is delayed by it, "
                                        "and the program compensates for that.");
    addToggle (limiter, fx.limTruePeak, "True peak", "Also catch the peaks that lie between the samples (they can clip after conversion to MP3 or on a speaker)");
    addMeter (limiter, [&fx] { return fx.limGr.load(); }, "reduction", 18.0f);
    addPresets (limiter, fx.limOn, {
        { "Default",        { { &fx.limGain, 0.0f }, { &fx.limCeiling, -1.0f }, { &fx.limRelease, 80.0f }, { &fx.limLookahead, 2.0f } }, { { &fx.limTruePeak, false } } },
        { "Transparent",    { { &fx.limGain, 0.0f }, { &fx.limCeiling, -1.0f }, { &fx.limRelease, 150.0f }, { &fx.limLookahead, 3.0f } }, { { &fx.limTruePeak, true } } },
        { "Loud master",    { { &fx.limGain, 6.0f }, { &fx.limCeiling, -0.5f }, { &fx.limRelease, 60.0f }, { &fx.limLookahead, 4.0f } }, { { &fx.limTruePeak, true } } },
        { "Safety net",     { { &fx.limGain, 0.0f }, { &fx.limCeiling, -0.3f }, { &fx.limRelease, 250.0f }, { &fx.limLookahead, 1.0f } }, { { &fx.limTruePeak, false } } },
    });

    // ---- page 3: saturation and chorus
    auto& sat = addSection ("Saturation", fx.satOn);
    sat.page = 2;
    addRow (sat, "Drive",  0.0, 30.0, 8.0,   " dB", 1, fx.satDrive);
    addRow (sat, "Mix",    0.0, 1.0, 0.0,    "", 2, fx.satMix);
    addRow (sat, "Output", -12.0, 12.0, 0.0, " dB", 1, fx.satOutput);
    addRow (sat, "Bias",   0.0, 1.0, 0.0,    "", 2, fx.satBias);
    addRow (sat, "Tone",   -1.0, 1.0, 0.0,   "", 2, fx.satTone);
    sat.rows[3]->slider.setTooltip ("Clips one side of the wave more than the other: adds the even harmonics (warmer, like a tube or a transformer)");
    sat.rows[4]->slider.setTooltip ("Left = darker (a low-pass on the saturated sound), right = brighter");
    satTypeLabel.setText ("Type", juce::dontSendNotification);
    satTypeLabel.setColour (juce::Label::textColourId, Theme::text.withAlpha (0.8f));
    addAndMakeVisible (satTypeLabel);
    satTypeBox.addItem ("Tube (smooth)", 1);
    satTypeBox.addItem ("Tape (warm, uneven)", 2);
    satTypeBox.addItem ("Hard clip (harsh)", 3);
    satTypeBox.addItem ("Fold (wave folder)", 4);
    satTypeBox.addItem ("Diode (one side harder)", 5);
    satTypeBox.setSelectedId (juce::jlimit (0, 4, (int) std::lround (fx.satType.load())) + 1, juce::dontSendNotification);
    satTypeBox.onChange = [this]
    {
        if (beginEdit) beginEdit();
        track.fx.satType = (float) (satTypeBox.getSelectedId() - 1);
    };
    addAndMakeVisible (satTypeBox);
    sat.footerRows.push_back ({ &satTypeLabel, &satTypeBox });
    addToggle (sat, fx.satOversample, "4x oversampling", "Runs the clipping at 4 times the sample rate: far less aliasing (no fizzy, inharmonic overtones). "
                                                          "The sound is delayed by 12 samples; the program compensates for that.");
    addPresets (sat, fx.satOn, {
        { "Default",        { { &fx.satDrive, 6.0f }, { &fx.satType, 0.0f }, { &fx.satMix, 1.0f }, { &fx.satOutput, 0.0f }, { &fx.satBias, 0.0f }, { &fx.satTone, 0.0f } }, { { &fx.satOversample, true } } },
        { "Warm tube",      { { &fx.satDrive, 8.0f }, { &fx.satType, 0.0f }, { &fx.satMix, 0.6f }, { &fx.satOutput, 0.0f }, { &fx.satBias, 0.15f }, { &fx.satTone, -0.2f } }, { { &fx.satOversample, true } } },
        { "Tape glue",      { { &fx.satDrive, 6.0f }, { &fx.satType, 1.0f }, { &fx.satMix, 0.7f }, { &fx.satOutput, 0.0f }, { &fx.satBias, 0.3f }, { &fx.satTone, -0.3f } }, { { &fx.satOversample, true } } },
        { "Crunch",         { { &fx.satDrive, 14.0f }, { &fx.satType, 2.0f }, { &fx.satMix, 0.5f }, { &fx.satOutput, -2.0f }, { &fx.satBias, 0.0f }, { &fx.satTone, 0.1f } }, { { &fx.satOversample, true } } },
        { "Fuzz",           { { &fx.satDrive, 24.0f }, { &fx.satType, 4.0f }, { &fx.satMix, 0.85f }, { &fx.satOutput, -4.0f }, { &fx.satBias, 0.4f }, { &fx.satTone, -0.4f } }, { { &fx.satOversample, true } } },
        { "Wave folder",    { { &fx.satDrive, 12.0f }, { &fx.satType, 3.0f }, { &fx.satMix, 0.4f }, { &fx.satOutput, -2.0f }, { &fx.satBias, 0.0f }, { &fx.satTone, 0.0f } }, { { &fx.satOversample, true } } },
    });

    auto& chorus = addSection ("Chorus / flanger", fx.chorusOn);
    chorus.page = 2;
    addRow (chorus, "Rate",     0.05, 5.0, 0.8,  " Hz", 2, fx.chorusRate);
    addRow (chorus, "Depth",    0.0, 1.0, 0.0,   "", 2, fx.chorusDepth);
    addRow (chorus, "Delay",    0.5, 40.0, 10.0, " ms", 1, fx.chorusDelay);
    addRow (chorus, "Feedback", 0.0, 0.9, 0.0,   "", 2, fx.chorusFeedback);
    addRow (chorus, "Mix",      0.0, 1.0, 0.0,   "", 2, fx.chorusMix);
    addRow (chorus, "Voices",   1.0, 4.0, 2.0,   "", 0, fx.chorusVoices);
    addRow (chorus, "Width",    0.0, 1.0, 0.0,   "", 2, fx.chorusWidth);
    addRow (chorus, "Tone",     1000.0, 20000.0, 6000.0, " Hz", 0, fx.chorusTone);
    chorus.rows[2]->slider.setTooltip ("Around 10-25 ms is a chorus; 1-5 ms with some feedback is a flanger");
    chorus.rows[4]->slider.setTooltip ("A crossfade with the original: all the way right gives only the effect (vibrato)");
    chorus.rows[5]->slider.setTooltip ("More voices make a thicker ensemble sound");
    chorus.rows[7]->slider.setTooltip ("A low-pass on the effect: lower = darker, more like an old analog chorus");
    addPresets (chorus, fx.chorusOn, {
        { "Default",        { { &fx.chorusRate, 0.8f }, { &fx.chorusDepth, 0.5f }, { &fx.chorusDelay, 12.0f }, { &fx.chorusFeedback, 0.0f }, { &fx.chorusMix, 0.4f }, { &fx.chorusVoices, 2.0f }, { &fx.chorusWidth, 1.0f }, { &fx.chorusTone, 20000.0f } }, {} },
        { "Classic chorus", { { &fx.chorusRate, 0.6f }, { &fx.chorusDepth, 0.5f }, { &fx.chorusDelay, 14.0f }, { &fx.chorusFeedback, 0.0f }, { &fx.chorusMix, 0.5f }, { &fx.chorusVoices, 2.0f }, { &fx.chorusWidth, 1.0f }, { &fx.chorusTone, 12000.0f } }, {} },
        { "Ensemble",       { { &fx.chorusRate, 0.35f }, { &fx.chorusDepth, 0.7f }, { &fx.chorusDelay, 18.0f }, { &fx.chorusFeedback, 0.0f }, { &fx.chorusMix, 0.55f }, { &fx.chorusVoices, 4.0f }, { &fx.chorusWidth, 1.0f }, { &fx.chorusTone, 9000.0f } }, {} },
        { "Slow flanger",   { { &fx.chorusRate, 0.15f }, { &fx.chorusDepth, 0.8f }, { &fx.chorusDelay, 3.0f }, { &fx.chorusFeedback, 0.6f }, { &fx.chorusMix, 0.5f }, { &fx.chorusVoices, 1.0f }, { &fx.chorusWidth, 0.6f }, { &fx.chorusTone, 20000.0f } }, {} },
        { "Jet flanger",    { { &fx.chorusRate, 0.3f }, { &fx.chorusDepth, 1.0f }, { &fx.chorusDelay, 2.0f }, { &fx.chorusFeedback, 0.85f }, { &fx.chorusMix, 0.5f }, { &fx.chorusVoices, 1.0f }, { &fx.chorusWidth, 0.3f }, { &fx.chorusTone, 20000.0f } }, {} },
        { "Vibrato",        { { &fx.chorusRate, 5.5f }, { &fx.chorusDepth, 0.3f }, { &fx.chorusDelay, 6.0f }, { &fx.chorusFeedback, 0.0f }, { &fx.chorusMix, 1.0f }, { &fx.chorusVoices, 1.0f }, { &fx.chorusWidth, 0.0f }, { &fx.chorusTone, 20000.0f } }, {} },
        { "Wide doubler",   { { &fx.chorusRate, 0.2f }, { &fx.chorusDepth, 0.25f }, { &fx.chorusDelay, 22.0f }, { &fx.chorusFeedback, 0.0f }, { &fx.chorusMix, 0.5f }, { &fx.chorusVoices, 2.0f }, { &fx.chorusWidth, 1.0f }, { &fx.chorusTone, 14000.0f } }, {} },
    });

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

    // the extra widgets of the first four effects go in rows of their sections
    compSection->footerRows.push_back ({ &autoToggle, &compPresets });
    if (sideBox.getNumItems() > 0)
        compSection->footerRows.push_back ({ &sideLabel, &sideBox });
    if (delaySync.getNumItems() > 0)
        delaySection->footerRows.push_back ({ &pingToggle, &delaySync });
    else
        delaySection->footerRows.push_back ({ &pingToggle });
    delaySection->footerRows.push_back ({ &delayPresets });
    reverbSection->footerRows.push_back ({ &reverbPresets });

    setSize (1100, 12 + 52 + 12 + 300 + 8 + 30 + 12);
    selectSection (track.fx.eqOn.load() || (! track.fx.compOn.load() && ! track.fx.delayOn.load() && ! track.fx.reverbOn.load()) ? 0 : track.fx.compOn.load() ? 1 : track.fx.delayOn.load() ? 2 : 3);
    startTimerHz (8);
}

// ---- the extra effect pages

// A small bar of a gain reduction (or any level in dB), with its caption and number.
class MeterBar : public juce::Component, private juce::Timer
{
public:
    MeterBar (std::function<float()> readDb, juce::String name, float maxDb) : read (std::move (readDb)), caption (std::move (name)), maxReduction (maxDb) { startTimerHz (30); }

    void paint (juce::Graphics& g) override
    {
        const auto r = getLocalBounds().toFloat().reduced (0.0f, 2.0f);
        g.setColour (Theme::laneBg);
        g.fillRoundedRectangle (r, 3.0f);
        g.setColour (Theme::orange);
        g.fillRoundedRectangle (r.withWidth (r.getWidth() * juce::jlimit (0.0f, 1.0f, shown / maxReduction)), 3.0f);
        g.setColour (Theme::text.withAlpha (0.85f));
        g.setFont (juce::FontOptions (9.5f));
        g.drawText (caption + (shown > 0.05f ? "  -" + juce::String (shown, 1) : juce::String()), r.toNearestInt().reduced (4, 0), juce::Justification::centredLeft);
    }

private:
    void timerCallback() override
    {
        const float now = read();
        shown = juce::jmax (now, shown * 0.88f);
        if (shown < 0.05f) shown = 0.0f;
        repaint();
    }

    std::function<float()> read;
    juce::String caption;
    float maxReduction;
    float shown = 0.0f;
};

juce::ToggleButton& FxPanel::addToggle (Section& s, std::atomic<bool>& param, const juce::String& text, const juce::String& tooltip, bool sameRowAsPrevious)
{
    auto toggle = std::make_unique<juce::ToggleButton> (text);
    auto* raw = toggle.get();
    raw->setToggleState (param.load(), juce::dontSendNotification);
    raw->setTooltip (tooltip);
    raw->onClick = [this, raw, &param]
    {
        if (beginEdit) beginEdit();
        param = raw->getToggleState();
    };
    addAndMakeVisible (*raw);
    toggleLinks.push_back ({ raw, &param });
    owned.push_back (std::move (toggle));
    if (sameRowAsPrevious && ! s.footerRows.empty())
        s.footerRows.back().push_back (raw);
    else
        s.footerRows.push_back ({ raw });
    return *raw;
}

void FxPanel::addMeter (Section& s, std::function<float()> read, const juce::String& caption, float maxDb)
{
    auto meter = std::make_unique<MeterBar> (std::move (read), caption, maxDb);
    addAndMakeVisible (*meter);
    s.meters.push_back (meter.get());
    owned.push_back (std::move (meter));
}

// A box of presets as the last row of a section; choosing one sets the parameters, turns the effect on, and is one undo step.
void FxPanel::addPresets (Section& s, std::atomic<bool>& enabled, std::vector<PresetDef> presets)
{
    auto box = std::make_unique<juce::ComboBox>();
    auto* raw = box.get();
    raw->setTextWhenNothingSelected ("Presets...");
    for (int i = 0; i < (int) presets.size(); ++i)
        raw->addItem (presets[(size_t) i].name, i + 1);
    raw->onChange = [this, raw, &s, &enabled, presets]
    {
        const int id = raw->getSelectedId();
        raw->setSelectedId (0, juce::dontSendNotification);
        if (id < 1 || id > (int) presets.size())
            return;
        if (beginEdit) beginEdit();
        for (auto& [param, value] : presets[(size_t) id - 1].floats) param->store (value);
        for (auto& [param, value] : presets[(size_t) id - 1].bools) param->store (value);
        if (id > 1)
            turnOn (s, enabled);
        syncFromParams();
    };
    addAndMakeVisible (*raw);
    s.footerRows.push_back ({ raw });
    owned.push_back (std::move (box));
}

void FxPanel::setSectionVisible (Section& s, bool show)
{
    s.toggle.setVisible (show);
    for (auto& row : s.rows)
    {
        row->label.setVisible (show);
        row->slider.setVisible (show);
    }
    for (auto& footer : s.footerRows)
        for (auto* c : footer)
            c->setVisible (show);
    for (auto* m : s.meters)
        m->setVisible (show);
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
        // (the on/off boxes of the sections of the extra pages are kept by turnOn)
        s->toggle.setToggleState (s.get() == eqSection ? track.fx.eqOn.load() : s.get() == compSection ? track.fx.compOn.load() : s->toggle.getToggleState(),
                                  juce::dontSendNotification);
    }
    for (auto& row : bandRows)
        if (row.param != nullptr)
            row.slider.setValue (row.param->load(), juce::dontSendNotification);
    autoToggle.setToggleState (track.fx.compAuto, juce::dontSendNotification);
    pingToggle.setToggleState (track.fx.delayPing, juce::dontSendNotification);
    for (auto& [toggle, param] : toggleLinks)
        toggle->setToggleState (param->load(), juce::dontSendNotification);
    satTypeBox.setSelectedId (juce::jlimit (0, 4, (int) std::lround (track.fx.satType.load())) + 1, juce::dontSendNotification);
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

namespace
{
    // The chain strip: the effects in the order the sound goes through them (the numbers are the indexes of the sections)
    constexpr int chainOrder[10] = { 4, 0, 1, 5, 8, 6, 9, 2, 3, 7 };
    const char* const tileNames[10] = { "EQ", "Compressor", "Delay", "Reverb", "Gate", "De-esser", "Multiband", "Limiter", "Saturation", "Chorus" };   // by section index
    constexpr int detailRowHeight = 30;
}

juce::Rectangle<int> FxPanel::tileRect (int k) const
{
    const auto b = getLocalBounds().reduced (14, 12);
    const int gapTile = 6, w = (b.getWidth() - 9 * gapTile) / 10;
    return { b.getX() + k * (w + gapTile), b.getY(), w, 52 };
}

void FxPanel::selectSection (int index)
{
    selected = juce::jlimit (0, (int) sections.size() - 1, index);
    for (int i = 0; i < (int) sections.size(); ++i)
        setSectionVisible (*sections[(size_t) i], i == selected);

    const bool eq = selected == 0, comp = selected == 1;
    eqCurve->setVisible (eq);
    for (auto& row : bandRows)
    {
        row.label.setVisible (eq);
        row.slider.setVisible (eq);
    }
    eqPresets.setVisible (eq);
    eqReset.setVisible (eq);
    compView->setVisible (comp);
    resized();
    repaint();
}

void FxPanel::mouseDown (const juce::MouseEvent& e)
{
    for (int k = 0; k < 10; ++k)
    {
        const auto r = tileRect (k);
        if (! r.contains (e.getPosition()))
            continue;
        const int i = chainOrder[k];
        if (e.x < r.getX() + 28 && e.y < r.getY() + 28)   // the light: switches the effect on or off
            sections[(size_t) i]->toggle.triggerClick();
        else
            selectSection (i);
        repaint();
        return;
    }
}

void FxPanel::paint (juce::Graphics& g)
{
    g.fillAll (Theme::panel);

    // the chain strip
    for (int k = 0; k < 10; ++k)
    {
        const int i = chainOrder[k];
        const auto r = tileRect (k).toFloat();
        const bool on = sections[(size_t) i]->toggle.getToggleState(), sel = i == selected;
        g.setColour (sel ? Theme::laneBg.brighter (0.07f) : Theme::laneBg);
        g.fillRoundedRectangle (r, 9.0f);
        if (sel)
        {
            g.setColour (Theme::orange);
            g.drawRoundedRectangle (r.reduced (0.5f), 9.0f, 1.6f);
        }

        const auto led = juce::Rectangle<float> (r.getX() + 10.0f, r.getY() + 10.0f, 10.0f, 10.0f);
        if (on)
        {
            g.setColour (Theme::orange.withAlpha (0.3f));
            g.fillEllipse (led.expanded (3.0f));
            g.setColour (Theme::yellow);
            g.fillEllipse (led);
        }
        else
        {
            g.setColour (Theme::textFaint);
            g.drawEllipse (led.reduced (0.5f), 1.2f);
        }

        g.setColour (on || sel ? Theme::text : Theme::text.withAlpha (0.6f));
        g.setFont (juce::FontOptions (13.0f, sel ? juce::Font::bold : juce::Font::plain));
        g.drawText (tileNames[i], r.toNearestInt().withTrimmedTop (26).reduced (8, 0), juce::Justification::centredLeft, true);

        if (k < 9)   // a small arrow: the sound goes on to the next one
        {
            g.setColour (Theme::textFaint.withAlpha (0.6f));
            const float ax = r.getRight() + 3.0f, ay = r.getY() + 14.0f;
            g.drawLine (ax - 1.5f, ay - 3.0f, ax + 1.5f, ay, 1.0f);
            g.drawLine (ax - 1.5f, ay + 3.0f, ax + 1.5f, ay, 1.0f);
        }
    }

    // the card of the selected effect
    g.setColour (Theme::laneBg);
    g.fillRoundedRectangle (detail.toFloat(), 12.0f);
    g.setColour (Theme::border.withAlpha (0.5f));
    g.drawRoundedRectangle (detail.toFloat().reduced (0.5f), 12.0f, 1.0f);
}

void FxPanel::resized()
{
    const auto b = getLocalBounds().reduced (14, 12);
    auto rest = b.withTrimmedTop (52 + 12);
    auto bottom = rest.removeFromBottom (30);
    monoToggle.setBounds (bottom.removeFromLeft (520));
    rest.removeFromBottom (8);
    detail = rest;

    if (sections.empty())
        return;
    auto& s = *sections[(size_t) selected];
    auto area = detail.reduced (20, 14);

    auto header = area.removeFromTop (32);   // the name with its on/off box, the meters on the right
    for (int m = (int) s.meters.size() - 1; m >= 0; --m)
        s.meters[(size_t) m]->setBounds (header.removeFromRight (s.meters.size() > 1 ? 110 : 220).reduced (2, 7));
    s.toggle.setBounds (header.removeFromLeft (260));
    area.removeFromTop (6);

    // sliders and the rows under them, spread over two columns
    auto layoutItems = [&] (juce::Rectangle<int> r)
    {
        const int total = (int) s.rows.size() + (int) s.footerRows.size();
        const int perColumn = (total + 1) / 2;
        const int colGap = 28, colW = (r.getWidth() - colGap) / 2;
        for (int idx = 0; idx < total; ++idx)
        {
            const int column = idx / perColumn, row = idx % perColumn;
            const juce::Rectangle<int> cell (r.getX() + column * (colW + colGap), r.getY() + row * detailRowHeight, colW, detailRowHeight);
            if (idx < (int) s.rows.size())
            {
                auto& rw = *s.rows[(size_t) idx];
                auto c = cell;
                rw.label.setBounds (c.removeFromLeft (84));
                rw.slider.setBounds (c);
                continue;
            }
            auto& footer = s.footerRows[(size_t) idx - s.rows.size()];
            auto c = cell.reduced (0, 2);
            if (footer.size() == 1)
                footer[0]->setBounds (c);
            else if (dynamic_cast<juce::Label*> (footer[0]) != nullptr)
            {
                footer[0]->setBounds (c.removeFromLeft (84));
                footer[1]->setBounds (c);
            }
            else
            {
                const int w = c.getWidth() / (int) footer.size();
                for (auto* comp : footer)
                    comp->setBounds (c.removeFromLeft (w));
            }
        }
    };

    if (&s == eqSection)   // the curve on the left, the sliders of the selected band and the presets on the right
    {
        auto left = area.removeFromLeft ((int) (area.getWidth() * 0.64f));
        area.removeFromLeft (24);
        eqCurve->setBounds (left);
        for (auto& row : bandRows)
        {
            auto r = area.removeFromTop (detailRowHeight);
            row.label.setBounds (r.removeFromLeft (70));
            row.slider.setBounds (r);
        }
        area.removeFromTop (14);
        auto r = area.removeFromTop (detailRowHeight).reduced (0, 2);
        eqReset.setBounds (r.removeFromRight (64));
        r.removeFromRight (6);
        eqPresets.setBounds (r);
    }
    else if (&s == compSection)   // the sliders on the left (two columns), the curve and the gain reduction on the right
    {
        auto right = area.removeFromRight ((int) (area.getWidth() * 0.34f));
        area.removeFromRight (24);
        compView->setBounds (right);
        layoutItems (area);
    }
    else
        layoutItems (area);
}

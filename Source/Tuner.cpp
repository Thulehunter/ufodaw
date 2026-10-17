#include "Tuner.h"
#include "Theme.h"

float detectPitchHz (const float* x, int n, double sampleRate, float& rms)
{
    double energy = 0.0;
    for (int i = 0; i < n; ++i)
        energy += (double) x[i] * x[i];
    rms = (float) std::sqrt (energy / juce::jmax (1, n));
    if (rms < 0.004f)
        return 0.0f;

    // YIN: difference function over lags up to 40 ms back (25 Hz), the window being half the data.
    const int maxLag = juce::jmin (n / 2, (int) (sampleRate / 25.0));
    const int minLag = juce::jmax (2, (int) (sampleRate / 1500.0));
    const int window = n - maxLag;
    if (window < 256 || maxLag <= minLag + 2)
        return 0.0f;

    std::vector<float> d ((size_t) maxLag + 1, 0.0f);
    for (int tau = 1; tau <= maxLag; ++tau)
    {
        double sum = 0.0;
        for (int j = 0; j < window; ++j)
        {
            const double diff = (double) x[j] - (double) x[j + tau];
            sum += diff * diff;
        }
        d[(size_t) tau] = (float) sum;
    }

    // cumulative mean normalised difference
    std::vector<float> cm ((size_t) maxLag + 1, 1.0f);
    double running = 0.0;
    for (int tau = 1; tau <= maxLag; ++tau)
    {
        running += d[(size_t) tau];
        cm[(size_t) tau] = running > 0.0 ? (float) (d[(size_t) tau] * tau / running) : 1.0f;
    }

    // the first dip below the threshold (then down to its bottom) is the period
    constexpr float threshold = 0.15f;
    int best = -1;
    for (int tau = minLag; tau < maxLag; ++tau)
        if (cm[(size_t) tau] < threshold)
        {
            while (tau + 1 < maxLag && cm[(size_t) tau + 1] < cm[(size_t) tau])
                ++tau;
            best = tau;
            break;
        }
    if (best < 0)
        return 0.0f;

    // parabolic interpolation around the minimum
    float tauF = (float) best;
    if (best > 1 && best < maxLag)
    {
        const float a = cm[(size_t) best - 1], b = cm[(size_t) best], c = cm[(size_t) best + 1];
        const float denom = a - 2.0f * b + c;
        if (std::abs (denom) > 1.0e-9f)
            tauF += 0.5f * (a - c) / denom;
    }
    return (float) (sampleRate / tauF);
}

//==============================================================================
TunerPanel::TunerPanel (Engine& e, const juce::StringArray& inputNames, int firstInput) : engine (e)
{
    for (int i = 0; i < inputNames.size(); ++i)
        inputBox.addItem (inputNames[i], i + 1);
    inputBox.setSelectedId (juce::jlimit (0, juce::jmax (0, inputNames.size() - 1), firstInput) + 1, juce::dontSendNotification);
    inputBox.onChange = [this] { engine.setTunerChannel (inputBox.getSelectedId() - 1); };
    addAndMakeVisible (inputBox);

    auto setRef = [this] (int delta)
    {
        refHz = juce::jlimit (430, 450, refHz + delta);
        repaint();
    };
    refDown.onClick = [setRef] { setRef (-1); };
    refUp.onClick = [setRef] { setRef (1); };
    addAndMakeVisible (refDown);
    addAndMakeVisible (refUp);

    engine.setTunerChannel (juce::jmax (0, inputBox.getSelectedId() - 1));
    startTimerHz (25);
    setSize (420, 250);
}

TunerPanel::~TunerPanel()
{
    stopTimer();
    engine.setTunerChannel (-1);
}

void TunerPanel::resized()
{
    inputBox.setBounds (14, 12, 150, 26);
    refUp.setBounds (getWidth() - 14 - 30, 12, 30, 26);
    refDown.setBounds (getWidth() - 14 - 30 - 4 - 30 - 70, 12, 30, 26);
}

void TunerPanel::timerCallback()
{
    constexpr int n = 4096;
    float buf[n];
    if (! engine.readTunerSamples (buf, n))
        return;

    float rms = 0.0f;
    const float f = detectPitchHz (buf, n, engine.getSampleRate(), rms);
    level = level * 0.7f + rms * 0.3f;

    if (f > 0.0f)
    {
        const float semis = 12.0f * std::log2 (f / (float) refHz) + 69.0f;
        const int note = (int) std::round (semis);
        const float cents = (semis - (float) note) * 100.0f;
        const bool sameNote = note == midiNote && holdFrames > 0;
        hz = f;
        midiNote = note;
        shownCents = sameNote ? shownCents * 0.5f + cents * 0.5f : cents;
        holdFrames = 25;   // one second
    }
    else if (holdFrames > 0)
    {
        --holdFrames;
    }
    else
    {
        midiNote = -1;
        hz = 0.0f;
    }
    repaint();
}

void TunerPanel::paint (juce::Graphics& g)
{
    g.fillAll (Theme::background);

    auto area = getLocalBounds().reduced (14);
    area.removeFromTop (32);

    // A= reference
    g.setColour (Theme::textDim);
    g.setFont (juce::FontOptions (13.0f));
    g.drawText ("A = " + juce::String (refHz) + " Hz", getWidth() - 14 - 30 - 4 - 30 - 72 + 30 + 4, 12, 72, 26, juce::Justification::centred);

    if (midiNote < 0)
    {
        g.setColour (Theme::textFaint);
        g.setFont (juce::FontOptions (18.0f));
        g.drawText ("Play a note...", area, juce::Justification::centred);
        // input level, so you can see the input is alive
        g.setColour (Theme::panelRaised);
        g.fillRoundedRectangle (area.removeFromBottom (6).toFloat(), 3.0f);
        return;
    }

    static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    const auto name = juce::String (names[((midiNote % 12) + 12) % 12]);
    const int octave = midiNote / 12 - 1;
    const bool inTune = std::abs (shownCents) < 5.0f;

    g.setColour (inTune ? Theme::yellow : Theme::orange);
    g.setFont (juce::FontOptions (64.0f, juce::Font::bold));
    auto noteArea = area.removeFromTop (78);
    g.drawText (name, noteArea, juce::Justification::centred);
    g.setColour (Theme::textDim);
    g.setFont (juce::FontOptions (18.0f));
    g.drawText (juce::String (octave), noteArea.withTrimmedLeft (noteArea.getWidth() / 2 + 34).withHeight (30).translated (0, 40), juce::Justification::left);

    // the scale: -50 .. +50 cents
    auto scale = area.removeFromTop (60).reduced (10, 4);
    g.setColour (Theme::panelRaised);
    g.fillRoundedRectangle (scale.toFloat(), 6.0f);
    const float cx = (float) scale.getCentreX();
    const float halfW = (float) scale.getWidth() * 0.5f - 8.0f;
    for (int c = -50; c <= 50; c += 10)
    {
        const float x = cx + halfW * (float) c / 50.0f;
        g.setColour (c == 0 ? Theme::yellow : Theme::textFaint);
        g.drawVerticalLine ((int) x, (float) scale.getY() + (c == 0 ? 6.0f : 14.0f), (float) scale.getBottom() - (c == 0 ? 6.0f : 14.0f));
    }
    const float needleX = cx + halfW * juce::jlimit (-50.0f, 50.0f, shownCents) / 50.0f;
    g.setColour (inTune ? Theme::yellow : Theme::redBright);
    g.fillRoundedRectangle (needleX - 3.0f, (float) scale.getY() + 4.0f, 6.0f, (float) scale.getHeight() - 8.0f, 3.0f);

    g.setColour (Theme::text);
    g.setFont (juce::FontOptions (16.0f));
    const auto cents = juce::String (std::abs (shownCents), 0);
    g.drawText (inTune ? "in tune" : (shownCents < 0 ? "flat  " + cents + " cents" : "sharp  " + cents + " cents"),
                area.removeFromTop (26), juce::Justification::centred);
    g.setColour (Theme::textDim);
    g.setFont (juce::FontOptions (13.0f));
    g.drawText (juce::String (hz, 1) + " Hz", area.removeFromTop (20), juce::Justification::centred);
}

//==============================================================================
TunerWindow::TunerWindow (Engine& e, const juce::StringArray& inputNames, int firstInput, std::function<void()> onClosed)
    : juce::DocumentWindow ("Tuner", Theme::background, juce::DocumentWindow::closeButton), engine (e), closed (std::move (onClosed))
{
    setUsingNativeTitleBar (true);
    setContentOwned (new TunerPanel (e, inputNames, firstInput), true);
    setResizable (false, false);
    setAlwaysOnTop (true);
    centreWithSize (getWidth(), getHeight());
    setVisible (true);
}

TunerWindow::~TunerWindow() = default;

void TunerWindow::closeButtonPressed()
{
    if (closed)
        closed();   // the owner deletes this window
}

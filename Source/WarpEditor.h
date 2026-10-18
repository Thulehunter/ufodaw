#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "Engine.h"
#include "Beat.h"
#include "TempoMap.h"
#include "Theme.h"

// The warp editor of an audio clip: the audio of the clip drawn over the project's beat grid, with warp markers. A marker pins a moment of the
// audio (put it on a hit); drag it to where it should play and the audio between the markers stretches to fit (pitch kept). "Find hits" puts
// markers on the hits, "Match tempo" brings the whole clip to the project's tempo, "Snap to grid" puts every marker on the nearest sixteenth.
// Nothing is processed until Apply (the stretching of long clips takes a few seconds).
class WarpPanel : public juce::Component
{
public:
    WarpPanel (std::shared_ptr<const juce::AudioBuffer<float>> audio, double sampleRate, double regionStartSeconds, double regionLengthSeconds,
               std::vector<WarpMarker> markers, const TempoMap& map, double clipStartSeconds,
               std::function<void (std::vector<WarpMarker>)> onApply, std::function<void()> onClose)
        : source (std::move (audio)), rate (sampleRate), regionStart (regionStartSeconds), regionLength (juce::jmax (0.01, regionLengthSeconds)),
          warps (std::move (markers)), tempoMap (map), clipStart (clipStartSeconds), apply (std::move (onApply)), close (std::move (onClose))
    {
        std::sort (warps.begin(), warps.end(), [] (auto& a, auto& b) { return a.src < b.src; });
        refreshView();

        for (auto* b : { &findButton, &tempoButton, &snapButton, &clearButton, &applyButton, &cancelButton })
            addAndMakeVisible (*b);
        findButton.setTooltip ("Puts a marker on every hit (kick, snare, hat...) of the clip, without changing anything yet");
        tempoButton.setTooltip ("Detects the tempo of the clip and spaces the markers so that it plays at the project's tempo (finds the hits first if needed)");
        snapButton.setTooltip ("Moves every marker to the nearest sixteenth of the project's grid");
        clearButton.setTooltip ("Removes all markers (the clip goes back to its original timing when you Apply)");
        applyButton.setTooltip ("Stretches the audio between the markers (it runs in the background; Ctrl+Z undoes it)");
        findButton.onClick = [this] { findHits(); };
        tempoButton.onClick = [this] { matchTempo(); };
        snapButton.onClick = [this] { snapAll(); };
        clearButton.onClick = [this] { warps.clear(); refreshView(); repaint(); };
        applyButton.onClick = [this] { if (apply) apply (warps); };
        cancelButton.onClick = [this] { if (close) close(); };

        snapToggle.setToggleState (true, juce::dontSendNotification);
        snapToggle.setTooltip ("Markers jump to the grid while you drag them (hold Alt to move freely)");
        addAndMakeVisible (snapToggle);

        hint.setText ("Double-click: add a marker (it sticks to a nearby hit).  Drag a marker to where it should play.  Right-click a marker: remove it.  "
                      "The audio after the last marker keeps its speed.",
                      juce::dontSendNotification);
        hint.setFont (juce::FontOptions (12.0f));
        hint.setColour (juce::Label::textColourId, Theme::textFaint);
        addAndMakeVisible (hint);

        hits = detectOnsets (*source, rate, startSample(), startSample() + regionSamples(), 0.5f);
        setSize (900, 330);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Theme::panel);
        const auto area = waveArea();
        g.setColour (Theme::laneBg);
        g.fillRoundedRectangle (area.toFloat(), 6.0f);
        g.saveState();
        g.reduceClipRegion (area);

        // the grid of the project, bars and beats
        const double total = anchorsEnd();
        tempoMap.forEachGridLine (clipStart, clipStart + viewDuration, 1, [&] (double seconds, int kind, int bar)
        {
            const int x = area.getX() + (int) ((seconds - clipStart) / viewDuration * area.getWidth());
            g.setColour (Theme::text.withAlpha (kind == 2 ? 0.28f : 0.09f));
            g.drawVerticalLine (x, (float) area.getY(), (float) area.getBottom());
            if (kind == 2)
            {
                g.setFont (juce::FontOptions (10.5f));
                g.setColour (Theme::textDim);
                g.drawText (juce::String (bar + 1), x + 3, area.getY() + 1, 40, 12, juce::Justification::left);
            }
        });

        // the audio, stretched as the markers say
        if (source != nullptr)
        {
            if (peaksDirty || (int) peaks.size() != area.getWidth())
                makePeaks (area.getWidth());
            const float mid = (float) area.getCentreY();
            const float half = (float) area.getHeight() * 0.42f;
            g.setColour (Theme::audioWave);
            for (int x = 0; x < (int) peaks.size(); ++x)
                if (peaks[(size_t) x] > 0.0f)
                    g.drawVerticalLine (area.getX() + x, mid - peaks[(size_t) x] * half, mid + peaks[(size_t) x] * half);
        }

        // the hits that were found (small ticks along the top), then the markers
        g.setColour (Theme::redBright.withAlpha (0.55f));
        for (double h : hitSeconds())
        {
            const int x = xOfDest (srcToDest (h));
            g.drawVerticalLine (x, (float) area.getY(), (float) area.getY() + 6.0f);
        }
        for (size_t i = 0; i < warps.size(); ++i)
        {
            const int x = xOfDest (warps[i].dest);
            const bool grabbed = (int) i == dragging;
            g.setColour (grabbed ? Theme::yellow : Theme::orange);
            g.fillRect (x - 1, area.getY(), 2, area.getHeight());
            juce::Path tri;
            tri.addTriangle ((float) x - 6.0f, (float) area.getY(), (float) x + 6.0f, (float) area.getY(), (float) x, (float) area.getY() + 9.0f);
            g.fillPath (tri);
        }
        juce::ignoreUnused (total);
        g.restoreState();

        g.setColour (Theme::textDim);
        g.setFont (juce::FontOptions (12.5f));
        g.drawText (juce::String (warps.size()) + (warps.size() == 1 ? " marker" : " markers") + "   |   clip length "
                        + juce::String (anchorsEnd(), 2) + " s (was " + juce::String (regionLength, 2) + " s)",
                    getLocalBounds().reduced (12, 0).removeFromBottom (30).withTrimmedBottom (4).removeFromLeft (440), juce::Justification::centredLeft);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (12, 10);
        auto top = r.removeFromTop (28);
        findButton.setBounds (top.removeFromLeft (100));
        top.removeFromLeft (6);
        tempoButton.setBounds (top.removeFromLeft (120));
        top.removeFromLeft (6);
        snapButton.setBounds (top.removeFromLeft (130));
        top.removeFromLeft (6);
        clearButton.setBounds (top.removeFromLeft (80));
        top.removeFromLeft (14);
        snapToggle.setBounds (top.removeFromLeft (170));
        r.removeFromTop (6);
        auto bottom = r.removeFromBottom (30);
        applyButton.setBounds (bottom.removeFromRight (100));
        bottom.removeFromRight (8);
        cancelButton.setBounds (bottom.removeFromRight (90));
        hint.setBounds (r.removeFromBottom (34));
        r.removeFromBottom (4);
        wave = r;
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        dragging = markerAt (e.x);
        if (dragging >= 0 && e.mods.isPopupMenu())
        {
            warps.erase (warps.begin() + dragging);
            dragging = -1;
            refreshView();
            repaint();
            return;
        }
        if (dragging >= 0)
            dragDuration = viewDuration;   // (fixed while dragging: the view must not rescale under the mouse)
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragging < 0)
            return;
        const auto area = waveArea();
        double d = (double) (e.x - area.getX()) / (double) juce::jmax (1, area.getWidth()) * dragDuration;
        if (snapToggle.getToggleState() && ! e.mods.isAltDown())
            d = snapDest (d);
        const double lo = dragging > 0 ? warps[(size_t) dragging - 1].dest + 0.01 : 0.01;
        const double hi = dragging + 1 < (int) warps.size() ? warps[(size_t) dragging + 1].dest - 0.01 : 1.0e9;
        warps[(size_t) dragging].dest = juce::jlimit (lo, juce::jmax (lo, hi), d);
        peaksDirty = true;
        repaint();
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (dragging >= 0)
        {
            dragging = -1;
            refreshView();
            repaint();
        }
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu() || markerAt (e.x) >= 0 || ! waveArea().contains (e.getPosition()))
            return;
        const auto area = waveArea();
        const double dest = (double) (e.x - area.getX()) / (double) area.getWidth() * viewDuration;
        double s = destToSrc (dest);
        double bestDistance = 0.04;   // sticks to a hit within 40 ms
        for (double h : hitSeconds())
            if (std::abs (h - s) < bestDistance)
            {
                bestDistance = std::abs (h - s);
                s = h;
            }
        if (s <= 0.004 || s >= regionLength - 0.004)
            return;
        for (auto& w : warps)
            if (std::abs (w.src - s) < 0.004)
                return;
        WarpMarker m;
        m.src = s;
        m.dest = srcToDest (s);   // (where it plays now: nothing moves until you drag it)
        warps.push_back (m);
        std::sort (warps.begin(), warps.end(), [] (auto& a, auto& b) { return a.src < b.src; });
        refreshView();
        repaint();
    }

private:
    int startSample() const { return juce::jlimit (0, source->getNumSamples(), (int) std::llround (regionStart * rate)); }
    int regionSamples() const { return juce::jlimit (0, source->getNumSamples() - startSample(), (int) std::llround (regionLength * rate)); }
    juce::Rectangle<int> waveArea() const { return wave; }

    std::vector<double> hitSeconds() const
    {
        std::vector<double> out;
        for (int h : hits)
            out.push_back ((double) (h - startSample()) / rate);
        return out;
    }

    // (src, dest) pairs: the start, the markers, the end of the region (the rest after the last marker plays at its own speed)
    std::vector<std::pair<double, double>> anchors() const
    {
        std::vector<std::pair<double, double>> a { { 0.0, 0.0 } };
        for (auto& m : warps)
            if (m.src > a.back().first + 0.004 && m.src < regionLength - 0.004)
                a.push_back ({ m.src, juce::jmax (a.back().second + 0.004, m.dest) });
        a.push_back ({ regionLength, a.back().second + (regionLength - a.back().first) });
        return a;
    }

    double anchorsEnd() const { return anchors().back().second; }

    double destToSrc (double d) const
    {
        const auto a = anchors();
        for (size_t i = 0; i + 1 < a.size(); ++i)
            if (d <= a[i + 1].second || i + 2 == a.size())
            {
                const double span = a[i + 1].second - a[i].second;
                return a[i].first + (span > 0.0 ? (d - a[i].second) / span : 0.0) * (a[i + 1].first - a[i].first);
            }
        return d;
    }

    double srcToDest (double s) const
    {
        const auto a = anchors();
        for (size_t i = 0; i + 1 < a.size(); ++i)
            if (s <= a[i + 1].first || i + 2 == a.size())
            {
                const double span = a[i + 1].first - a[i].first;
                return a[i].second + (span > 0.0 ? (s - a[i].first) / span : 0.0) * (a[i + 1].second - a[i].second);
            }
        return s;
    }

    int xOfDest (double d) const { return waveArea().getX() + (int) (d / viewDuration * waveArea().getWidth()); }

    int markerAt (int x) const
    {
        int best = -1, bestDistance = 8;
        for (size_t i = 0; i < warps.size(); ++i)
            if (std::abs (xOfDest (warps[i].dest) - x) < bestDistance)
            {
                bestDistance = std::abs (xOfDest (warps[i].dest) - x);
                best = (int) i;
            }
        return best;
    }

    void refreshView()
    {
        viewDuration = juce::jmax (regionLength, anchorsEnd()) * 1.04;
        peaksDirty = true;
    }

    // the nearest sixteenth of the project's grid, as a time in the clip
    double snapDest (double d) const
    {
        const double beat = tempoMap.secondsToBeats (clipStart + d);
        return tempoMap.beatsToSeconds (std::round (beat * 4.0) / 4.0) - clipStart;
    }

    void makePeaks (int w)
    {
        peaks.assign ((size_t) juce::jmax (1, w), 0.0f);
        peaksDirty = false;
        const int channels = source->getNumChannels();
        const int base = startSample(), n = regionSamples();
        for (int x = 0; x < w; ++x)
        {
            const double d0 = (double) x / w * viewDuration, d1 = (double) (x + 1) / w * viewDuration;
            if (d0 >= anchorsEnd())
                break;
            const double s0 = destToSrc (d0), s1 = destToSrc (juce::jmin (d1, anchorsEnd()));
            const int a = juce::jlimit (0, n, (int) (s0 * rate)), b = juce::jlimit (a + 1, n, (int) (s1 * rate) + 1);
            float m = 0.0f;
            for (int ch = 0; ch < channels; ++ch)
            {
                const float* d = source->getReadPointer (ch) + base;
                const int step = juce::jmax (1, (b - a) / 64);   // (a long stretch of audio per pixel is sampled, not read through)
                for (int i = a; i < b; i += step)
                    m = juce::jmax (m, std::abs (d[i]));
            }
            peaks[(size_t) x] = m;
        }
    }

    void findHits()
    {
        warps.clear();
        for (double h : hitSeconds())
            if (h > 0.01 && h < regionLength - 0.01)
                warps.push_back ({ h, h });
        refreshView();
        repaint();
    }

    // The tempo of the clip against the project's: every marker (the hits, and one at the end so that the last piece follows) moves so that the
    // clip plays at the project's tempo.
    void matchTempo()
    {
        const double detected = detectBpm (*source, rate, startSample(), startSample() + regionSamples());
        if (detected <= 0.0)
        {
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "No clear beat found",
                                                    "The tempo of this clip could not be found. Place the markers by hand.");
            return;
        }
        const double projectBpm = tempoMap.bpmAt (clipStart);
        const double ratio = detected / projectBpm;   // a clip at 158 in a project at 120 gets 1.32 times as long
        if (warps.empty())
            findHits();
        for (auto& w : warps)
            w.dest = w.src * ratio;
        if (warps.empty() || warps.back().src < regionLength - 0.05)
            warps.push_back ({ regionLength - 0.02, (regionLength - 0.02) * ratio });
        refreshView();
        repaint();
    }

    void snapAll()
    {
        double previous = 0.0;
        for (auto& w : warps)
        {
            const double snapped = snapDest (w.dest);
            if (snapped > previous + 0.01)
                w.dest = snapped;
            previous = w.dest;
        }
        refreshView();
        repaint();
    }

    std::shared_ptr<const juce::AudioBuffer<float>> source;
    double rate, regionStart, regionLength;
    std::vector<WarpMarker> warps;
    TempoMap tempoMap;
    double clipStart;
    std::function<void (std::vector<WarpMarker>)> apply;
    std::function<void()> close;
    std::vector<int> hits;   // sample positions of the hits of the region
    juce::Rectangle<int> wave;
    std::vector<float> peaks;
    bool peaksDirty = true;
    double viewDuration = 1.0, dragDuration = 1.0;
    int dragging = -1;

    juce::TextButton findButton { "Find hits" }, tempoButton { "Match tempo" }, snapButton { "Snap to grid" }, clearButton { "Clear" },
                     applyButton { "Apply" }, cancelButton { "Cancel" };
    juce::ToggleButton snapToggle { "Snap while dragging" };
    juce::Label hint;
};

class WarpWindow : public juce::DocumentWindow
{
public:
    WarpWindow (const juce::String& title, WarpPanel* panel, std::function<void()> onClosed)
        : juce::DocumentWindow (title, Theme::background, juce::DocumentWindow::closeButton), closed (std::move (onClosed))
    {
        setUsingNativeTitleBar (true);
        setContentOwned (panel, true);
        setResizable (false, false);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
    }

    void closeButtonPressed() override { if (closed) closed(); }

private:
    std::function<void()> closed;
};

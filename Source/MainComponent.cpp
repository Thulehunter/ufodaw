#include "MainComponent.h"
#include "PianoRoll.h"
#include "DrumSequencer.h"
#include "FxPanel.h"
#include "PluginPanel.h"
#include "RoutingPanel.h"
#include "MidiTools.h"
#include "TempoPanel.h"
#include "SyncPanels.h"
#include "ClipPanel.h"
#include "Stretch.h"
#include "WarpEditor.h"
#include "VersionHistory.h"
#include "SectionsPanel.h"
#include "Commands.h"
#include "MasterMeter.h"
#include "ProjectTemplates.h"
#include "SampleBrowser.h"
#include "UpdateCheck.h"
#include "StarterPack.h"
#include "MidiLearn.h"
#include "AudioExport.h"
#include "SamplerPanel.h"
#include "DrumRackPanel.h"
#include "MixerPanel.h"
#include <algorithm>
#include <map>

//==============================================================================
// Pan slider that jumps in 5 % steps (-100 ... 0 ... +100) when "Pan snap" is on, and moves freely when it is off.
// Holding Alt while dragging gives the opposite of the current setting.
class PanSlider : public juce::Slider
{
public:
    double snapValue (double attempted, DragMode) override
    {
        const bool snap = UiSettings::panSnap != juce::ModifierKeys::currentModifiers.isAltDown();
        if (! snap)
            return attempted;
        return juce::jlimit (-1.0, 1.0, std::round (attempted / UiSettings::panStep) * UiSettings::panStep);
    }

    static juce::String describe (double v)
    {
        const int percent = (int) std::round (std::abs (v) * 100.0);
        return percent == 0 ? juce::String ("Center") : (v < 0 ? "L " : "R ") + juce::String (percent) + "%";
    }
};

//==============================================================================
// Left-hand controls for one track: name, volume, pan, mute, solo, level meter.
class TrackHeader : public juce::Component, public juce::SettableTooltipClient
{
public:
    TrackHeader (Track& t, std::function<void (Track*)> onRemove, std::function<void()> onBeginEdit,
                 std::function<void (Track&, juce::Component&)> onOpenPlugins,
                 std::function<void (Track&, juce::Component&)> onOpenAutomation,
                 std::function<double()> onPosition,
                 std::function<void (Track&)> onOpenEditor,
                 std::function<void (Track&)> onToggleArm,
                 std::function<void (Track&, juce::Component&)> onOpenInput,
                 std::function<void (Track&, juce::Component&)> onOpenRouting,
                 std::function<void (Track&)> onContextMenu,
                 std::function<void (Track&)> onToggleSubLanes,
                 bool masterStrip = false)
        : track (t), remove (std::move (onRemove)), beginEdit (std::move (onBeginEdit)), openPlugins (std::move (onOpenPlugins)),
          openAutomation (std::move (onOpenAutomation)), position (std::move (onPosition)), openEditor (std::move (onOpenEditor)),
          toggleArm (std::move (onToggleArm)), openInput (std::move (onOpenInput)), openRouting (std::move (onOpenRouting)),
          contextMenu (std::move (onContextMenu)), toggleSubLanes (std::move (onToggleSubLanes)), isMaster (masterStrip)
    {
        const bool audioTrack = track.type == Track::Type::Audio && ! track.bus;   // (a bus has no input to record)

        // record-arm: audio tracks only; the next audio recording goes into the armed track
        armButton.setTooltip (track.type == Track::Type::Midi
                                  ? "Record arm: this track plays and records the MIDI of its input (set it in the VST panel; default: every input). "
                                    "Arm several MIDI tracks to record a keyboard and an e-drum at once."
                                  : "Record arm: the next recording goes into this track (click Record at the bottom, or press R)");
        armButton.onClick = [this] { toggleArm (track); };
        addChildComponent (armButton);
        armButton.setVisible (audioTrack || (track.type == Track::Type::Midi && ! isMaster));

        // fourth row: audio tracks pick their input here (and can monitor it); MIDI tracks show what plays them
        rowButton.setTooltip (track.bus ? "A bus: it plays what other tracks send or route to it (the Out button on those tracks)"
                                        : track.type == Track::Type::Audio ? "Which input of the audio interface this track records from (and monitors)"
                                                                            : "Instrument or sequencer plugin, hardware output... (opens the VST panel)");
        rowButton.onClick = [this]
        {
            if (track.bus)                            return;
            if (track.type == Track::Type::Audio) openInput (track, rowButton);
            else if (track.kit != nullptr && track.instrument == nullptr) openEditor (track);
            else                                  openPlugins (track, rowButton);
        };
        rowButton.setEnabled (! track.bus);
        addAndMakeVisible (rowButton);

        routeButton.setTooltip ("Output and sends: play into the master or a bus, and send some of this track to buses (reverb, delay...)");
        routeButton.onClick = [this] { openRouting (track, routeButton); };
        addChildComponent (routeButton);
        routeButton.setVisible (! track.bus);

        // Mono / stereo: lit = this channel plays in mono (left and right summed). Also on buses and the master.
        monoButton.setClickingTogglesState (true);
        monoButton.setColour (juce::TextButton::buttonOnColourId, Theme::yellow);
        monoButton.setColour (juce::TextButton::textColourOnId, Theme::border);
        monoButton.setTooltip ("Mono: sum left and right, after the effects and plugins (lit = mono, dark = stereo). "
                               "Handy to centre a stereo recording, or to check a mix in mono.");
        monoButton.setToggleState (track.fx.mono.load(), juce::dontSendNotification);
        monoButton.onClick = [this] { beginEdit(); track.fx.mono = monoButton.getToggleState(); };
        addAndMakeVisible (monoButton);

        monitorButton.setClickingTogglesState (true);
        monitorButton.setColour (juce::TextButton::buttonOnColourId, Theme::yellow);
        monitorButton.setTooltip ("Monitor: hear this track's input live through its effects and volume while it is armed. "
                                  "Use headphones: a microphone in front of speakers will feed back.");
        monitorButton.onClick = [this] { track.monitor = monitorButton.getToggleState(); };
        addChildComponent (monitorButton);
        monitorButton.setVisible (audioTrack);

        nameLabel.setInterceptsMouseClicks (false, false);   // so double-clicking the name reaches the header (which renames the track)
        nameLabel.setEditable (false, false, true);          // (the editor is opened by that double-click; leaving it keeps what was typed)
        nameLabel.onTextChange = [this]
        {
            auto text = nameLabel.getText().trim();
            if (text.isEmpty() || text == track.name)
            {
                nameLabel.setText (track.name, juce::dontSendNotification);
                return;
            }
            beginEdit();   // (one undo step; the track still has its old name here)
            track.name = text;
        };
        nameLabel.setText (track.name, juce::dontSendNotification);
        nameLabel.setFont (juce::FontOptions (14.0f, juce::Font::bold));
        addAndMakeVisible (nameLabel);

        volume.setSliderStyle (juce::Slider::LinearHorizontal);
        volume.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        volume.setRange (0.0, 1.5, 0.001);
        volume.setValue (track.gain, juce::dontSendNotification);
        volume.onDragStart = [this] { beginEdit(); };
        volume.onValueChange = [this] { track.gain = (float) volume.getValue(); };
        volume.textFromValueFunction = [] (double v)
        {
            return v < 0.001 ? juce::String ("-inf dB") : juce::String (20.0 * std::log10 (v), 1) + " dB";
        };
        volume.setPopupDisplayEnabled (true, true, nullptr);   // value bubble while dragging
        volume.setTooltip ("Volume (double-click: reset)");
        volume.setDoubleClickReturnValue (true, 0.8);
        addAndMakeVisible (volume);

        pan.setSliderStyle (juce::Slider::LinearHorizontal);
        pan.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        pan.setRange (-1.0, 1.0, 0.01);
        pan.setValue (track.pan, juce::dontSendNotification);
        pan.setDoubleClickReturnValue (true, 0.0);
        pan.onDragStart = [this] { beginEdit(); };
        pan.onValueChange = [this] { track.pan = (float) pan.getValue(); };
        pan.textFromValueFunction = [] (double v) { return PanSlider::describe (v); };
        pan.setPopupDisplayEnabled (true, true, nullptr);   // shows "L 35%", "Center", "R 20%" while dragging
        pan.setTooltip ("Pan left / right (double-click: center). Snaps to 5% steps when 'Pan snap' is on; hold Alt for the other mode");
        addAndMakeVisible (pan);

        muteButton.setClickingTogglesState (true);
        muteButton.setToggleState (track.mute, juce::dontSendNotification);
        muteButton.setColour (juce::TextButton::buttonOnColourId, Theme::redBright);
        muteButton.onClick = [this] { beginEdit(); track.mute = muteButton.getToggleState(); };
        addAndMakeVisible (muteButton);

        soloButton.setClickingTogglesState (true);
        soloButton.setToggleState (track.solo, juce::dontSendNotification);
        soloButton.setColour (juce::TextButton::buttonOnColourId, Theme::yellow);
        soloButton.onClick = [this] { beginEdit(); track.solo = soloButton.getToggleState(); };
        addAndMakeVisible (soloButton);

        subToggle.setTooltip ("Show or hide the sub-tracks of this track (a lane per pitch)");
        subToggle.setWantsKeyboardFocus (false);
        subToggle.onClick = [this] { if (toggleSubLanes) toggleSubLanes (track); };
        addChildComponent (subToggle);
        deleteButton.onClick = [this] { remove (&track); };
        addAndMakeVisible (deleteButton);

        fxButton.setTooltip ("Effects: EQ, compressor, delay, reverb");
        fxButton.onClick = [this]
        {
            if (openFxPanel)
                openFxPanel (track, fxButton);
            else
                juce::CallOutBox::launchAsynchronously (std::make_unique<FxPanel> (track, beginEdit), fxButton.getScreenBounds(), nullptr);
        };
        addAndMakeVisible (fxButton);

        vstButton.setTooltip ("VST3 plugins: instrument and effects");
        vstButton.onClick = [this] { openPlugins (track, vstButton); };
        addAndMakeVisible (vstButton);

        autoButton.setTooltip ("Automation of volume and pan");
        autoButton.onClick = [this] { openAutomation (track, autoButton); };
        addAndMakeVisible (autoButton);

        if (isMaster)   // the master channel: no pan, mute, solo, arming or deleting; it is the final stage of the mix
        {
            for (juce::Component* c : std::initializer_list<juce::Component*> { &pan, &muteButton, &soloButton, &deleteButton, &armButton,
                                                                               &rowButton, &monitorButton, &routeButton })
                c->setVisible (false);
            nameLabel.setText ("MASTER", juce::dontSendNotification);
            nameLabel.setColour (juce::Label::textColourId, Theme::yellow);
            volume.setTooltip ("Master volume: everything you hear goes through it (double-click: reset)");
            fxButton.setTooltip ("Effects on the whole mix: EQ, compressor, delay, reverb");
            vstButton.setTooltip ("VST3 effect plugins on the whole mix (a limiter or EQ, for example)");
            autoButton.setTooltip ("Automation of the master volume and its effects");
            meterButton.setTooltip ("The master meter: loudness (LUFS), spectrum and a reference track to compare your mix with");
            meterButton.onClick = [this] { if (openMeter) openMeter(); };
            addAndMakeVisible (meterButton);
        }
    }

    Track& getTrack() { return track; }
    void startRename() { if (! isMaster) nameLabel.showEditor(); }

    // After the track's values were changed from outside (undo, opening a project).
    void syncControls()
    {
        volume.setValue (track.gain, juce::dontSendNotification);
        pan.setValue (track.pan, juce::dontSendNotification);
        muteButton.setToggleState (track.mute, juce::dontSendNotification);
        soloButton.setToggleState (track.solo, juce::dontSendNotification);
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (e.y < 30 && ! isMaster)   // the name row: rename the track (the instrument opens from the button below the sliders)
            nameLabel.showEditor();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Theme::background);
        const auto card = getLocalBounds().toFloat().reduced (5.0f, 3.0f);
        g.setColour (Theme::panel);
        g.fillRoundedRectangle (card, 10.0f);

        // colour stripe by track type: orange = audio, red = MIDI synth, yellow = drums
        g.setColour (isMaster ? Theme::yellow : track.colour != 0 ? juce::Colour (track.colour)
                                                        : track.bus ? Theme::yellowDim
                                                                    : Theme::trackColour (track.type == Track::Type::Midi, track.kit != nullptr));
        g.fillRoundedRectangle (card.getX() + 3.0f, card.getY() + 8.0f, isMaster ? 5.0f : 4.0f, card.getHeight() - 16.0f, 2.0f);

        if (track.groupId != 0)   // a member of a group: a line at the left edge, under the group's header
        {
            g.setColour (Theme::yellowDim);
            g.fillRect (card.getX() - 3.0f, card.getY() - 3.0f, 3.0f, card.getHeight() + 6.0f);
        }

        if (track.frozen.load())
        {
            g.setColour (Theme::yellow.withAlpha (0.8f));   // a yellow frame: this track is switched off, its bounce (the next track) plays
            g.drawRoundedRectangle (card.reduced (1.0f), 10.0f, 2.0f);
        }

        // level meter, with a clip lamp on top: it lights red when the track (or, for an armed audio track, its input) reached full
        // scale and stays lit until you click it, so a clip that happened a moment ago is not missed
        const bool clip = track.clipped.load() || track.inputClipped.load();
        auto lamp = juce::Rectangle<int> (getWidth() - 19, 9, 9, 11);
        auto m = juce::Rectangle<int> (getWidth() - 19, 23, 9, getHeight() - 32);
        g.setColour (juce::Colours::black);
        g.fillRect (m);
        const float lvl = juce::jlimit (0.0f, 1.0f, shown);
        g.setColour (lvl > 0.9f ? Theme::redBright : Theme::yellow);
        g.fillRect (m.withTop (m.getBottom() - (int) (lvl * (float) m.getHeight())));
        g.setColour (Theme::redBright.withAlpha (0.45f));   // the 0 dB mark
        g.drawHorizontalLine (m.getY() + 1, (float) m.getX(), (float) m.getRight());

        if (clip)
        {
            g.setColour (Theme::redBright.withAlpha (0.25f));   // a glow behind the lamp
            g.fillRoundedRectangle (lamp.expanded (3).toFloat(), 4.0f);
            g.setColour (Theme::redBright);
            g.fillRect (lamp);
            g.setColour (Theme::text);
            g.drawRect (lamp, 1);
            g.setColour (Theme::redBright);
            g.drawRoundedRectangle (card, 10.0f, 2.0f);   // a red frame round the whole strip
        }
        else
        {
            g.setColour (Theme::redDim.withAlpha (0.45f));
            g.fillRect (lamp);
        }

    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())   // right-click on the header: the track menu
        {
            if (! isMaster && contextMenu)
                contextMenu (track);
            return;
        }
        if (e.x >= getWidth() - 24 && e.y < 26)   // the clip lamp: click to clear it
        {
            track.clipped = false;
            track.inputClipped = false;
            repaint();
            return;
        }
        dragArmed = ! isMaster && reorder && e.x < getWidth() - 24;   // grab the header (the name, or any gap between the controls) to move the whole track, sub-tracks with it
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (! dragArmed || (! dragging && e.getDistanceFromDragStart() < 6))
            return;
        dragging = true;
        setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
        if (auto* holder = getParentComponent())
            reorder (track, e.getEventRelativeTo (holder).y, false);
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        const bool was = dragging;
        dragging = dragArmed = false;
        setMouseCursor (juce::MouseCursor::NormalCursor);
        if (was)
            if (auto* holder = getParentComponent())
                reorder (track, e.getEventRelativeTo (holder).y, true);   // (this header may be deleted by the call)
    }

    // Set by the main window: drag the name row up or down to move the track (finished = the mouse was released).
    std::function<void (Track&, int yInHolder, bool finished)> reorder;
    std::function<void (Track&, juce::Component&)> openFxPanel;   // the effects panel (the main window gives it the engine, for the sidechain)
    std::function<void()> openMeter;                              // the master's Meter button

    void resized() override
    {
        if (isMaster)   // compact: name and buttons on top, the volume fader below
        {
            auto m = getLocalBounds().withTrimmedRight (20).withTrimmedLeft (6).reduced (8, 7);
            auto row = m.removeFromTop (22);
            fxButton.setBounds (row.removeFromRight (32));
            row.removeFromRight (4);
            vstButton.setBounds (row.removeFromRight (38));
            row.removeFromRight (4);
            autoButton.setBounds (row.removeFromRight (26));
            row.removeFromRight (4);
            meterButton.setBounds (row.removeFromRight (48));
            nameLabel.setBounds (row);
            m.removeFromTop (6);
            auto fader = m.removeFromTop (24);
            monoButton.setBounds (fader.removeFromRight (46));   // the master can be listened to in mono, too
            fader.removeFromRight (6);
            volume.setBounds (fader);
            return;
        }

        auto c = getLocalBounds().withTrimmedRight (20).withTrimmedLeft (6).reduced (8, 6);
        auto top = c.removeFromTop (20);
        deleteButton.setBounds (top.removeFromRight (22));
        top.removeFromRight (4);
        fxButton.setBounds (top.removeFromRight (32));
        top.removeFromRight (4);
        vstButton.setBounds (top.removeFromRight (38));
        top.removeFromRight (4);
        autoButton.setBounds (top.removeFromRight (26));
        if (subToggle.isVisible())   // the + / - of a track that has sub-tracks
        {
            top.removeFromRight (4);
            subToggle.setBounds (top.removeFromRight (24));
        }
        if (armButton.isVisible())
        {
            armButton.setBounds (top.removeFromLeft (22));
            top.removeFromLeft (4);
        }
        nameLabel.setBounds (top);
        c.removeFromTop (4);

        auto bottom = c.removeFromBottom (22);   // input / instrument row, with the output on the right
        c.removeFromBottom (4);
        if (routeButton.isVisible())
        {
            routeButton.setBounds (bottom.removeFromRight (74));
            bottom.removeFromRight (4);
        }
        monoButton.setBounds (bottom.removeFromRight (46));
        bottom.removeFromRight (4);
        if (monitorButton.isVisible())
        {
            monitorButton.setBounds (bottom.removeFromRight (40));
            bottom.removeFromRight (4);
        }
        rowButton.setBounds (bottom);

        auto buttons = c.removeFromRight (56);
        muteButton.setBounds (buttons.removeFromTop (22));
        buttons.removeFromTop (4);
        soloButton.setBounds (buttons.removeFromTop (22));
        volume.setBounds (c.removeFromTop (24));
        pan.setBounds (c.removeFromTop (24));
    }

    void tick()
    {
        const bool wantSubToggle = ! isMaster && track.type == Track::Type::Midi;
        if (wantSubToggle != subToggleShown)
        {
            subToggleShown = wantSubToggle;
            subToggle.setVisible (wantSubToggle);
            resized();
        }
        if (wantSubToggle && subToggle.getButtonText() != (track.subLanesOpen && ! track.subLanes.empty() ? "-" : "+"))
            subToggle.setButtonText (track.subLanesOpen && ! track.subLanes.empty() ? "-" : "+");
        if (! isMaster && ! nameLabel.isBeingEdited() && nameLabel.getText() != track.name)   // (renamed somewhere else, e.g. in the mixer)
            nameLabel.setText (track.name, juce::dontSendNotification);
        const bool armedAudio = track.type == Track::Type::Audio && ! track.bus && track.recordArmed;
        const float l = juce::jmax (track.level.load(), armedAudio ? track.inputLevel.load() : 0.0f);   // an armed track shows its input
        shown = l > shown ? l : shown * 0.85f;

        // the name turns red when the track clipped; otherwise yellow / orange when its plugins and effects use a lot of the audio time
        const bool clipNow = track.clipped.load() || track.inputClipped.load();
        const int cpuPercent = (int) std::lround (track.dspLoad.load() * 100.0f);
        const int cpuLevel = cpuPercent >= 50 ? 2 : cpuPercent >= 25 ? 1 : 0;
        if (clipNow != clipShown || cpuLevel != cpuLevelShown || std::abs (cpuPercent - cpuPercentShown) >= 2)
        {
            clipShown = clipNow;
            cpuLevelShown = cpuLevel;
            cpuPercentShown = cpuPercent;
            nameLabel.setColour (juce::Label::textColourId, clipNow ? Theme::redBright : cpuLevel == 2 ? Theme::orange : cpuLevel == 1 ? Theme::yellow
                                                                                       : (isMaster ? Theme::yellow : Theme::text));
            juce::String tip;
            if (clipNow)
                tip = juce::String (track.inputClipped.load() ? "The input clipped: lower the gain on the interface. " : "This track clipped: lower its volume or what is before it. ")
                      + "Click the red lamp to clear it.\n";
            if (cpuPercent >= 1)
                tip += "CPU: this track needs " + juce::String (cpuPercent) + " % of the audio time (its plugins and effects)"
                       + (cpuLevel > 0 ? ". That is a lot: freeze it, use a lighter plugin, or raise the buffer size under Audio..." : ".");
            setTooltip (tip.trim());
        }

        const bool fxActive = track.fx.anyOn();
        if (fxActive != fxShownActive)
        {
            fxShownActive = fxActive;
            fxButton.setColour (juce::TextButton::buttonColourId, fxActive ? Theme::orangeDim : Theme::buttonOff);
            fxButton.repaint();
        }

        // Automated controls follow the envelope at the playhead and can't be dragged by hand.
        const bool volAuto = track.autoVolume.active && ! track.autoVolume.points.empty();
        const bool panAuto = track.autoPan.active && ! track.autoPan.points.empty();
        volume.setEnabled (! volAuto);
        pan.setEnabled (! panAuto);
        if (volAuto && ! volume.isMouseButtonDown())
            volume.setValue (evalAutomation (track.autoVolume.points, position(), track.gain), juce::dontSendNotification);
        if (panAuto && ! pan.isMouseButtonDown())
            pan.setValue (evalAutomation (track.autoPan.points, position(), track.pan), juce::dontSendNotification);

        bool otherAuto = false;
        for (auto& lane : track.autoLanes)
            otherAuto = otherAuto || (lane->active && ! lane->points.empty());
        const int autoState = track.autoEditId.isNotEmpty() ? 2 : (volAuto || panAuto || otherAuto) ? 1 : 0;
        if (autoState != autoShownState)
        {
            autoShownState = autoState;
            autoButton.setColour (juce::TextButton::buttonColourId, autoState == 2 ? Theme::orange
                                                                  : autoState == 1 ? Theme::orangeDim
                                                                                   : Theme::buttonOff);
            autoButton.repaint();
        }

        // the fourth row: the audio input of an audio track, what plays a MIDI track
        {
            juce::String text;
            if (track.bus)
            {
                text = "Bus: effects return / group";
            }
            else if (track.type == Track::Type::Audio)
            {
                text = "In: " + (track.inputCount > 1 ? juce::String (track.inputFirst + 1) + "+" + juce::String (track.inputFirst + 2)
                                                      : juce::String (track.inputFirst + 1));
            }
            else
            {
                text = track.instrument != nullptr ? track.instrument->description.name
                                                    : track.sampler != nullptr ? "Sampler: " + (track.sampler->getSample() != nullptr ? track.sampler->getName() : juce::String ("no sample"))
                                                    : track.kit != nullptr ? juce::String ("Drum kit") : juce::String ("Built-in synth");
                if (track.sequencer)
                    text = "Sequencer: " + text;
                if (! track.externalOuts.empty())
                    text += "  >  " + track.externalOuts.front().deviceName;
                else if (! track.midiOuts.empty())
                    text += "  >  " + juce::String ((int) track.midiOuts.size()) + " track" + (track.midiOuts.size() > 1 ? "s" : "");
            }
            if (text != rowShown)
            {
                rowShown = text;
                rowButton.setButtonText (text);
            }
            if (track.monitor != monitorShown)
            {
                monitorShown = track.monitor;
                monitorButton.setToggleState (monitorShown, juce::dontSendNotification);
            }
            if (track.fx.mono.load() != monoButton.getToggleState())   // changed elsewhere: the FX panel, undo, loading
                monoButton.setToggleState (track.fx.mono.load(), juce::dontSendNotification);

            if (! track.bus && ! isMaster)   // where this track plays, and how many sends it has
            {
                auto route = juce::String::fromUTF8 ("\xe2\x86\x92 ") + (track.output != nullptr ? track.output->name : juce::String ("Master"));
                if (! track.sends.empty())
                    route += " +" + juce::String ((int) track.sends.size());
                if (route != routeShown)
                {
                    routeShown = route;
                    routeButton.setButtonText (route);
                    routeButton.setColour (juce::TextButton::buttonColourId,
                                           track.output != nullptr || ! track.sends.empty() ? Theme::orangeDim : Theme::buttonOff);
                }
            }
        }

        const bool armedNow = track.type == Track::Type::Midi ? track.midiArmed : track.recordArmed;
        if (armedNow != armShown)
        {
            armShown = armedNow;
            armButton.setColour (juce::TextButton::buttonColourId, armShown ? Theme::redBright : Theme::buttonOff);
            armButton.setColour (juce::TextButton::textColourOffId, armShown ? Theme::text : Theme::redDim);
            armButton.repaint();
        }

        const bool hasPlugins = track.instrument != nullptr || ! track.inserts.empty();
        if (hasPlugins != vstShownActive)
        {
            vstShownActive = hasPlugins;
            vstButton.setColour (juce::TextButton::buttonColourId, hasPlugins ? Theme::red : Theme::buttonOff);
            vstButton.repaint();
        }

        // Only what changed is painted again: the meter strip when the level moved a step, the whole header when the clip state
        // changed (a full repaint of every strip 30 times a second costs a lot of CPU for nothing).
        const int bin = (int) (juce::jlimit (0.0f, 1.0f, shown) * 40.0f);
        if (bin != shownBin)
        {
            shownBin = bin;
            repaint (getWidth() - 26, 0, 26, getHeight());
        }
        if (clipNow != clipPainted)
        {
            clipPainted = clipNow;
            repaint();
        }
    }

private:
    Track& track;
    bool fxShownActive = false;
    bool vstShownActive = false;
    int autoShownState = -1;
    std::function<void (Track*)> remove;
    std::function<void()> beginEdit;
    std::function<void (Track&, juce::Component&)> openPlugins;
    std::function<void (Track&, juce::Component&)> openAutomation;
    std::function<double()> position;
    std::function<void (Track&)> openEditor;
    std::function<void (Track&)> toggleArm;
    std::function<void (Track&, juce::Component&)> openInput;
    std::function<void (Track&, juce::Component&)> openRouting;
    std::function<void (Track&)> contextMenu;   // right-click on the header: rename, duplicate, delete, move...
    std::function<void (Track&)> toggleSubLanes;   // the + / - button: show or hide the sub-tracks
    juce::TextButton subToggle { "+" };
    bool dragArmed = false, dragging = false;
    bool subToggleShown = false;
    juce::TextButton rowButton, monitorButton { "Mon" }, routeButton, monoButton { "Mono" };
    juce::String rowShown, routeShown;
    bool monitorShown = false;
    bool isMaster = false;
    bool armShown = false;
    bool clipShown = false;
    int cpuLevelShown = 0, cpuPercentShown = 0;
    int shownBin = -1;           // the meter as last painted, in 1/40 steps: the strip is only repainted when this changes
    bool clipPainted = false;
    float shown = 0.0f;

    juce::Label nameLabel;
    juce::Slider volume;
    PanSlider pan;
    juce::TextButton muteButton { "M" }, soloButton { "S" }, deleteButton { "x" }, fxButton { "FX" }, vstButton { "VST" }, autoButton { "A" }, armButton { juce::String::fromUTF8 ("\xe2\x97\x8f") };
    juce::TextButton meterButton { "Meter" };   // the master only
};

//==============================================================================
// Bar/beat ruler above the lanes. Click or drag to move the playhead.
class TimeRuler : public juce::Component
{
public:
    explicit TimeRuler (MainComponent& m) : main (m) {}

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Theme::panel);
        const double pps = main.getPixelsPerSecond();
        const double scroll = main.getScrollX();

        auto& engine = main.getEngine();
        const double loopA = engine.loopStart, loopB = engine.loopEnd;
        if (loopB > loopA)
        {
            const int x1 = (int) (loopA * pps - scroll), x2 = (int) (loopB * pps - scroll);
            const bool on = engine.loopEnabled;
            g.setColour ((on ? Theme::orange : Theme::textFaint).withAlpha (on ? 0.35f : 0.22f));
            g.fillRect (x1, 0, x2 - x1, getHeight());
            g.setColour (on ? Theme::orange : Theme::textFaint);
            g.fillRect (x1, getHeight() - 3, x2 - x1, 3);
        }

        const double punchA = engine.punchIn, punchB = engine.punchOut;
        if (punchB > punchA)   // punch region: the top half of the ruler in red
        {
            const int x1 = (int) (punchA * pps - scroll), x2 = (int) (punchB * pps - scroll);
            const bool on = engine.punchEnabled;
            g.setColour ((on ? Theme::redBright : Theme::textFaint).withAlpha (on ? 0.5f : 0.22f));
            g.fillRect (x1, 0, x2 - x1, getHeight() / 2);
            g.setColour (on ? Theme::redBright : Theme::textFaint);
            g.fillRect (x1, 0, x2 - x1, 3);
        }

        // bar and beat lines from the tempo map (bar numbers are thinned out when the bars get narrow)
        const auto& map = main.getTempoMap();
        const double secondsAtLeft = scroll / pps, secondsAtRight = (scroll + getWidth()) / pps;
        const double barPx = map.barSecondsAt (secondsAtLeft) * pps;
        const int labelEvery = juce::jmax (1, (int) std::ceil (60.0 / juce::jmax (1.0, barPx)));
        const double beatPx = map.beatSecondsAt (secondsAtLeft) * pps;

        g.setFont (11.0f);
        map.forEachGridLine (secondsAtLeft, secondsAtRight, 1, [&] (double sec, int kind, int bar)
        {
            const int x = (int) (sec * pps - scroll);
            if (kind == 2)
            {
                g.setColour (Theme::text.withAlpha (0.4f));
                g.drawVerticalLine (x, (float) getHeight() * 0.45f, (float) getHeight());
                if (bar % labelEvery == 0)
                {
                    g.setColour (Theme::text.withAlpha (0.75f));
                    g.drawText (juce::String (bar + 1), x + 3, 1, 50, 14, juce::Justification::left);
                }
            }
            else if (beatPx >= 14.0)
            {
                g.setColour (Theme::text.withAlpha (0.2f));
                g.drawVerticalLine (x, (float) getHeight() * 0.7f, (float) getHeight());
            }
        });

        // the sections of the arrangement (verse, chorus...) as bands along the top
        {
            const auto& arrangement = main.getEngine().getSections();
            for (size_t i = 0; i < arrangement.size(); ++i)
            {
                const int x0 = (int) (map.barStartSeconds (arrangement[i].startBar) * pps - scroll);
                const int x1 = (int) (map.barStartSeconds (arrangement[i].startBar + arrangement[i].bars) * pps - scroll);
                if (x1 < 0 || x0 > getWidth())
                    continue;
                const auto colour = juce::Colour (arrangement[i].colour != 0 ? arrangement[i].colour : SectionsPanel::palette()[i % SectionsPanel::palette().size()]);
                g.setColour (colour.withAlpha (0.8f));
                g.fillRoundedRectangle ((float) x0, 1.0f, (float) juce::jmax (2, x1 - x0 - 1), 13.0f, 3.0f);
                g.setColour (juce::Colours::black.withAlpha (0.8f));
                g.setFont (juce::FontOptions (10.5f, juce::Font::bold));
                g.drawText (juce::String (arrangement[i].name), x0 + 4, 1, juce::jmax (0, x1 - x0 - 6), 13, juce::Justification::left, true);
            }
        }

        // where the tempo or the time signature changes
        const auto& sections = map.getSections();
        for (size_t i = 0; i < sections.size(); ++i)
        {
            const int x = (int) (map.barStartSeconds (sections[i].bar) * pps - scroll);
            if (x < -200 || x > getWidth())
                continue;
            const auto name = juce::String (sections[i].name);
            if (i == 0 && name.isEmpty())
                continue;   // (the start of the song is only marked when it has a name)
            juce::String text = i > 0 ? juce::String ((int) std::round (sections[i].bpm)) : juce::String();
            if (i > 0 && (sections[i].numerator != sections[i - 1].numerator || sections[i].denominator != sections[i - 1].denominator))
                text += "  " + juce::String (sections[i].numerator) + "/" + juce::String (sections[i].denominator);
            if (name.isNotEmpty())
                text = name + (text.isNotEmpty() ? "  " + text : juce::String());
            g.setColour (Theme::yellow);
            if (i > 0)
                g.fillRect (x, 0, 2, getHeight());
            g.setFont (juce::FontOptions (10.5f, juce::Font::bold));
            g.drawText (text, x + 4, getHeight() - 14, 190, 13, juce::Justification::left);
        }
        g.setColour (Theme::border);
        g.drawHorizontalLine (getHeight() - 1, 0.0f, (float) getWidth());
    }

    // left button: playhead; right button: drag out the loop region (with Shift: the punch region)
    void mouseDown (const juce::MouseEvent& e) override
    {
        loopDragging = e.mods.isPopupMenu();
        punchDragging = loopDragging && e.mods.isShiftDown();
        if (loopDragging)
            loopAnchor = main.snapTime (timeAt (e.x));
        else
            move (e);
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (loopDragging)
        {
            const double t = main.snapTime (timeAt (e.x));
            if (punchDragging)
                main.setPunchRegion (juce::jmin (loopAnchor, t), juce::jmax (loopAnchor, t));
            else
                main.setLoopRegion (juce::jmin (loopAnchor, t), juce::jmax (loopAnchor, t));
        }
        else
        {
            move (e);
        }
    }

    void mouseUp (const juce::MouseEvent&) override { loopDragging = punchDragging = false; }

private:
    double timeAt (int x) const { return juce::jmax (0.0, (x + main.getScrollX()) / main.getPixelsPerSecond()); }
    void move (const juce::MouseEvent& e) { main.setPlayhead (timeAt (e.x)); }

    bool loopDragging = false, punchDragging = false;
    double loopAnchor = 0.0;

    MainComponent& main;
};

//==============================================================================
// One track's timeline row: draws and edits its clips.
class TrackLane : public juce::Component, public juce::FileDragAndDropTarget, public juce::SettableTooltipClient
{
public:
    TrackLane (MainComponent& m, Track& t) : main (m), track (t) {}

    // an empty audio track says whether it is armed, so the lane is redrawn when that changes (from anywhere)
    void checkArmed()
    {
        if (track.recordArmed != armedShown)
        {
            armedShown = track.recordArmed;
            repaint();
        }
    }

    Track& getTrack() { return track; }

    // while this track records, the lane is redrawn as the waveform grows
    void tickLive()
    {
        double start = 0.0, perPeak = 0.01;
        const bool live = main.liveRecordingFor (track, start, perPeak) != nullptr;
        if (live || liveShown)
            repaint();
        liveShown = live;
    }

    void paint (juce::Graphics& g) override
    {
        const double pps = main.getPixelsPerSecond();
        const auto area = g.getClipBounds();

        g.fillAll (Theme::background);
        g.setColour (Theme::laneBg);
        g.fillRoundedRectangle (juce::Rectangle<float> (0.0f, 2.0f, (float) getWidth(), (float) getHeight() - 4.0f), 8.0f);

        // grid
        const auto& map = main.getTempoMap();
        const double leftSeconds = area.getX() / pps, rightSeconds = area.getRight() / pps;
        const double beatPx = map.beatSecondsAt (leftSeconds) * pps;
        map.forEachGridLine (leftSeconds, rightSeconds, 1, [&] (double sec, int kind, int)
        {
            const int x = (int) (sec * pps);
            if (kind == 2)
            {
                g.setColour (Theme::text.withAlpha (0.10f));
                g.drawVerticalLine (x, 3.0f, (float) getHeight() - 3.0f);
            }
            else if (beatPx >= 14.0)
            {
                g.setColour (Theme::text.withAlpha (0.04f));
                g.drawVerticalLine (x, 3.0f, (float) getHeight() - 3.0f);
            }
        });

        // clips
        if (track.audioCompView)
            paintAudioComp (g, pps);
        const auto clips = main.getEngine().getClips (track);
        for (auto it = cache.begin(); it != cache.end();)
            it = std::any_of (clips.begin(), clips.end(), [&] (auto& c) { return c.id == it->first; }) ? std::next (it) : cache.erase (it);

        for (auto& c : clips)
        {
            if (track.audioCompView || ! c.audible())   // (the comp view draws the takes itself)
                continue;
            const auto r = clipRect (c, pps);
            if (! r.intersects (area))
                continue;

            const bool sel = main.isSelected (&track, c.id);
            g.setColour (track.colour != 0 ? (sel ? juce::Colour (track.colour).brighter (0.25f) : juce::Colour (track.colour).darker (0.35f))
                                           : (sel ? Theme::audioClipSel : Theme::audioClip));
            g.fillRoundedRectangle (r.toFloat(), 4.0f);

            const auto& peaks = peaksFor (c, pps);
            const float mid = (float) r.getCentreY();
            const float half = (float) r.getHeight() * 0.5f - 3.0f;
            g.setColour (Theme::audioWave);
            for (int x = juce::jmax (r.getX(), area.getX()); x < juce::jmin (r.getRight(), area.getRight()); ++x)
            {
                const int idx = c.reverse ? r.getWidth() - 1 - (x - r.getX()) : x - r.getX();   // a reversed clip shows its wave backwards
                if (idx >= 0 && idx < (int) peaks.size())
                {
                    const float p = juce::jmin (1.0f, peaks[(size_t) idx] * c.gain);   // (the clip gain is part of what is drawn)
                    g.drawVerticalLine (x, mid - p * half, mid + p * half);
                }
            }

            // the volume curve: a line over the clip (dots too while it is being edited)
            const bool editingCurve = main.getEnvelopeClip() == c.id && main.getEnvelopeTrack() == &track;
            if (! c.envelope.empty() || editingCurve)
            {
                juce::Path curve;
                curve.startNewSubPath ((float) r.getX(), envelopeY (r, evalAutomation (c.envelope, 0.0, 1.0f)));
                for (auto& p : c.envelope)
                    curve.lineTo ((float) r.getX() + (float) (p.time * pps), envelopeY (r, p.value));
                curve.lineTo ((float) r.getRight(), envelopeY (r, evalAutomation (c.envelope, c.length, 1.0f)));
                g.setColour (Theme::yellow.withAlpha (editingCurve ? 1.0f : 0.75f));
                g.strokePath (curve, juce::PathStrokeType (editingCurve ? 2.0f : 1.5f));
                if (editingCurve)
                {
                    for (auto& p : c.envelope)
                        g.fillEllipse ((float) r.getX() + (float) (p.time * pps) - 4.0f, envelopeY (r, p.value) - 4.0f, 8.0f, 8.0f);
                    g.setColour (Theme::text.withAlpha (0.7f));
                    g.setFont (11.0f);
                    g.drawText ("Volume curve: click = add, drag = move, right-click = remove, Esc = done", r.getX() + 24, r.getBottom() - 15, 440, 13,
                                juce::Justification::left);
                }
            }

            // fades: the part that is faded away is darkened, with the fade curve as a line
            const int fiPx = juce::jmin (r.getWidth(), (int) (c.fadeIn * pps)), foPx = juce::jmin (r.getWidth(), (int) (c.fadeOut * pps));
            if (fiPx > 1)
            {
                juce::Path p;
                p.addTriangle ((float) r.getX(), (float) r.getY(), (float) (r.getX() + fiPx), (float) r.getY(), (float) r.getX(), (float) r.getBottom());
                g.setColour (juce::Colours::black.withAlpha (0.4f));
                g.fillPath (p);
                g.setColour (Theme::text.withAlpha (0.8f));
                g.drawLine ((float) r.getX(), (float) r.getBottom(), (float) (r.getX() + fiPx), (float) r.getY(), 1.5f);
            }
            if (foPx > 1)
            {
                juce::Path p;
                p.addTriangle ((float) r.getRight(), (float) r.getY(), (float) (r.getRight() - foPx), (float) r.getY(), (float) r.getRight(), (float) r.getBottom());
                g.setColour (juce::Colours::black.withAlpha (0.4f));
                g.fillPath (p);
                g.setColour (Theme::text.withAlpha (0.8f));
                g.drawLine ((float) r.getRight(), (float) r.getBottom(), (float) (r.getRight() - foPx), (float) r.getY(), 1.5f);
            }

            g.setColour (sel ? Theme::text : Theme::text.withAlpha (0.35f));
            g.drawRoundedRectangle (r.toFloat(), 4.0f, sel ? 2.0f : 1.0f);

            if (r.getWidth() > 30)   // the handles to drag the fades
            {
                g.setColour (sel ? Theme::yellow : Theme::text.withAlpha (0.45f));
                g.fillRoundedRectangle (fadeInHandle (r, c, pps).toFloat(), 2.0f);
                g.fillRoundedRectangle (fadeOutHandle (r, c, pps).toFloat(), 2.0f);
            }

            g.saveState();
            g.reduceClipRegion (r.reduced (2));
            g.setColour (Theme::text.withAlpha (0.85f));
            g.setFont (11.0f);
            auto label = c.sourceFile.getFileNameWithoutExtension();
            if (c.takeGroup != 0)
            {
                int count = 0;
                for (auto& other : clips)
                    count += other.takeGroup == c.takeGroup ? 1 : 0;
                label = "Take " + juce::String (c.takeIndex) + " of " + juce::String (count) + "  (right-click to choose)";
            }
            g.drawText (label, r.getX() + 5, r.getY() + 2, 400, 13, juce::Justification::left);
            g.restoreState();
        }

        // the audio that is being recorded into this track: its waveform grows while you play
        {
            double liveStart = 0.0, perPeak = 0.01;
            if (auto live = main.liveRecordingFor (track, liveStart, perPeak))
            {
                const int count = live->count.load (std::memory_order_acquire);
                const auto r = rectFor (liveStart, juce::jmax (0.05, count * perPeak), pps);
                if (r.intersects (area))
                {
                    g.setColour (Theme::redBright.withAlpha (0.28f));
                    g.fillRoundedRectangle (r.toFloat(), 4.0f);
                    const float mid = (float) r.getCentreY();
                    const float half = (float) r.getHeight() * 0.5f - 3.0f;
                    g.setColour (Theme::redBright);
                    for (int x = juce::jmax (r.getX(), area.getX()); x < juce::jmin (r.getRight(), area.getRight()); ++x)
                    {
                        const int from = (int) ((double) (x - r.getX()) / pps / perPeak);
                        const int to = juce::jmin (count, juce::jmax (from + 1, (int) ((double) (x + 1 - r.getX()) / pps / perPeak)));
                        float p = 0.0f;
                        for (int k = juce::jmax (0, from); k < to; ++k)
                            p = juce::jmax (p, live->data[(size_t) k]);
                        p = juce::jmin (1.0f, p);
                        g.drawVerticalLine (x, mid - p * half, mid + p * half + 1.0f);
                    }
                    g.setColour (Theme::redBright);
                    g.drawRoundedRectangle (r.toFloat(), 4.0f, 1.5f);
                    g.setColour (Theme::text.withAlpha (0.85f));
                    g.setFont (11.0f);
                    g.drawText ("Recording...", r.getX() + 5, r.getY() + 2, 120, 13, juce::Justification::left);
                }
            }
        }

        // MIDI clips, with a miniature of their notes
        for (auto& m : main.getEngine().getMidiClips (track))
        {
            const auto r = rectFor (m.start, m.length, pps);
            if (! r.intersects (area))
                continue;

            const bool sel = main.isSelected (&track, m.id);
            const bool drums = track.kit != nullptr;   // drum clips are yellow, synth/MIDI clips red
            g.setColour (track.colour != 0 ? (sel ? juce::Colour (track.colour).brighter (0.25f) : juce::Colour (track.colour).darker (0.35f))
                                           : (sel ? (drums ? Theme::drumClipSel : Theme::midiClipSel) : (drums ? Theme::drumClip : Theme::midiClip)));
            g.fillRoundedRectangle (r.toFloat(), 4.0f);

            int lo = 127, hi = 0;
            for (auto& n : m.notes)
            {
                lo = juce::jmin (lo, n.pitch);
                hi = juce::jmax (hi, n.pitch);
            }
            if (m.notes.empty()) { lo = 60; hi = 71; }
            const int span = juce::jmax (12, hi - lo + 1);
            const float rowPx = (float) (r.getHeight() - 18) / (float) span;

            g.saveState();
            g.reduceClipRegion (r.reduced (1));
            g.setColour (Theme::midiNote);
            for (auto& n : m.notes)
            {
                const float nx = (float) r.getX() + (float) (n.start * pps);
                const float nw = juce::jmax (2.0f, (float) (n.length * pps));
                const float ny = (float) r.getY() + 15.0f + (float) (hi - n.pitch) * rowPx;
                g.fillRect (nx, ny, nw, juce::jmax (2.0f, rowPx - 1.0f));
            }
            g.setColour (Theme::text.withAlpha (0.85f));
            g.setFont (11.0f);
            g.drawText ("MIDI", r.getX() + 5, r.getY() + 2, 100, 13, juce::Justification::left);
            g.restoreState();

            g.setColour (sel ? Theme::text : Theme::text.withAlpha (0.35f));
            g.drawRoundedRectangle (r.toFloat(), 4.0f, sel ? 2.0f : 1.0f);
        }

        // the master channel's lane: a hint at the start of the (usually empty) strip
        if (main.getEngine().isMaster (track) && track.autoEditId.isEmpty())
        {
            g.setColour (Theme::textFaint);
            g.setFont (juce::FontOptions (13.0f));
            g.drawText ("Master channel - the whole mix goes through here. A = automate its volume or effects, VST = plugins on the mix.",
                        14, 0, juce::jmax (200, getWidth() - 20), getHeight(), juce::Justification::centredLeft);
        }

        if (track.bus && track.autoEditId.isEmpty())
        {
            g.setColour (Theme::textFaint);
            g.setFont (juce::FontOptions (13.0f));
            g.drawText ("Bus: plays what other tracks send or route here (the \"Out\" button on a track). Put effects on it with FX or VST.",
                        14, 0, juce::jmax (200, getWidth() - 20), getHeight(), juce::Justification::centredLeft);
        }

        // an audio track without clips says what can be done with it
        if (track.type == Track::Type::Audio && ! track.bus && clips.empty() && track.autoEditId.isEmpty() && ! main.getEngine().isMaster (track) && ! main.getEngine().isRecording())
        {
            g.setColour (track.recordArmed ? Theme::redBright : Theme::textFaint);
            g.setFont (juce::FontOptions (13.0f));
            g.drawText (track.recordArmed ? "Armed (red dot): press Record at the bottom to record into this track, or drop audio files here."
                                          : "Empty audio track: drop audio files here, or click the dot next to the name and record.",
                        14, 0, juce::jmax (200, getWidth() - 20), getHeight(), juce::Justification::centredLeft);
        }

        paintAutomation (g, pps);

        g.setColour (Theme::border);
        g.drawHorizontalLine (getHeight() - 1, 0.0f, (float) getWidth());
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        main.grabKeyboardFocus();
        if (track.audioCompView)
        {
            audioCompMouseDown (e);
            return;
        }
        if (track.autoEditId.isNotEmpty())
        {
            autoMouseDown (e);
            return;
        }

        const double pps = main.getPixelsPerSecond();
        const double t = e.x / pps;

        dragId = -1;
        resizing = false;
        rightPending = rightDragging = false;
        fadeDrag = 0;

        // the fade handles at the top corners of an audio clip
        if (! e.mods.isPopupMenu())
            for (auto& c : main.getEngine().getClips (track))
            {
                if (! c.audible())
                    continue;
                const auto r = clipRect (c, pps);
                if (r.getWidth() <= 30)
                    continue;
                const bool inHandle = fadeInHandle (r, c, pps).expanded (2).contains (e.getPosition());
                const bool outHandle = fadeOutHandle (r, c, pps).expanded (2).contains (e.getPosition());
                if (inHandle || outHandle)
                {
                    fadeDrag = inHandle ? 1 : 2;
                    fadeClipId = c.id;
                    edited = false;
                    main.selectClip (&track, c.id);
                    return;
                }
            }

        // the volume curve of a clip that is being edited: click adds a point, drag moves it, right-click removes it
        if (main.getEnvelopeClip() >= 0 && main.getEnvelopeTrack() == &track)
        {
            Clip c;
            if (main.getEngine().getClip (track, main.getEnvelopeClip(), c) && clipRect (c, pps).contains (e.getPosition()))
            {
                envDrag = -1;
                edited = false;
                const auto r = clipRect (c, pps);
                int nearest = -1;
                float bestDistance = 10.0f;
                for (int i = 0; i < (int) c.envelope.size(); ++i)
                {
                    const float dx = (float) (r.getX() + (int) (c.envelope[(size_t) i].time * pps) - e.x);
                    const float dy = envelopeY (r, c.envelope[(size_t) i].value) - (float) e.y;
                    const float d = std::sqrt (dx * dx + dy * dy);
                    if (d < bestDistance)
                    {
                        bestDistance = d;
                        nearest = i;
                    }
                }

                auto points = c.envelope;
                if (e.mods.isPopupMenu() && nearest >= 0)   // right-click on a point removes it (elsewhere it opens the clip menu as usual)
                {
                    main.beginEdit();
                    points.erase (points.begin() + nearest);
                    main.getEngine().setClipEnvelope (track, c.id, std::move (points));
                    repaint();
                    return;
                }

                if (! e.mods.isPopupMenu() && nearest < 0)   // a new point where the mouse is
                {
                    main.beginEdit();
                    edited = true;
                    const AutoPoint p { juce::jlimit (0.0, c.length, (double) (e.x - r.getX()) / pps), envelopeValue (r, e.y) };
                    points.push_back (p);
                    std::stable_sort (points.begin(), points.end(), [] (auto& a, auto& b) { return a.time < b.time; });
                    for (int i = 0; i < (int) points.size(); ++i)
                        if (points[(size_t) i].time == p.time && points[(size_t) i].value == p.value)
                            nearest = i;
                    main.getEngine().setClipEnvelope (track, c.id, std::move (points));
                    repaint();
                }
                envDrag = nearest;
                envClipId = c.id;
                return;
            }
        }

        // Right button: a click opens the clip menu (on release), a drag sets the loop region (Shift: the punch region).
        if (e.mods.isPopupMenu())
        {
            rightPending = true;
            rightClipId = -1;
            rightTime = t;
            for (auto& h : hits())
                if (rectFor (h.start, h.length, pps).contains (e.getPosition()))
                {
                    rightClipId = h.id;
                    break;
                }
            main.selectClip (rightClipId >= 0 ? &track : nullptr, rightClipId);
            return;
        }

        for (auto& h : hits())   // topmost first
        {
            const auto r = rectFor (h.start, h.length, pps);
            if (! r.contains (e.getPosition()))
                continue;

            dragId = h.id;
            dragTrack = &track;
            dragStart = h.start;
            dragLength = h.length;
            edited = false;
            grabOffset = t - h.start;
            resizing = h.midi && r.getWidth() > 16 && e.x >= r.getRight() - 6;   // right edge of a MIDI clip
            if (resizing && ! main.getEngine().getMidiClip (track, dragId, resizeOriginal))
                resizing = false;
            main.selectClip (&track, dragId);
            return;
        }

        main.selectClip (nullptr, -1);
        main.setPlayhead (t);
    }

    // the right edge of a MIDI / drum clip: the pointer says it can be dragged, the tooltip says what the modifiers do
    void mouseMove (const juce::MouseEvent& e) override
    {
        bool edge = false;
        if (track.autoEditId.isEmpty())
        {
            const double pps = main.getPixelsPerSecond();
            for (auto& h : hits())
            {
                const auto r = rectFor (h.start, h.length, pps);
                if (h.midi && r.getWidth() > 16 && r.contains (e.getPosition()) && e.x >= r.getRight() - 6)
                    edge = true;
            }
        }
        setTooltip (edge ? "Drag the edge: a longer clip repeats the pattern, a shorter one cuts it off.  Shift+drag: stretch the pattern (half as long = twice as fast).  Alt: no snapping."
                         : juce::String());
        setMouseCursor (edge ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::NormalCursor);
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (track.type != Track::Type::Midi || track.autoEditId.isNotEmpty())
            return;

        const double pps = main.getPixelsPerSecond();
        for (auto& h : hits())
            if (rectFor (h.start, h.length, pps).contains (e.getPosition()))
            {
                if (h.midi && track.instrument != nullptr)   // double-click on a clip: open the instrument's own window
                    main.openInstrumentEditor (track);
                return;
            }

        // empty spot on a MIDI track: new two-bar clip at the bar under the cursor
        const auto& map = main.getTempoMap();
        const int barIndex = map.barAtSeconds (e.x / pps);
        const double start = map.barStartSeconds (barIndex);
        main.beginEdit();
        const int id = main.getEngine().addMidiClip (track, start, map.barStartSeconds (barIndex + 2) - start);
        if (id >= 0)
        {
            main.selectClip (&track, id);
            main.clipsChanged();
        }
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (track.audioCompView)
        {
            audioCompMouseDrag (e);
            return;
        }
        if (track.autoEditId.isNotEmpty())
        {
            autoMouseDrag (e);
            return;
        }

        const double t = juce::jmax (0.0, e.x / main.getPixelsPerSecond());

        if (envDrag >= 0)   // dragging a point of the volume curve
        {
            Clip c;
            if (! main.getEngine().getClip (track, envClipId, c) || envDrag >= (int) c.envelope.size())
                return;
            const auto r = clipRect (c, main.getPixelsPerSecond());
            auto points = c.envelope;
            AutoPoint p { juce::jlimit (0.0, c.length, (double) (e.x - r.getX()) / main.getPixelsPerSecond()), envelopeValue (r, e.y) };
            points[(size_t) envDrag] = p;
            std::stable_sort (points.begin(), points.end(), [] (auto& a, auto& b) { return a.time < b.time; });
            for (int i = 0; i < (int) points.size(); ++i)
                if (points[(size_t) i].time == p.time && points[(size_t) i].value == p.value)
                    envDrag = i;
            if (! edited)
            {
                main.beginEdit();
                edited = true;
            }
            main.getEngine().setClipEnvelope (track, envClipId, std::move (points));
            repaint();
            return;
        }

        if (fadeDrag != 0)
        {
            Clip c;
            if (! main.getEngine().getClip (track, fadeClipId, c))
                return;
            double fi = c.fadeIn, fo = c.fadeOut;
            if (fadeDrag == 1)
                fi = juce::jlimit (0.0, juce::jmax (0.0, c.length - fo), t - c.start);
            else
                fo = juce::jlimit (0.0, juce::jmax (0.0, c.length - fi), c.end() - t);
            if (std::abs (fi - c.fadeIn) < 1.0e-6 && std::abs (fo - c.fadeOut) < 1.0e-6)
                return;
            if (! edited)
            {
                main.beginEdit();
                edited = true;
            }
            main.getEngine().setClipFades (track, fadeClipId, fi, fo);
            repaint();
            return;
        }

        if (rightPending)
        {
            if (! rightDragging && e.getDistanceFromDragStart() < 5)
                return;   // still a click
            rightDragging = true;
            const double a = main.snapTime (rightTime), b = main.snapTime (t);
            if (e.mods.isShiftDown())
                main.setPunchRegion (juce::jmin (a, b), juce::jmax (a, b));
            else
                main.setLoopRegion (juce::jmin (a, b), juce::jmax (a, b));
            return;
        }

        if (dragId < 0)
        {
            main.setPlayhead (t);   // scrub
            return;
        }

        if (resizing)
        {
            const double end = e.mods.isAltDown() ? t : main.snapTime (t);
            const double newLength = juce::jmax (main.getTempoMap().beatSecondsAt (dragStart), end - dragStart);
            if (! edited)
            {
                if (std::abs (newLength - dragLength) < 1.0e-6)
                    return;
                main.beginEdit();
                edited = true;
            }
            // Dragging the right edge of a MIDI clip: longer repeats the pattern to fill the clip, shorter cuts it off;
            // Shift stretches the pattern instead (half as long = twice as fast); Alt switches snapping off.
            const bool stretching = e.mods.isShiftDown();
            const double original = juce::jmax (0.001, resizeOriginal.length);
            std::vector<Note> notes;
            std::vector<CcEvent> ccs;
            if (stretching)
            {
                const double f = newLength / original;
                for (auto n : resizeOriginal.notes)
                {
                    n.start *= f;
                    n.length *= f;
                    notes.push_back (n);
                }
                for (auto c : resizeOriginal.ccs)
                {
                    c.time *= f;
                    ccs.push_back (c);
                }
            }
            else
            {
                for (int k = 0; (double) k * original < newLength - 1.0e-6; ++k)
                {
                    const double offset = (double) k * original;
                    for (auto n : resizeOriginal.notes)
                    {
                        n.start += offset;
                        if (n.start >= newLength - 1.0e-6)
                            continue;
                        n.length = juce::jmin (n.length, newLength - n.start);
                        notes.push_back (n);
                    }
                    for (auto c : resizeOriginal.ccs)
                    {
                        c.time += offset;
                        if (c.time < newLength)
                            ccs.push_back (c);
                    }
                }
            }
            main.getEngine().setMidiNotes (*dragTrack, dragId, std::move (notes));
            main.getEngine().setMidiCcs (*dragTrack, dragId, std::move (ccs));
            main.getEngine().setMidiClipLength (*dragTrack, dragId, newLength);
            main.clipsChanged();
            return;
        }

        // dragging up or down carries the clip to another track of the same kind
        if (auto* target = main.trackAtY (e.getEventRelativeTo (&main.getLaneHolder()).y);
            target != nullptr && target != dragTrack && target->type == dragTrack->type)
        {
            if (! edited)
            {
                main.beginEdit();
                edited = true;
            }
            if (main.getEngine().moveClipToTrack (*dragTrack, *target, dragId))
            {
                dragTrack = target;
                main.selectClip (dragTrack, dragId);
            }
        }

        double newStart = t - grabOffset;
        if (! e.mods.isAltDown())
            newStart = main.snapTime (newStart);
        if (! edited)
        {
            if (std::abs (newStart - dragStart) < 1.0e-6)
                return;
            main.beginEdit();
            edited = true;
        }
        main.getEngine().moveClip (*dragTrack, dragId, newStart);
        main.clipsChanged();
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (rightPending && ! rightDragging)
            main.showClipMenu (track, rightClipId, rightTime, *this);
        rightPending = rightDragging = false;
        fadeDrag = 0;
        envDrag = -1;
        dragId = -1;
        autoDrag = -1;
        rangeDragging = false;
        if (track.audioCompView)
            audioCompMouseUp();
        else
            compMouseUp();
    }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        if (e.mods.isCtrlDown())
            main.zoomBy (w.deltaY > 0 ? 1.15 : 1.0 / 1.15, e.getEventRelativeTo (&main.getLaneViewport()).x);
        else
            Component::mouseWheelMove (e, w);
    }

    bool isInterestedInFileDrag (const juce::StringArray&) override
    {
        return ! main.getEngine().isMaster (track) && ! track.bus;   // (a file dropped on a sampler track becomes its sample)
    }
    void filesDropped (const juce::StringArray& files, int x, int) override
    {
        main.importFiles (files, &track, juce::jmax (0.0, main.snapTime (x / main.getPixelsPerSecond())));
    }

private:
    struct Peaks
    {
        double pps = 0.0, offset = -1.0, length = -1.0;
        const void* audio = nullptr;   // which buffer the peaks were made from (a stretched / shifted clip gets a new one)
        std::vector<float> values;
    };

    //==========================================================================
    // Automation envelope editing (when the track's "A" menu picked a parameter to edit)

    static constexpr int autoMargin = 10;

    float valueToY (float v, const AutoInfo& info) const
    {
        const float span = juce::jmax (1.0e-9f, info.hi - info.lo);
        return (float) autoMargin + (1.0f - (v - info.lo) / span) * (float) (getHeight() - 2 * autoMargin);
    }

    float yToValue (int y, const AutoInfo& info) const
    {
        const float f = 1.0f - ((float) y - (float) autoMargin) / (float) (getHeight() - 2 * autoMargin);
        return juce::jlimit (info.lo, info.hi, info.lo + f * (info.hi - info.lo));
    }

    // Range and name of the envelope being edited; looked up again only when the choice changes.
    const AutoInfo& editInfo()
    {
        if (track.autoEditId != cachedInfoId)
        {
            cachedInfoId = track.autoEditId;
            cachedInfo = main.getEngine().getAutomationInfo (track, cachedInfoId);
        }
        return cachedInfo;
    }

    void paintAutomation (juce::Graphics& g, double pps)
    {
        const auto editing = track.autoEditId;
        auto& engine = main.getEngine();

        if (editing.isNotEmpty())
        {
            g.setColour (juce::Colours::black.withAlpha (0.45f));   // dim the clips: they can't be edited right now
            g.fillRect (getLocalBounds());
        }

        auto ids = engine.getAutomationIds (track);   // envelopes that have points
        if (editing.isNotEmpty() && std::find (ids.begin(), ids.end(), editing) == ids.end())
            ids.push_back (editing);

        for (auto& id : ids)
        {
            const bool full = id == editing;
            if (! full && ! engine.isAutomationActive (track, id))
                continue;

            const auto info = engine.getAutomationInfo (track, id);
            if (! info.valid)
                continue;

            const auto pts = engine.getAutomation (track, id);
            if (full)
            {
                const auto data = engine.getAutomationData (track, id);
                if (track.autoCompView)
                    paintCompView (g, info, pps, data, pts);
                else
                    drawTakes (g, info, pps, data);
                if (rangeA >= 0.0 && std::abs (rangeB - rangeA) > 0.0)
                {
                    g.setColour (Theme::text.withAlpha (0.18f));
                    g.fillRect (juce::Rectangle<float> ((float) (juce::jmin (rangeA, rangeB) * pps), 0.0f, (float) (std::abs (rangeB - rangeA) * pps), (float) getHeight()));
                }
                AutoRecMode armedMode;
                if (engine.isAutomationArmed (track, id, &armedMode))
                {
                    const bool rec = engine.isAutomationRecording (track, id);
                    g.setColour (rec ? Theme::redBright : Theme::redBright.withAlpha (0.75f));
                    g.fillEllipse (8.0f, (float) getHeight() - 20.0f, 10.0f, 10.0f);
                    g.setFont (12.0f);
                    g.drawText (juce::String (rec ? "REC " : "armed: ") + (armedMode == AutoRecMode::Touch ? "Touch" : armedMode == AutoRecMode::Latch ? "Latch" : "Write"),
                                22, getHeight() - 22, 200, 14, juce::Justification::left);
                }
            }
            if (full && track.autoCompView)
                continue;   // (the comp view draws the curves itself)
            const auto colour = id == "volume" ? Theme::orange : id == "pan" ? Theme::yellow
                                                                                        : Theme::redBright;
            g.setColour (colour.withAlpha (full ? 1.0f : 0.5f));

            if (pts.empty())
            {
                // nothing yet: show where the parameter currently sits and how to start
                const float y = valueToY (info.current, info);
                const float dashes[] = { 5.0f, 4.0f };
                g.drawDashedLine (juce::Line<float> (0.0f, y, (float) getWidth(), y), dashes, 2, 1.0f);
                g.setFont (12.0f);
                g.drawText (info.name + " automation: click to add points, right-click a point to remove it",
                            8, 4, 700, 14, juce::Justification::left);
                continue;
            }

            juce::Path path;
            path.startNewSubPath (0.0f, valueToY (pts.front().value, info));
            for (auto& p : pts)
                path.lineTo ((float) (p.time * pps), valueToY (p.value, info));
            path.lineTo ((float) getWidth(), valueToY (pts.back().value, info));
            g.strokePath (path, juce::PathStrokeType (full ? 2.0f : 1.0f));

            if (full)
            {
                g.setFont (12.0f);
                g.drawText (info.name, 8, 4, 700, 14, juce::Justification::left);
                for (int i = 0; i < (int) pts.size(); ++i)
                {
                    const float x = (float) (pts[(size_t) i].time * pps), y = valueToY (pts[(size_t) i].value, info);
                    g.setColour (i == autoDrag ? Theme::text : colour);
                    g.fillEllipse (x - 4.5f, y - 4.5f, 9.0f, 9.0f);
                    g.setColour (juce::Colours::black);
                    g.drawEllipse (x - 4.5f, y - 4.5f, 9.0f, 9.0f, 1.0f);
                }
            }
        }
    }

    // ---- comp view: every take in a row of its own; swipe across a row to play that take there

    juce::Rectangle<int> compRowRect (int row, int numTakes) const
    {
        const int resultH = 34;
        const int rowH = numTakes > 0 ? juce::jmin (30, (getHeight() - resultH - 6) / numTakes) : 0;
        if (row < 0)
            return { 0, 3, getWidth(), resultH - 3 };
        return { 0, resultH + 2 + row * rowH, getWidth(), rowH };
    }

    int compRowAt (int y, int numTakes) const
    {
        for (int i = 0; i < numTakes; ++i)
            if (compRowRect (i, numTakes).contains (juce::Point<int> (1, y)))
                return i;
        return -2;   // not on a take row
    }

    static float valueToYInRow (float v, const AutoInfo& info, const juce::Rectangle<int>& r)
    {
        const float span = juce::jmax (1.0e-9f, info.hi - info.lo);
        return (float) r.getY() + 3.0f + (1.0f - (v - info.lo) / span) * (float) (r.getHeight() - 6);
    }

    void paintCompView (juce::Graphics& g, const AutoInfo& info, double pps, const AutoData& data, const std::vector<AutoPoint>& result)
    {
        const int n = (int) data.takes.size();

        // the result (what plays): the flattened envelope, with a strip in each take's colour above where that take plays
        auto resultRow = compRowRect (-1, n);
        g.setColour (Theme::background.withAlpha (0.55f));
        g.fillRect (resultRow);
        for (auto& seg : data.comp)
        {
            int idx = 0, i = 0;
            for (auto& tk : data.takes) { if (tk.id == seg.takeId) idx = i; ++i; }
            g.setColour (takeColour (idx).withAlpha (0.8f));
            g.fillRect (juce::Rectangle<float> ((float) (seg.start * pps), (float) resultRow.getY(), (float) ((seg.end - seg.start) * pps), 3.0f));
        }
        {
            juce::Path p;
            bool first = true;
            for (auto& pt : result)
            {
                const float x = (float) (pt.time * pps), y = valueToYInRow (pt.value, info, resultRow);
                if (first) { p.startNewSubPath (x, y); first = false; } else p.lineTo (x, y);
            }
            if (! result.empty())
                p.lineTo ((float) getWidth(), valueToYInRow (result.back().value, info, resultRow));
            g.setColour (Theme::orange);
            g.strokePath (p, juce::PathStrokeType (2.0f));
            g.setFont (11.0f);
            g.setColour (Theme::text.withAlpha (0.85f));
            g.drawText ("Result", 6, resultRow.getY() + 4, 80, 14, juce::Justification::left);
        }

        // one row per take
        int i = 0;
        for (auto& tk : data.takes)
        {
            const auto row = compRowRect (i, n);
            const auto colour = takeColour (i);
            g.setColour (Theme::background.withAlpha (i % 2 == 0 ? 0.35f : 0.2f));
            g.fillRect (row);

            for (auto& seg : data.comp)   // where this take plays now: a bright band
                if (seg.takeId == tk.id)
                {
                    g.setColour (colour.withAlpha (0.32f));
                    g.fillRect (juce::Rectangle<float> ((float) (seg.start * pps), (float) row.getY(), (float) ((seg.end - seg.start) * pps), (float) row.getHeight()));
                }

            // the take as a block: its range, with a handle at each end (drag to trim; Alt+drag the body to move)
            {
                const auto block = juce::Rectangle<float> ((float) (tk.start * pps), (float) row.getY() + 1.0f, (float) ((tk.end - tk.start) * pps), (float) row.getHeight() - 2.0f);
                g.setColour (colour.withAlpha (0.10f));
                g.fillRoundedRectangle (block, 3.0f);
                g.setColour (colour.withAlpha (0.65f));
                g.drawRoundedRectangle (block, 3.0f, 1.0f);
                g.setColour (colour.withAlpha (0.9f));
                g.fillRect (block.getX(), block.getY() + 2.0f, 3.0f, block.getHeight() - 4.0f);
                g.fillRect (block.getRight() - 3.0f, block.getY() + 2.0f, 3.0f, block.getHeight() - 4.0f);
            }

            juce::Path p;
            bool first = true;
            for (auto& pt : tk.points)
            {
                const float x = (float) (pt.time * pps), y = valueToYInRow (pt.value, info, row);
                if (first) { p.startNewSubPath (x, y); first = false; } else p.lineTo (x, y);
            }
            g.setColour (colour.withAlpha (0.9f));
            g.strokePath (p, juce::PathStrokeType (1.5f));

            g.setColour (colour);
            g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
            g.drawText (tk.name, 6, row.getY() + 1, 90, 13, juce::Justification::left);
            g.setColour (Theme::gridLineStrong);
            g.drawHorizontalLine (row.getBottom() - 1, 0.0f, (float) getWidth());

            if (compDragging && compRow == i && compA != compB)   // the swipe in progress
            {
                g.setColour (Theme::text.withAlpha (0.3f));
                g.fillRect (juce::Rectangle<float> ((float) (juce::jmin (compA, compB) * pps), (float) row.getY(),
                                                    (float) (std::abs (compB - compA) * pps), (float) row.getHeight()));
            }
            ++i;
        }

        if (data.takes.empty())
        {
            g.setColour (Theme::textDim);
            g.setFont (12.0f);
            g.drawText ("No takes yet: arm Touch / Latch / Write in the A menu and ride the control. Each pass gets a row here.",
                        8, 44, getWidth() - 16, 16, juce::Justification::left);
        }
        else
        {
            g.setColour (Theme::textFaint);
            g.setFont (10.5f);
            g.drawText ("Swipe a row = play that take there.  Drag a block's edge = trim.  Alt+drag a block = move.  Right-click = duplicate / delete.",
                        8, getHeight() - 14, juce::jmin (getWidth() - 16, 640), 12, juce::Justification::left);
        }
    }

    // Takes as blocks: drag a block's edge to trim it, Alt+drag its body to move it (both are recomputed from how it was when the
    // drag began, so nothing is lost on the way); a plain drag swipes (comping).
    enum class BlockMode { None, TrimStart, TrimEnd, Move };
    BlockMode blockMode = BlockMode::None;
    int blockTakeId = -1;
    double blockGrab = 0.0;
    bool blockEdited = false;
    AutoData blockOriginal;

    void compMouseDown (const juce::MouseEvent& e)
    {
        if (e.mods.isPopupMenu())
        {
            showTakeMenu (e);
            return;
        }
        const auto data = main.getEngine().getAutomationData (track, track.autoEditId);
        compRow = compRowAt (e.y, (int) data.takes.size());
        if (compRow < 0)
            return;

        const double pps = main.getPixelsPerSecond();
        const auto& tk = data.takes[(size_t) compRow];
        blockMode = BlockMode::None;
        if (std::abs (e.x - tk.start * pps) <= 6.0)        blockMode = BlockMode::TrimStart;
        else if (std::abs (e.x - tk.end * pps) <= 6.0)     blockMode = BlockMode::TrimEnd;
        else if (e.mods.isAltDown() && e.x >= tk.start * pps && e.x <= tk.end * pps)   blockMode = BlockMode::Move;
        if (blockMode != BlockMode::None)
        {
            blockTakeId = tk.id;
            blockGrab = e.x / pps;
            blockEdited = false;
            blockOriginal = data;
            repaint();
            return;
        }
        compDragging = true;
        compA = compB = autoTime (e);
        repaint();
    }

    void compMouseDrag (const juce::MouseEvent& e)
    {
        if (blockMode != BlockMode::None)
        {
            auto& engine = main.getEngine();
            const double pps = main.getPixelsPerSecond();
            const double now = e.mods.isAltDown() && blockMode != BlockMode::Move ? e.x / pps : main.snapTime (e.x / pps);
            const AutoTake* orig = nullptr;
            for (auto& tk : blockOriginal.takes)
                if (tk.id == blockTakeId)
                    orig = &tk;
            if (orig == nullptr)
                return;
            if (! blockEdited)
            {
                if (std::abs (e.x / pps - blockGrab) < 0.01)
                    return;   // still a click
                main.beginEdit();
                blockEdited = true;
            }
            engine.setAutomationData (track, track.autoEditId, blockOriginal);   // (always from the start of the drag)
            if (blockMode == BlockMode::Move)
                engine.moveAutomationTake (track, track.autoEditId, blockTakeId, main.snapTime (e.x / pps) - main.snapTime (blockGrab));
            else if (blockMode == BlockMode::TrimStart)
                engine.trimAutomationTake (track, track.autoEditId, blockTakeId, juce::jmin (now, orig->end - 0.05), orig->end);
            else
                engine.trimAutomationTake (track, track.autoEditId, blockTakeId, orig->start, juce::jmax (now, orig->start + 0.05));
            main.clipsChanged();
            repaint();
            return;
        }
        if (! compDragging)
            return;
        compB = autoTime (e);
        repaint();
    }

    void compMouseUp()
    {
        if (blockMode != BlockMode::None)
        {
            blockMode = BlockMode::None;
            repaint();
            return;
        }
        if (! compDragging)
            return;
        compDragging = false;
        const double a = juce::jmin (compA, compB), b = juce::jmax (compA, compB);
        const auto data = main.getEngine().getAutomationData (track, track.autoEditId);
        if (b - a > 0.01 && compRow >= 0 && compRow < (int) data.takes.size())
        {
            main.beginEdit();
            main.getEngine().useAutomationTake (track, track.autoEditId, data.takes[(size_t) compRow].id, a, b);
            main.clipsChanged();
        }
        repaint();
    }

    // ---- comp view of a loop recording's takes: a row per take; swipe across a row to play that take there

    int audioCompRows() const
    {
        int rows = 0;
        for (auto& c : main.getEngine().getClips (track))
            if (c.takeGroup != 0 && c.end() > track.compFrom - 0.01 && c.start < track.compTo + 0.01)
                rows = juce::jmax (rows, c.takeIndex);
        return rows;
    }

    juce::Rectangle<int> audioCompRowRect (int takeIndex, int rows) const
    {
        const int rowH = rows > 0 ? (getHeight() - 6) / rows : 0;
        return { 0, 3 + (takeIndex - 1) * rowH, getWidth(), rowH };
    }

    void paintAudioComp (juce::Graphics& g, double pps)
    {
        const auto clips = main.getEngine().getClips (track);
        const int rows = audioCompRows();
        for (int k = 1; k <= rows; ++k)
        {
            const auto row = audioCompRowRect (k, rows);
            g.setColour (Theme::background.withAlpha (k % 2 == 0 ? 0.35f : 0.2f));
            g.fillRect (row);
            const auto colour = takeColour (k - 1);

            for (auto& c : clips)
            {
                if (c.takeGroup == 0 || c.takeIndex != k || c.end() < track.compFrom - 0.01 || c.start > track.compTo + 0.01)
                    continue;
                const juce::Rectangle<int> r ((int) std::round (c.start * pps), row.getY() + 1, juce::jmax (2, (int) std::round (c.length * pps)), row.getHeight() - 2);
                g.setColour (c.takeActive ? colour.withAlpha (0.85f) : Theme::audioClip.withAlpha (0.3f));
                g.fillRoundedRectangle (r.toFloat(), 3.0f);

                const auto& peaks = peaksFor (c, pps);
                const float mid = (float) r.getCentreY(), half = (float) r.getHeight() * 0.5f - 2.0f;
                g.setColour (c.takeActive ? Theme::background.withAlpha (0.8f) : Theme::audioWave.withAlpha (0.55f));
                for (int x = juce::jmax (r.getX(), g.getClipBounds().getX()); x < juce::jmin (r.getRight(), g.getClipBounds().getRight()); ++x)
                {
                    const int idx = c.reverse ? r.getWidth() - 1 - (x - r.getX()) : x - r.getX();
                    if (idx >= 0 && idx < (int) peaks.size())
                    {
                        const float p = juce::jmin (1.0f, peaks[(size_t) idx] * c.gain);
                        g.drawVerticalLine (x, mid - p * half, mid + p * half);
                    }
                }
                if (c.takeActive)
                {
                    g.setColour (colour);
                    g.drawRoundedRectangle (r.toFloat(), 3.0f, 1.5f);
                }
            }

            g.setColour (colour);
            g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
            g.drawText ("Take " + juce::String (k), (int) (track.compFrom * pps) + 6, row.getY() + 2, 70, 13, juce::Justification::left);
            g.setColour (Theme::gridLineStrong);
            g.drawHorizontalLine (row.getBottom() - 1, 0.0f, (float) getWidth());

            if (compDragging && compRow == k && compA != compB)   // the swipe in progress
            {
                g.setColour (Theme::text.withAlpha (0.3f));
                g.fillRect (juce::Rectangle<float> ((float) (juce::jmin (compA, compB) * pps), (float) row.getY(),
                                                    (float) (std::abs (compB - compA) * pps), (float) row.getHeight()));
            }
        }
        g.setColour (Theme::textDim);
        g.setFont (11.0f);
        g.drawText ("Comp: swipe across a take to play it there. Right-click to leave.", 8, getHeight() - 15, 420, 13, juce::Justification::left);
    }

    void audioCompMouseDown (const juce::MouseEvent& e)
    {
        if (e.mods.isPopupMenu())
        {
            juce::PopupMenu m;
            m.addItem (1, "Leave comp view");
            m.showMenuAsync ({}, [this] (int r)
            {
                if (r == 1)
                {
                    track.audioCompView = false;
                    main.clipsChanged();
                    repaint();
                }
            });
            return;
        }
        const int rows = audioCompRows();
        compRow = -2;
        for (int k = 1; k <= rows; ++k)
            if (audioCompRowRect (k, rows).contains (juce::Point<int> (1, e.y)))
                compRow = k;
        if (compRow < 0)
            return;
        compDragging = true;
        compA = compB = autoTime (e);
        repaint();
    }

    void audioCompMouseDrag (const juce::MouseEvent& e)
    {
        if (! compDragging)
            return;
        compB = autoTime (e);
        repaint();
    }

    void audioCompMouseUp()
    {
        if (! compDragging)
            return;
        compDragging = false;
        const double a = juce::jmin (compA, compB), b = juce::jmax (compA, compB);
        if (b - a > 0.02 && compRow >= 1)
        {
            main.beginEdit();
            if (main.getEngine().compTakes (track, compRow, a, b))
                main.clipsChanged();
            else
                main.cancelEdit();
        }
        repaint();
    }

    // ---- takes and comping (an automation lane that is being edited)

    static juce::Colour takeColour (int index)
    {
        static const juce::Colour colours[] = { juce::Colour (0xfff28c28), juce::Colour (0xffffc933), juce::Colour (0xffff5a47), juce::Colour (0xffe86a9a), juce::Colour (0xff9ad06a) };
        return colours[(size_t) index % 5];
    }

    void drawTakes (juce::Graphics& g, const AutoInfo& info, double pps, const AutoData& data)
    {
        // where each take plays (the comp), then every take's own curve in its colour
        for (auto& seg : data.comp)
        {
            int index = 0, i = 0;
            juce::String name;
            for (auto& tk : data.takes)
            {
                if (tk.id == seg.takeId) { index = i; name = tk.name; }
                ++i;
            }
            const auto r = juce::Rectangle<float> ((float) (seg.start * pps), 1.0f, (float) ((seg.end - seg.start) * pps), (float) getHeight() - 2.0f);
            g.setColour (takeColour (index).withAlpha (0.14f));
            g.fillRect (r);
            g.setColour (takeColour (index).withAlpha (0.7f));
            g.fillRect (r.withHeight (3.0f));
            g.setFont (11.0f);
            g.drawText (name, r.toNearestInt().withTrimmedLeft (4).withY (4).withHeight (14), juce::Justification::centredLeft, true);
        }

        int i = 0;
        for (auto& tk : data.takes)
        {
            juce::Path p;
            bool first = true;
            for (auto& pt : tk.points)
            {
                const float x = (float) (pt.time * pps), y = valueToY (pt.value, info);
                if (first) { p.startNewSubPath (x, y); first = false; }
                else       p.lineTo (x, y);
            }
            g.setColour (takeColour (i).withAlpha (0.55f));
            g.strokePath (p, juce::PathStrokeType (1.2f));
            ++i;
        }
    }

    void showTakeMenu (const juce::MouseEvent& e)
    {
        auto& engine = main.getEngine();
        const auto id = track.autoEditId;
        const auto data = engine.getAutomationData (track, id);
        const double clickTime = juce::jmax (0.0, e.x / main.getPixelsPerSecond());

        double a = juce::jmin (rangeA, rangeB), b = juce::jmax (rangeA, rangeB);
        const bool haveRange = rangeA >= 0.0 && b - a > 0.01;
        if (! haveRange)
            for (auto& seg : data.comp)
                if (clickTime >= seg.start && clickTime <= seg.end)
                {
                    a = seg.start;
                    b = seg.end;
                }

        juce::PopupMenu menu;
        menu.addSectionHeader (data.takes.empty() ? "No takes yet: arm Touch / Latch / Write in the A menu and ride a control"
                                                  : (haveRange ? "Take for the selected range" : "Take"));
        auto actions = std::make_shared<std::vector<std::function<void()>>>();
        auto add = [&] (juce::PopupMenu& m, const juce::String& text, bool enabled, bool ticked, std::function<void()> action)
        {
            actions->push_back (std::move (action));
            m.addItem ((int) actions->size(), text, enabled, ticked);
        };
        const auto track_ = &track;
        int index = 0;
        for (auto& tk : data.takes)
        {
            const int takeId = tk.id;
            const double from = (a < b) ? a : tk.start, to = (a < b) ? b : tk.end;
            juce::PopupMenu sub;
            add (sub, (a < b) ? "Play here" : "Play where it was recorded", true, false, [this, track_, id, takeId, from, to]
            {
                main.beginEdit();
                main.getEngine().useAutomationTake (*track_, id, takeId, from, to);
            });
            add (sub, "Duplicate take (after it)", true, false, [this, track_, id, takeId, tk]
            {
                main.beginEdit();
                main.getEngine().duplicateAutomationTake (*track_, id, takeId, tk.end - tk.start);
            });
            add (sub, "Delete take", true, false, [this, track_, id, takeId]
            {
                main.beginEdit();
                main.getEngine().deleteAutomationTake (*track_, id, takeId);
            });
            menu.addSubMenu (tk.name + juce::String::formatted ("  (%.1f - %.1f s)", tk.start, tk.end), sub);
            ++index;
        }

        menu.addSeparator();
        add (menu, "Comp view (a row per take)", true, track_->autoCompView, [track_] { track_->autoCompView = ! track_->autoCompView; });
        juce::PopupMenu xf;
        for (double ms : { 0.0, 10.0, 30.0, 100.0, 300.0 })
            add (xf, ms == 0.0 ? juce::String ("Hard cut") : juce::String ((int) ms) + " ms", true, std::abs (data.crossfade * 1000.0 - ms) < 0.5, [this, track_, id, ms]
            {
                main.beginEdit();
                auto d = main.getEngine().getAutomationData (*track_, id);
                d.crossfade = ms / 1000.0;
                main.getEngine().setAutomationData (*track_, id, std::move (d));
            });
        menu.addSubMenu ("Fade between takes", xf);
        if (haveRange)
            add (menu, "Clear range selection", true, false, [this] { rangeA = rangeB = -1.0; });

        menu.showMenuAsync ({}, [this, actions] (int result)
        {
            if (result <= 0 || result > (int) actions->size())
                return;
            (*actions)[(size_t) result - 1]();
            main.clipsChanged();
            repaint();
        });
    }

    int autoHit (const std::vector<AutoPoint>& pts, juce::Point<int> p, const AutoInfo& info) const
    {
        const double pps = main.getPixelsPerSecond();
        for (int i = 0; i < (int) pts.size(); ++i)
            if (std::abs (p.x - pts[(size_t) i].time * pps) <= 7.0 && std::abs ((float) p.y - valueToY (pts[(size_t) i].value, info)) <= 7.0f)
                return i;
        return -1;
    }

    // Pan envelope points follow the same 5 % snapping as the pan slider (Alt flips it).
    float autoValue (const juce::MouseEvent& e, const AutoInfo& info) const
    {
        float v = yToValue (e.y, info);
        if (track.autoEditId == "pan" && UiSettings::panSnap != e.mods.isAltDown())
            v = juce::jlimit (-1.0f, 1.0f, (float) (std::round ((double) v / UiSettings::panStep) * UiSettings::panStep));
        return v;
    }

    double autoTime (const juce::MouseEvent& e) const
    {
        const double t = juce::jmax (0.0, e.x / main.getPixelsPerSecond());
        return e.mods.isAltDown() ? t : main.snapTime (t);
    }

    void autoMouseDown (const juce::MouseEvent& e)
    {
        if (track.autoCompView)
        {
            compMouseDown (e);
            return;
        }
        auto& engine = main.getEngine();
        const auto param = track.autoEditId;
        const auto info = editInfo();
        autoEdited = false;
        autoDrag = -1;
        if (! info.valid)
            return;
        autoPts = engine.getAutomation (track, param);
        const int hit = autoHit (autoPts, e.getPosition(), info);

        if (e.mods.isShiftDown() && ! e.mods.isPopupMenu())   // Shift+drag picks a time range, to hand to a take
        {
            rangeDragging = true;
            rangeA = rangeB = autoTime (e);
            repaint();
            return;
        }

        if (e.mods.isPopupMenu())   // right click removes a point
        {
            if (hit >= 0)
            {
                main.beginEdit();
                autoPts.erase (autoPts.begin() + hit);
                engine.setAutomation (track, param, autoPts);
                main.clipsChanged();
            }
            else
                showTakeMenu (e);   // not on a point: the takes of this envelope
            return;
        }

        if (hit >= 0)
        {
            autoDrag = hit;
            repaint();
            return;
        }

        // empty spot: new point, which can be dragged straight away
        main.beginEdit();
        autoEdited = true;
        const AutoPoint np { autoTime (e), autoValue (e, info) };
        autoPts.push_back (np);
        std::stable_sort (autoPts.begin(), autoPts.end(), [] (const AutoPoint& a, const AutoPoint& b) { return a.time < b.time; });
        for (int i = 0; i < (int) autoPts.size(); ++i)
            if (autoPts[(size_t) i].time == np.time && autoPts[(size_t) i].value == np.value)
                autoDrag = i;
        engine.setAutomation (track, param, autoPts);
        main.clipsChanged();
    }

    void autoMouseDrag (const juce::MouseEvent& e)
    {
        if (track.autoCompView)
        {
            compMouseDrag (e);
            return;
        }
        if (rangeDragging)
        {
            rangeB = autoTime (e);
            repaint();
            return;
        }
        if (autoDrag < 0 || autoDrag >= (int) autoPts.size())
            return;

        const size_t i = (size_t) autoDrag;
        const double minT = i > 0 ? autoPts[i - 1].time + 0.001 : 0.0;
        const double maxT = i + 1 < autoPts.size() ? autoPts[i + 1].time - 0.001 : 1.0e9;
        const double t = juce::jlimit (minT, juce::jmax (minT, maxT), autoTime (e));
        const float v = autoValue (e, editInfo());

        if (! autoEdited)
        {
            if (std::abs (t - autoPts[i].time) < 1.0e-9 && std::abs (v - autoPts[i].value) < 1.0e-6f)
                return;
            main.beginEdit();
            autoEdited = true;
        }
        autoPts[i].time = t;
        autoPts[i].value = v;
        main.getEngine().setAutomation (track, track.autoEditId, autoPts);
        main.clipsChanged();
    }

    static juce::Rectangle<int> rectFor (double start, double length, double pps)
    {
        return { (int) std::round (start * pps), 3, juce::jmax (2, (int) std::round (length * pps)), MainComponent::stripHeight - 7 };
    }

    static juce::Rectangle<int> clipRect (const Clip& c, double pps) { return rectFor (c.start, c.length, pps); }

    // the little squares at the top corners of an audio clip that set its fade-in and fade-out
    static juce::Rectangle<int> fadeInHandle (const juce::Rectangle<int>& r, const Clip& c, double pps)
    {
        return { r.getX() + juce::jmin (r.getWidth() - 12, (int) (c.fadeIn * pps)), r.getY(), 10, 10 };
    }
    static juce::Rectangle<int> fadeOutHandle (const juce::Rectangle<int>& r, const Clip& c, double pps)
    {
        return { r.getRight() - 10 - juce::jmin (r.getWidth() - 12, (int) (c.fadeOut * pps)), r.getY(), 10, 10 };
    }

    struct Hit { int id; double start, length; bool midi; };

    // Audio and MIDI clips of this track, topmost last-drawn first.
    std::vector<Hit> hits() const
    {
        std::vector<Hit> out;
        for (auto& c : main.getEngine().getClips (track))
            if (c.audible())   // inactive takes are hidden
                out.push_back ({ c.id, c.start, c.length, false });
        for (auto& m : main.getEngine().getMidiClips (track))
            out.push_back ({ m.id, m.start, m.length, true });
        std::reverse (out.begin(), out.end());
        return out;
    }

    // Waveform columns for one clip at the current zoom, cached until the zoom or the clip's region changes.
    const std::vector<float>& peaksFor (const Clip& c, double pps)
    {
        auto& p = cache[c.id];
        if (p.pps != pps || p.offset != c.offset || p.length != c.length || p.audio != c.audio.get())
        {
            p.pps = pps;
            p.offset = c.offset;
            p.length = c.length;
            p.audio = c.audio.get();

            const int w = juce::jmax (2, (int) std::round (c.length * pps));
            p.values.assign ((size_t) w, 0.0f);

            const int n = c.audio->getNumSamples();
            const double spp = c.length * c.fileRate / w;
            const double base = c.offset * c.fileRate;
            for (int ch = 0; ch < c.audio->getNumChannels(); ++ch)
            {
                const auto* d = c.audio->getReadPointer (ch);
                for (int i = 0; i < w; ++i)
                {
                    const int a = juce::jmax (0, (int) (base + i * spp));
                    const int b = juce::jmin (n, (int) (base + (i + 1) * spp) + 1);
                    float m = 0.0f;
                    for (int s = a; s < b; ++s)
                        m = juce::jmax (m, std::abs (d[s]));
                    p.values[(size_t) i] = juce::jmax (p.values[(size_t) i], m);
                }
            }
        }
        return p.values;
    }

    MainComponent& main;
    Track& track;
    std::map<int, Peaks> cache;
    std::vector<AutoPoint> autoPts;   // working copy while an envelope point is being edited
    juce::String cachedInfoId;
    AutoInfo cachedInfo;
    int autoDrag = -1;
    bool autoEdited = false;
    bool armedShown = false, liveShown = false;
    double rangeA = -1.0, rangeB = -1.0;   // the time range picked with Shift+drag in an automation lane (for comping)
    bool rangeDragging = false;
    int compRow = -2;
    double compA = 0.0, compB = 0.0;
    bool compDragging = false;

    int dragId = -1;
    Track* dragTrack = nullptr;   // the track the dragged clip currently lives on
    bool resizing = false;
    int fadeDrag = 0;                                    // 1 = dragging a fade-in handle, 2 = a fade-out handle
    int fadeClipId = -1;
    int envDrag = -1, envClipId = -1;                    // the point of a clip's volume curve being dragged

    // the volume curve of a clip: its multiplier 0..2 (1 in the middle) as a height in the clip
    static float envelopeY (const juce::Rectangle<int>& r, float value) { return (float) r.getBottom() - (float) r.getHeight() * juce::jlimit (0.0f, 2.0f, value) * 0.5f; }
    static float envelopeValue (const juce::Rectangle<int>& r, int y)
    {
        return juce::jlimit (0.0f, 2.0f, 2.0f * ((float) r.getBottom() - (float) y) / (float) juce::jmax (1, r.getHeight()));
    }
    bool rightPending = false, rightDragging = false;   // right button: click = menu, drag = loop region
    int rightClipId = -1;
    double rightTime = 0.0;
    bool edited = false;   // true once this drag has taken its undo snapshot
    double grabOffset = 0.0;
    double dragStart = 0.0;
    double dragLength = 0.0;
    MidiClip resizeOriginal;   // the clip as it was when the edge drag began (every move is computed from this)
};

//==============================================================================
// Sub-tracks of a MIDI track: one row per pitch (kick, snare, hat...), under the track.

static juce::Colour subLaneColour (int index)
{
    static const juce::Colour colours[] = { juce::Colour (0xffff5a47), juce::Colour (0xfff28c28), juce::Colour (0xffffc933), juce::Colour (0xffe86a9a),
                                            juce::Colour (0xffd9702a), juce::Colour (0xffffe08a), juce::Colour (0xffc9302c), juce::Colour (0xff9ad06a) };
    return colours[(size_t) index % 8];
}

// The notes of one sub-track, over the arrangement. Click an empty spot inside a clip to add a note (one 16th, on the grid), click a note to
// remove it, like the steps of the drum sequencer.
class SubLaneRow : public juce::Component
{
public:
    SubLaneRow (MainComponent& m, Track& t, int index) : main (m), track (&t), sub (index) {}

    Track* getTrack() const { return track; }
    int getSub() const { return sub; }

    void paint (juce::Graphics& g) override
    {
        const double pps = main.getPixelsPerSecond();
        const auto area = g.getClipBounds();
        g.fillAll (Theme::background);
        g.setColour (Theme::laneBg);
        g.fillRoundedRectangle (juce::Rectangle<float> (0.0f, 1.0f, (float) getWidth(), (float) getHeight() - 2.0f), 6.0f);

        const auto lane = laneInfo();
        if (! lane.has_value())
            return;
        const auto colour = subLaneColour (sub);

        // grid
        const auto& map = main.getTempoMap();
        const double leftSeconds = area.getX() / pps, rightSeconds = area.getRight() / pps;
        const double beatPx = map.beatSecondsAt (leftSeconds) * pps;
        map.forEachGridLine (leftSeconds, rightSeconds, 1, [&] (double sec, int kind, int)
        {
            const int x = (int) (sec * pps);
            if (kind == 2)       { g.setColour (Theme::text.withAlpha (0.10f)); g.drawVerticalLine (x, 2.0f, (float) getHeight() - 2.0f); }
            else if (beatPx >= 14.0) { g.setColour (Theme::text.withAlpha (0.04f)); g.drawVerticalLine (x, 2.0f, (float) getHeight() - 2.0f); }
        });

        const bool dim = lane->mute || anySolo() && ! lane->solo;
        for (auto& clip : main.getEngine().getMidiClips (*track))
        {
            const juce::Rectangle<float> r ((float) (clip.start * pps), 3.0f, (float) (clip.length * pps), (float) getHeight() - 6.0f);
            if (! r.toNearestInt().intersects (area))
                continue;
            g.setColour (Theme::midiClip.withAlpha (0.22f));
            g.fillRoundedRectangle (r, 4.0f);
            g.setColour (colour.withAlpha (0.35f));
            g.drawRoundedRectangle (r, 4.0f, 1.0f);

            for (auto& n : clip.notes)
            {
                if (! lane->covers (n.pitch) || n.start >= clip.length)
                    continue;
                const float x = (float) ((clip.start + n.start) * pps);
                const float w = juce::jmax (4.0f, (float) (n.length * pps));
                const float vel = juce::jlimit (0.3f, 1.0f, n.velocity);
                g.setColour ((dim ? Theme::textFaint : colour).withAlpha (0.35f + 0.65f * vel));
                g.fillRoundedRectangle (x, r.getY() + 5.0f, w, r.getHeight() - 10.0f, 3.0f);
            }
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        const auto lane = laneInfo();
        if (! lane.has_value())
            return;
        auto& engine = main.getEngine();
        const double pps = main.getPixelsPerSecond();
        const double time = e.x / pps;

        if (e.mods.isPopupMenu())
        {
            main.showSubLaneMenu (*track, sub);
            return;
        }

        for (auto& clip : engine.getMidiClips (*track))
        {
            if (time < clip.start || time >= clip.end())
                continue;

            auto notes = clip.notes;
            const double local = time - clip.start;
            for (size_t i = 0; i < notes.size(); ++i)   // on a note of this sub-track: take it away
                if (lane->covers (notes[i].pitch) && local >= notes[i].start - 0.002 && local <= notes[i].start + juce::jmax (0.05, notes[i].length))
                {
                    main.beginEdit();
                    notes.erase (notes.begin() + (long) i);
                    engine.setMidiNotes (*track, clip.id, std::move (notes));
                    main.clipsChanged();
                    return;
                }

            // an empty spot: a note at the sub-track's pitch, on the grid
            const double at = e.mods.isAltDown() ? time : main.snapTime (time);
            Note n;
            n.pitch = lane->pitchLo;
            n.start = juce::jmax (0.0, at - clip.start);
            n.length = juce::jmax (0.02, main.getTempoMap().beatSecondsAt (time) * 0.25);
            n.velocity = 0.85f;
            if (n.start >= clip.length)
                return;
            main.beginEdit();
            notes.push_back (n);
            engine.setMidiNotes (*track, clip.id, std::move (notes));
            main.clipsChanged();
            auto* t = track;
            const int pitch = n.pitch;
            engine.previewNote (*t, pitch, true, 0.85f);   // (so it can be heard as it is placed)
            juce::Timer::callAfterDelay (140, [safe = juce::Component::SafePointer<SubLaneRow> (this), pitch]
            {
                if (safe != nullptr && safe->track != nullptr && safe->main.getEngine().indexOfTrack (safe->track) >= 0)
                    safe->main.getEngine().previewNote (*safe->track, pitch, false, 0.0f);
            });
            return;
        }
    }

private:
    std::optional<SubLane> laneInfo() const
    {
        if (track == nullptr || main.getEngine().indexOfTrack (track) < 0 || ! juce::isPositiveAndBelow (sub, (int) track->subLanes.size()))
            return std::nullopt;
        return track->subLanes[(size_t) sub];
    }

    bool anySolo() const
    {
        for (auto& s : track->subLanes)
            if (s.solo)
                return true;
        return false;
    }

    MainComponent& main;
    Track* track;
    int sub;
};

// The header row of a track group (folder): an arrow to collapse the tracks, the name, and mute / solo for all of them together.
class GroupHeader : public juce::Component, public juce::SettableTooltipClient, private juce::Timer
{
public:
    GroupHeader (MainComponent& m, int id) : main (m), groupId (id)
    {
        muteButton.setButtonText ("M");
        soloButton.setButtonText ("S");
        muteButton.setTooltip ("Mute or unmute all the tracks of this group");
        soloButton.setTooltip ("Solo or unsolo all the tracks of this group");
        muteButton.onClick = [this] { main.muteSoloGroup (groupId, false); };
        soloButton.onClick = [this] { main.muteSoloGroup (groupId, true); };
        addAndMakeVisible (muteButton);
        addAndMakeVisible (soloButton);
        setTooltip ("Click to collapse or expand the group. Double-click to rename. Right-click for more.");
        startTimerHz (5);
        refresh();
    }

    int getGroupId() const { return groupId; }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Theme::background);
        const auto card = getLocalBounds().toFloat().reduced (5.0f, 2.0f);
        g.setColour (Theme::panel.brighter (0.06f));
        g.fillRoundedRectangle (card, 8.0f);
        g.setColour (Theme::yellowDim);
        g.fillRoundedRectangle (card.getX() + 3.0f, card.getY() + 5.0f, 4.0f, card.getHeight() - 10.0f, 2.0f);

        auto* group = main.getEngine().findGroup (groupId);
        const bool collapsed = group != nullptr && group->collapsed;
        g.setColour (Theme::yellow);
        juce::Path arrow;   // ▸ collapsed, ▾ open
        const float cx = card.getX() + 20.0f, cy = card.getCentreY();
        if (collapsed) arrow.addTriangle (cx - 3.0f, cy - 5.0f, cx - 3.0f, cy + 5.0f, cx + 4.0f, cy);
        else           arrow.addTriangle (cx - 5.0f, cy - 3.0f, cx + 5.0f, cy - 3.0f, cx, cy + 4.0f);
        g.fillPath (arrow);

        g.setColour (Theme::text);
        g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        const int count = (int) main.getEngine().groupMembers (groupId).size();
        g.drawText (group != nullptr ? group->name : juce::String ("Group"), 34, 0, getWidth() - 34 - 82, getHeight(), juce::Justification::centredLeft, true);
        g.setColour (Theme::textFaint);
        g.setFont (juce::FontOptions (11.0f));
        g.drawText (juce::String (count) + (count == 1 ? " track" : " tracks"), getWidth() - 160, 0, 74, getHeight(), juce::Justification::centredRight);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (6, 5);
        soloButton.setBounds (r.removeFromRight (26));
        r.removeFromRight (4);
        muteButton.setBounds (r.removeFromRight (26));
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
            main.showGroupMenu (groupId);
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (! e.mods.isPopupMenu() && ! e.mouseWasDraggedSinceMouseDown() && e.getNumberOfClicks() == 1)
            main.toggleGroupCollapsed (groupId);
    }

    void mouseDoubleClick (const juce::MouseEvent&) override { main.toggleGroupCollapsed (groupId); main.renameGroup (groupId); }   // (the first click collapsed, the second undoes it)

private:
    void timerCallback() override { refresh(); }

    void refresh()
    {
        bool allMuted = true, anySolo = false;
        const auto members = main.getEngine().groupMembers (groupId);
        for (auto* t : members)
        {
            allMuted = allMuted && t->mute.load();
            anySolo = anySolo || t->solo.load();
        }
        const bool muted = ! members.empty() && allMuted;
        if (muted != muteShown || anySolo != soloShown)
        {
            muteShown = muted;
            soloShown = anySolo;
            muteButton.setColour (juce::TextButton::buttonColourId, muted ? Theme::redBright : Theme::buttonOff);
            soloButton.setColour (juce::TextButton::buttonColourId, anySolo ? Theme::yellow : Theme::buttonOff);
            soloButton.setColour (juce::TextButton::textColourOffId, anySolo ? Theme::background : Theme::text);
        }
        repaint();
    }

    MainComponent& main;
    int groupId;
    juce::TextButton muteButton, soloButton;
    bool muteShown = false, soloShown = false;
};

// The left side of a sub-track: its name, the pitch it stands for, and mute / solo for just that pitch.
class SubLaneHeader : public juce::Component, private juce::Timer
{
public:
    SubLaneHeader (MainComponent& m, Track& t, int index) : main (m), track (&t), sub (index)
    {
        name.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        name.setColour (juce::Label::textColourId, Theme::text);
        name.setInterceptsMouseClicks (false, false);
        name.setEditable (false, false, true);   // (the editor is opened by the double-click on the header)
        name.onTextChange = [this]
        {
            const auto text = name.getText().trim();
            if (text.isEmpty() || main.getEngine().indexOfTrack (track) < 0 || ! juce::isPositiveAndBelow (sub, (int) track->subLanes.size())
                || text == track->subLanes[(size_t) sub].name)
            {
                refresh();
                return;
            }
            main.renameSubLane (*track, sub, text);   // (later: the header is rebuilt with the new name)
        };
        addAndMakeVisible (name);
        pitchLabel.setFont (juce::FontOptions (11.0f));
        pitchLabel.setColour (juce::Label::textColourId, Theme::textFaint);
        pitchLabel.setInterceptsMouseClicks (false, false);
        addAndMakeVisible (pitchLabel);

        for (auto* b : { &muteButton, &soloButton })
        {
            b->setClickingTogglesState (true);
            addAndMakeVisible (*b);
        }
        muteButton.setColour (juce::TextButton::buttonOnColourId, Theme::redBright);
        soloButton.setColour (juce::TextButton::buttonOnColourId, Theme::yellow);
        muteButton.onClick = [this]
        {
            main.beginEdit();
            main.getEngine().setSubLaneState (*track, sub, muteButton.getToggleState() ? 1 : 0, -1);
        };
        soloButton.onClick = [this]
        {
            main.beginEdit();
            main.getEngine().setSubLaneState (*track, sub, -1, soloButton.getToggleState() ? 1 : 0);
        };
        volume.setSliderStyle (juce::Slider::LinearHorizontal);
        volume.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        volume.setRange (0.0, 1.5, 0.01);
        volume.setDoubleClickReturnValue (true, 1.0);   // double-click: back to 1 (the notes as written)
        volume.setTooltip ("Volume of this sub-track (scales the note velocities). Double-click: reset");
        volume.onDragStart = [this] { main.beginEdit(); };
        volume.onValueChange = [this]
        {
            if (main.getEngine().indexOfTrack (track) >= 0)
                main.getEngine().setSubLaneVolume (*track, sub, (float) volume.getValue());
        };
        addAndMakeVisible (volume);
        refresh();
        startTimerHz (30);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Theme::background);
        const auto card = getLocalBounds().toFloat().reduced (5.0f, 2.0f);
        g.setColour (Theme::panel);
        g.fillRoundedRectangle (card, 8.0f);
        g.setColour (subLaneColour (sub));
        g.fillRoundedRectangle (card.getX() + 3.0f, card.getY() + 6.0f, 4.0f, card.getHeight() - 12.0f, 2.0f);

        // level meter: how hard this sub-track is being hit (the velocity of its notes, falling back after each one)
        const auto m = meterBounds();
        g.setColour (juce::Colours::black);
        g.fillRect (m);
        const float lvl = juce::jlimit (0.0f, 1.0f, shownLevel);
        g.setColour (lvl > 0.9f ? Theme::redBright : subLaneColour (sub));
        g.fillRect (m.withTop (m.getBottom() - (int) (lvl * (float) m.getHeight())));
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (14, 4);
        r.removeFromRight (14);
        auto buttons = r.removeFromRight (56);
        muteButton.setBounds (buttons.removeFromLeft (27).reduced (1, 8));
        soloButton.setBounds (buttons.reduced (1, 8));
        name.setBounds (r.removeFromTop (r.getHeight() / 2 + 2));
        pitchLabel.setBounds (r.removeFromLeft (juce::jmin (84, r.getWidth() / 2)));
        volume.setBounds (r);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
            main.showSubLaneMenu (*track, sub);
    }

    // double-click the name: rename it right there; anywhere else on the header: the sub-track menu
    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (e.y < getHeight() / 2 + 2 && e.x < name.getRight())
            name.showEditor();
        else
            main.showSubLaneMenu (*track, sub);
    }

private:
    void refresh()
    {
        if (main.getEngine().indexOfTrack (track) < 0 || ! juce::isPositiveAndBelow (sub, (int) track->subLanes.size()))
            return;
        const auto& lane = track->subLanes[(size_t) sub];
        if (! name.isBeingEdited())
            name.setText (lane.name, juce::dontSendNotification);
        const auto noteName = [] (int p) { return juce::MidiMessage::getMidiNoteName (p, true, true, 3) + " (" + juce::String (p) + ")"; };
        pitchLabel.setText (lane.pitchLo == lane.pitchHi ? noteName (lane.pitchLo) : noteName (lane.pitchLo) + " - " + noteName (lane.pitchHi),
                            juce::dontSendNotification);
        if (! volume.isMouseButtonDown() && std::abs (volume.getValue() - (double) lane.volume) > 0.001)
            volume.setValue (lane.volume, juce::dontSendNotification);
        if (muteButton.getToggleState() != lane.mute) muteButton.setToggleState (lane.mute, juce::dontSendNotification);
        if (soloButton.getToggleState() != lane.solo) soloButton.setToggleState (lane.solo, juce::dontSendNotification);
    }

    juce::Rectangle<int> meterBounds() const { return { getWidth() - 21, 8, 8, getHeight() - 16 }; }

    void timerCallback() override
    {
        if (++tickCount % 5 == 0)
            refresh();

        float hit = 0.0f;
        if (main.getEngine().indexOfTrack (track) >= 0 && juce::isPositiveAndBelow (sub, (int) track->subLanes.size()))
        {
            const auto& lane = track->subLanes[(size_t) sub];
            for (int p = juce::jmax (0, lane.pitchLo); p <= juce::jmin (127, lane.pitchHi); ++p)
                hit = juce::jmax (hit, track->pitchActivity[p].exchange (0.0f));
        }
        shownLevel = hit > shownLevel ? hit : shownLevel * 0.88f;
        const int bin = (int) (juce::jlimit (0.0f, 1.0f, shownLevel) * 30.0f);
        if (bin != shownBin)
        {
            shownBin = bin;
            repaint (meterBounds());
        }
    }

    MainComponent& main;
    Track* track;
    int sub;
    int tickCount = 0, shownBin = 0;
    float shownLevel = 0.0f;
    juce::Label name, pitchLabel;
    juce::Slider volume;
    juce::TextButton muteButton { "M" }, soloButton { "S" };
};

// Under the sub-tracks of a track: how many there are, with - and + to take one away or add one.
class SubLaneFooter : public juce::Component, private juce::Timer
{
public:
    SubLaneFooter (MainComponent& m, Track& t) : main (m), track (&t)
    {
        info.setFont (juce::FontOptions (12.0f));
        info.setColour (juce::Label::textColourId, Theme::textDim);
        info.setInterceptsMouseClicks (false, false);
        addAndMakeVisible (info);
        minus.setTooltip ("Remove the last sub-track, with its notes");
        plus.setTooltip ("Add a sub-track (the next pitch up; right-click a sub-track to change its pitch, double-click its name to rename it)");
        minus.onClick = [this] { main.changeSubLaneCount (*track, -1); };
        plus.onClick = [this] { main.changeSubLaneCount (*track, 1); };
        addAndMakeVisible (minus);
        addAndMakeVisible (plus);
        refresh();
        startTimerHz (4);
    }

    Track* getTrack() const { return track; }

    void paint (juce::Graphics& g) override { g.fillAll (Theme::background); }

    void resized() override
    {
        auto r = getLocalBounds().reduced (14, 3);
        r.removeFromRight (14);
        plus.setBounds (r.removeFromRight (34));
        r.removeFromRight (4);
        minus.setBounds (r.removeFromRight (34));
        info.setBounds (r);
    }

private:
    void refresh()
    {
        if (main.getEngine().indexOfTrack (track) < 0)
            return;
        const int n = (int) track->subLanes.size();
        info.setText (juce::String (n) + (n == 1 ? " sub-track" : " sub-tracks"), juce::dontSendNotification);
        minus.setEnabled (n > 0);
    }

    void timerCallback() override { refresh(); }

    MainComponent& main;
    Track* track;
    juce::Label info;
    juce::TextButton minus { "-" }, plus { "+" };
};

//==============================================================================
// The audio settings window: JUCE's device selector (driver, device, sample rate, buffer size, channels),
// plus a note on ASIO when no ASIO driver type is available in this build.
class AudioDeviceTab : public juce::Component
{
public:
    explicit AudioDeviceTab (juce::AudioDeviceManager& dm)
        : selector (dm, 0, 256, 1, 256, false, false, true, false)
    {
        addAndMakeVisible (selector);

        bool hasAsio = false;
        for (auto* type : dm.getAvailableDeviceTypes())
            hasAsio = hasAsio || type->getTypeName() == "ASIO";

        if (! hasAsio)
        {
            hint.setText ("ASIO drivers are not listed because this build does not include ASIO support. "
                          "\"Windows Audio (Exclusive Mode)\" and \"Windows Audio (Low Latency Mode)\" are the "
                          "low-latency options meanwhile; see the README (ASIO) to switch ASIO on.",
                          juce::dontSendNotification);
            hint.setFont (juce::FontOptions (12.5f));
            hint.setColour (juce::Label::textColourId, Theme::yellow);
            hint.setMinimumHorizontalScale (1.0f);
            addAndMakeVisible (hint);
        }
        showsHint = ! hasAsio;
        setSize (560, showsHint ? 600 : 530);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (10);
        if (showsHint)
            hint.setBounds (area.removeFromBottom (64));
        selector.setBounds (area);
    }

private:
    juce::AudioDeviceSelectorComponent selector;
    juce::Label hint;
    bool showsHint = false;
};

// The audio settings window has three tabs: the device, the recording latency compensation, and MIDI clock to hardware.
class AudioSettingsPanel : public juce::TabbedComponent
{
public:
    AudioSettingsPanel (juce::AudioDeviceManager& dm, Engine& engine, std::function<void()> onSettingsChanged)
        : juce::TabbedComponent (juce::TabbedButtonBar::TabsAtTop)
    {
        setTabBarDepth (32);
        setOutline (0);
        auto* device = new AudioDeviceTab (dm);
        auto* latency = new LatencyPanel (dm, engine, onSettingsChanged);
        auto* clock = new ClockPanel (engine, onSettingsChanged);
        const int width = juce::jmax (device->getWidth(), latency->getWidth(), clock->getWidth());   // (adding a tab resizes it)
        const int height = juce::jmax (device->getHeight(), latency->getHeight(), clock->getHeight());
        addTab ("Audio device", Theme::panel, device, true);
        addTab ("Recording latency", Theme::panel, latency, true);
        addTab ("MIDI clock", Theme::panel, clock, true);
        setSize (width, height + getTabBarDepth());
    }
};

// Renders every stem on a background thread behind a progress window (with a cancel button), then reports.
class StemExportJob : public juce::ThreadWithProgressWindow
{
public:
    struct Item { Track* track; juce::File file; };

    StemExportJob (Engine& e, std::vector<Item> itemsToRender, double lengthSeconds, int formatChoiceIndex, bool withMaster,
                   juce::File targetFolder, juce::StringArray skippedTracks, juce::Component* centreAround)
        : juce::ThreadWithProgressWindow ("Exporting stems", true, true, 5000, "Cancel", centreAround),
          engine (e), items (std::move (itemsToRender)), seconds (lengthSeconds), formatChoice (formatChoiceIndex), master (withMaster),
          folder (std::move (targetFolder)), skipped (std::move (skippedTracks))
    {
    }

    void run() override
    {
        for (size_t i = 0; i < items.size() && ! threadShouldExit(); ++i)
        {
            setStatusMessage ("Rendering " + items[i].file.getFileNameWithoutExtension()
                              + "   (" + juce::String ((int) i + 1) + " of " + juce::String ((int) items.size()) + ")");

            Engine::RenderOptions options;
            options.onlyTrack = items[i].track;
            options.formatChoice = formatChoice;
            options.masterProcessing = master;
            options.shouldCancel = [this] { return threadShouldExit(); };
            options.onProgress = [this, i] (double f) { setProgress (((double) i + f) / (double) items.size()); };

            if (engine.renderToFile (items[i].file, seconds, options))
                done.add (items[i].file.getFileName());
            else if (! threadShouldExit())
                failed.add (items[i].file.getFileName());
        }
    }

    // Message thread, once the thread is finished or cancelled. (JUCE says this object may be deleted here.)
    void threadComplete (bool userPressedCancel) override
    {
        juce::String message;
        if (userPressedCancel)
            message << "Cancelled. " << done.size() << " stem(s) were finished before that.\n\n";
        else
            message << done.size() << " stem(s) exported to:\n" << folder.getFullPathName() << "\n\n";
        if (failed.size() > 0)
            message << "Could not write: " << failed.joinIntoString (", ") << "\n\n";
        if (skipped.size() > 0)
            message << "Not exported: " << skipped.joinIntoString (", ");

        auto target = folder;
        juce::AlertWindow::showAsync (juce::MessageBoxOptions()
                                          .withIconType (failed.isEmpty() ? juce::MessageBoxIconType::InfoIcon
                                                                          : juce::MessageBoxIconType::WarningIcon)
                                          .withTitle (userPressedCancel ? "Export cancelled" : "Stems exported")
                                          .withMessage (message.trim())
                                          .withButton ("Open folder")
                                          .withButton ("Close"),
                                      [target] (int result) { if (result == 1) target.revealToUser(); });
        delete this;
    }

private:
    Engine& engine;
    std::vector<Item> items;
    double seconds;
    int formatChoice;
    bool master;
    juce::File folder;
    juce::StringArray skipped, done, failed;
};

//==============================================================================
MainComponent::MainComponent (int slot, std::shared_ptr<juce::AudioDeviceManager> devices)
    : sharedDevices (devices != nullptr ? std::move (devices) : std::make_shared<juce::AudioDeviceManager>()),
      deviceManager (*sharedDevices), tabSlot (slot)
{
    // These must exist before setSize() triggers resized()
    ruler = std::make_unique<TimeRuler> (*this);
    pianoRoll = std::make_unique<PianoRoll> (*this);
    drumRoll = std::make_unique<DrumSequencer> (*this);
    roll = pianoRoll.get();
    pianoRoll->onClose = [this] { hideRoll(); };
    drumRoll->onClose = [this] { hideRoll(); };
    addChildComponent (*pianoRoll);
    addChildComponent (*drumRoll);

    setWantsKeyboardFocus (true);
    engine.beforeRecordedTake = [this] { beginEdit(); };   // a recorded ride can be undone

    // Start with the audio setup of the last run (driver, device, rate, buffer, channels); fall back to the default device.
    // (With project tabs the shared device manager is already open.)
    if (deviceManager.getCurrentAudioDeviceType().isEmpty())
        initialiseDevices (deviceManager);
    deviceManager.addChangeListener (this);
    player.setSource (&engine);
    deviceManager.addAudioCallback (&player);
    updateAudioInfo();

    deviceManager.addMidiInputDeviceCallback ({}, &midiForwarder);   // all enabled MIDI inputs
    midiListConnection = juce::MidiDeviceListConnection::make ([this] { enableAllMidiInputs(); });   // hot-plugging
    statusLabel.setFont (juce::FontOptions (11.0f));
    statusLabel.setMinimumHorizontalScale (0.75f);   // (squeezed a little before it wraps)
    statusLabel.setColour (juce::Label::textColourId, Theme::text.withAlpha (0.6f));
    addAndMakeVisible (statusLabel);

    keysButton.setTooltip ("Play the armed MIDI track with the computer keyboard: A W S E D F T G Y H U J K O L P. "
                           "Z / X change octave. While on, those letters are not shortcuts.");
    keysButton.setWantsKeyboardFocus (false);
    keysButton.onClick = [this] { grabKeyboardFocus(); };
    addAndMakeVisible (keysButton);

    audioButton.setTooltip ("Audio driver and interface: driver type (ASIO, Windows Audio...), device, sample rate, buffer size, channels");
    audioButton.setWantsKeyboardFocus (false);
    audioButton.onClick = [this] { openAudioSettings(); };
    addAndMakeVisible (audioButton);

    pluginsButton.setTooltip ("Scan for and manage VST3 plugins");
    pluginsButton.setWantsKeyboardFocus (false);
    pluginsButton.onClick = [this] { pluginHost.showManager(); };
    addAndMakeVisible (pluginsButton);
    enableAllMidiInputs();

    for (juce::Component* b : std::initializer_list<juce::Component*> {
             &rewindButton, &playButton, &pauseButton, &stopButton, &recordButton, &addButton, &addMidiButton,
             &openButton, &saveButton, &saveAsButton, &exportButton, &undoButton, &redoButton, &zoomOutButton, &zoomInButton })
        addAndMakeVisible (b);
    loopButton.setClickingTogglesState (true);
    loopButton.setColour (juce::TextButton::buttonOnColourId, Theme::orange);
    loopButton.setTooltip ("Loop playback (L). Right-drag on the ruler sets the loop; Shift+L loops the selected clip.");
    loopButton.setWantsKeyboardFocus (false);
    loopButton.onClick = [this] { toggleLoop(); grabKeyboardFocus(); };
    addAndMakeVisible (loopButton);

    punchButton.setClickingTogglesState (true);
    punchButton.setColour (juce::TextButton::buttonOnColourId, Theme::redBright);
    punchButton.setTooltip ("Punch in/out (P): a recording only captures inside the red punch region. Shift + right-drag on the ruler sets it. "
                            "Press Record before the region: playback starts a bar ahead and recording starts and stops by itself.");
    punchButton.setWantsKeyboardFocus (false);
    punchButton.onClick = [this] { togglePunch(); grabKeyboardFocus(); };
    addAndMakeVisible (punchButton);

    tunerButton.setTooltip ("Tuner: shows the note and how many cents off it is, for the input you pick (default: the armed track's input)");
    tunerButton.setWantsKeyboardFocus (false);
    tunerButton.onClick = [this] { openTuner(); };
    addAndMakeVisible (tunerButton);

    undoButton.onClick = [this] { undo(); };
    redoButton.onClick = [this] { redo(); };
    updateUndoButtons();
    addAndMakeVisible (metroButton);
    addAndMakeVisible (snapButton);
    addAndMakeVisible (bpmLabel);
    addAndMakeVisible (bpmSlider);
    addAndMakeVisible (timeLabel);

    // the master channel strip under the tracks
    masterHeader = makeHeader (engine.getMaster(), true);
    masterLane = std::make_unique<TrackLane> (*this, engine.getMaster());
    masterArea.addAndMakeVisible (*masterLane);
    addAndMakeVisible (*masterHeader);
    addAndMakeVisible (masterArea);

    rewindButton.onClick = [this] { engine.setPositionSeconds (0.0); playStartSeconds = 0.0; };
    playButton.onClick   = [this] { startPlayback(); };
    pauseButton.onClick  = [this] { pausePlayback(); };
    stopButton.onClick   = [this] { stopPlayback(); };
    recordButton.onClick = [this] { toggleRecording(); };
    addButton.onClick = [this] { addTrackMenu(); };
    addMidiButton.onClick = [this] { addMidiTrack (false); };
    addDrumButton.onClick = [this] { addMidiTrack (true); };
    addAndMakeVisible (addDrumButton);
    metroButton.setClickingTogglesState (true);
    metroButton.setColour (juce::TextButton::buttonOnColourId, Theme::orange);
    metroButton.setWantsKeyboardFocus (false);
    metroButton.onClick = [this] { engine.metronome = metroButton.getToggleState(); grabKeyboardFocus(); };

    countInButton.setWantsKeyboardFocus (false);
    countInButton.onClick = [this]
    {
        juce::PopupMenu menu;
        menu.addItem (1, "No count-in", true, UiSettings::countInBars == 0);
        for (int bars = 1; bars <= 4; ++bars)
            menu.addItem (bars + 1, juce::String (bars) + (bars == 1 ? " bar" : " bars"), true, UiSettings::countInBars == bars);
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&countInButton), [this] (int result)
        {
            if (result == 0)
                return;
            UiSettings::countInBars = result - 1;
            saveUiSettings();
            updateCountInButton();
        });
    };
    countInButton.setTooltip ("Count-in: this many bars of metronome clicks play before a recording starts (in the tempo and time signature "
                              "at the playhead, also when the metronome is off). Nothing is recorded during the count-in.");
    addAndMakeVisible (countInButton);

    followButton.setClickingTogglesState (true);
    followButton.setToggleState (UiSettings::follow, juce::dontSendNotification);
    followButton.setColour (juce::TextButton::buttonOnColourId, Theme::orangeDim);
    followButton.setWantsKeyboardFocus (false);
    followButton.setTooltip ("The arrangement scrolls along with the playhead while playing");
    followButton.onClick = [this] { UiSettings::follow = followButton.getToggleState(); saveUiSettings(); };
    followButton.getProperties().set ("pill", true);
    addAndMakeVisible (followButton);
    snapButton.setToggleState (true, juce::dontSendNotification);

    loadUiSettings();
    panSnapButton.setToggleState (UiSettings::panSnap, juce::dontSendNotification);
    panSnapButton.setTooltip ("On: pan sliders and pan envelopes jump in 5% steps (L 5%, L 10%, ... Center ... R 100%).\n"
                              "Off: free movement. Hold Alt while dragging to get the other mode for that move.");
    panSnapButton.onClick = [this]
    {
        UiSettings::panSnap = panSnapButton.getToggleState();
        saveUiSettings();
    };
    addAndMakeVisible (panSnapButton);
    openButton.onClick   = [this] { openProjectDialog(); };
    saveButton.onClick   = [this] { if (currentProject != juce::File()) saveProject (currentProject); else saveProjectAs(); };
    saveAsButton.onClick = [this] { saveMenu(); };
    saveAsButton.setTooltip ("Save as..., save a new version (v2, v3...), or save with all audio in one folder (to move the project)");
    exportButton.onClick = [this] { exportDialog(); };
    exportStemsButton.onClick = [this] { exportStemsDialog(); };
    exportStemsButton.setTooltip ("Export every track as its own WAV file (a stem) into a folder you choose");
    addChildComponent (exportStemsButton);   // (reached through the File menu)
    for (auto* hidden : { &openButton, &saveButton, &saveAsButton, &exportButton })
        hidden->setVisible (false);

    fileButton.setTooltip ("New project, open, recent projects, save, export");
    fileButton.onClick = [this] { fileMenu(); };
    addAndMakeVisible (fileButton);
    stopAllButton.setTooltip ("Stops playback and recording and cuts off every sound at once, including plugins and MIDI hardware (Ctrl+Shift+Space)");
    stopAllButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xffc8281e));
    stopAllButton.setColour (juce::TextButton::textColourOffId, juce::Colours::white);
    stopAllButton.onClick = [this] { finishRecording(); engine.silenceEverything(); engine.setPositionSeconds (playStartSeconds); };
    addAndMakeVisible (stopAllButton);

    // a newer version exists: this button shows up (see checkForUpdates)
    updateButton.getProperties().set ("pill", true);
    updateButton.setColour (juce::TextButton::buttonColourId, Theme::orange);
    updateButton.setColour (juce::TextButton::textColourOffId, Theme::background);
    updateButton.setWantsKeyboardFocus (false);
    updateButton.onClick = [this] { showUpdateInfo(); grabKeyboardFocus(); };
    addChildComponent (updateButton);
    {
        static bool startupCheckDone = false;   // (once per program start, not for every tab)
        const bool dueToday = juce::Time::currentTimeMillis() - UiSettings::lastUpdateCheck > 20ll * 3600ll * 1000ll;
        if (! startupCheckDone && UiSettings::autoUpdateCheck && dueToday)
        {
            startupCheckDone = true;
            juce::Component::SafePointer<MainComponent> safe (this);
            juce::Timer::callAfterDelay (7000, [safe] { if (safe != nullptr) safe->checkForUpdates (false); });
        }
    }

    // the icon rail replaces these buttons; they stay (hidden, placed over their rail icon) as the anchors of their menus
    for (auto* hidden : { &addButton, &addMidiButton, &addDrumButton, &pluginsButton, &audioButton, &tunerButton, &fileButton })
        hidden->setVisible (false);
    railFile.onClick = [this] { fileMenu(); };
    railAdd.onClick = [this] { addTrackMenu(); };
    railPlugins.onClick = [this] { pluginHost.showManager(); };
    railTuner.onClick = [this] { openTuner(); };
    railMixer.onClick = [this] { openMixer(); };
    railAudio.onClick = [this] { openAudioSettings(); };
    for (auto* rb : { &railFile, &railAdd, &railPlugins, &railTuner, &railMixer, &railAudio })
        addAndMakeVisible (rb);
    for (auto* pill : { &tempoButton, &countInButton, &undoButton, &redoButton, &stopAllButton })
        pill->getProperties().set ("pill", true);
    zoomOutButton.onClick = [this] { zoomBy (1.0 / 1.4, laneViewport.getMaximumVisibleWidth() / 2); };
    zoomInButton.onClick  = [this] { zoomBy (1.4, laneViewport.getMaximumVisibleWidth() / 2); };

    bpmSlider.setRange (40.0, 240.0, 1.0);
    bpmSlider.setValue (120.0, juce::dontSendNotification);
    bpmSlider.onDragStart = [this] { beginEdit(); };
    bpmSlider.onValueChange = [this]
    {
        auto sections = tempoMap.getSections();   // the slider is the tempo of the first section
        sections[0].bpm = bpmSlider.getValue();
        TempoMap m;
        m.setSections (sections);
        applyTempoMap (m);
    };

    clipLamp.onClick = [this] { engine.clearClipIndicators(); updateClipLamp(); repaint(); };
    addAndMakeVisible (clipLamp);

    tempoButton.setTooltip ("Tempo and time signature changes: add sections that start at a bar (for example 3/4 from bar 17, or 140 BPM from bar 33). "
                            "MIDI follows the tempo; audio clips stay where they are in time.");
    tempoButton.setWantsKeyboardFocus (false);
    tempoButton.onClick = [this] { openTempoPanel(); };
    addAndMakeVisible (tempoButton);

    tapButton.getProperties().set ("pill", true);
    tapButton.setTooltip ("Tap tempo: tap in time (or press T) and the tempo follows. Two seconds without a tap start over.");
    tapButton.setWantsKeyboardFocus (false);
    tapButton.onClick = [this] { tapTempo(); grabKeyboardFocus(); };
    addAndMakeVisible (tapButton);

    sectionsButton.getProperties().set ("pill", true);
    sectionsButton.setTooltip ("The sections of the arrangement (verse, chorus...): name them, loop one, copy one or swap two. They show as bands on the ruler.");
    sectionsButton.setWantsKeyboardFocus (false);
    sectionsButton.onClick = [this] { openSectionsPanel(); };
    addAndMakeVisible (sectionsButton);


    timeLabel.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 24.0f, juce::Font::plain));
    timeLabel.setJustificationType (juce::Justification::centred);
    timeLabel.setColour (juce::Label::textColourId, Theme::yellow);
    timeLabel.setColour (juce::Label::backgroundColourId, Theme::laneBg);
    timeLabel.setColour (juce::Label::outlineColourId, Theme::border);
    timeLabel.setTooltip ("Playhead position (minutes:seconds)");

    // hover hints for everything in the top bar
    addButton.setTooltip ("Add an audio track: an empty one to record on, or from audio files (you can also drag files into the window)");
    addMidiButton.setTooltip ("Add a synth track: play notes, or draw them in the piano roll");
    addDrumButton.setTooltip ("Add a drum track: click steps in a grid, or load your own samples per pad");
    openButton.setTooltip ("Open a project (Ctrl+O)");
    saveButton.setTooltip ("Save the project (Ctrl+S)");
    saveAsButton.setTooltip ("Save the project under a new name");
    exportButton.setTooltip ("Render the whole project to a WAV file");
    undoButton.setTooltip ("Undo (Ctrl+Z)");
    redoButton.setTooltip ("Redo (Ctrl+Y)");
    metroButton.setTooltip ("Metronome on / off (M), also while playing or recording. One click per beat, the first of each bar higher.");
    snapButton.setTooltip ("Snap moves and notes to a 16th-note grid (hold Alt while dragging to ignore it)");
    zoomInButton.setTooltip ("Zoom in (Ctrl + mouse wheel)");
    zoomOutButton.setTooltip ("Zoom out (Ctrl + mouse wheel)");
    bpmSlider.setTooltip ("Tempo in beats per minute (drag, or double-click to type)");

    laneViewport.setViewedComponent (&laneHolder, false);
    laneViewport.setScrollBarsShown (true, true);
    laneViewport.onScroll = [this]
    {
        headerViewport.setViewPosition (0, laneViewport.getViewPositionY());
        layoutMasterLane();
        ruler->repaint();
        roll->repaintGrid();
        repaint();
    };
    addAndMakeVisible (laneViewport);

    headerViewport.setViewedComponent (&headerHolder, false);
    headerViewport.setScrollBarsShown (false, false);
    addAndMakeVisible (headerViewport);
    addAndMakeVisible (*ruler);

    for (auto& v : ccValue) v = 0;
    for (auto& s : ccStamp) s = 0;
    pluginHost.onEditorOpened = [this] (PluginSlot& s) { pluginEditorOpened (s); };
    pluginHost.onEditorClosed = [this] (PluginSlot& s) { pluginEditorClosed (s); };
    buildCommands();
    setSize (1400, 640);   // last: resized() touches all the children above
    startTimerHz (30);
}

// How a sampler plays, as saved in the project (the sample itself is a file next to it)
static void writeSamplerParams (juce::DynamicObject& so, const Sampler::Params& sp, double stretch)
{
    so.setProperty ("mode", sp.mode.load());
    so.setProperty ("root", sp.rootNote.load());
    so.setProperty ("tune", (double) sp.tuneCents.load());
    so.setProperty ("start", (double) sp.start.load());
    so.setProperty ("end", (double) sp.end.load());
    so.setProperty ("loop", sp.loop.load());
    so.setProperty ("loopStart", (double) sp.loopStart.load());
    so.setProperty ("loopEnd", (double) sp.loopEnd.load());
    so.setProperty ("reverse", sp.reverse.load());
    so.setProperty ("attack", (double) sp.attack.load());
    so.setProperty ("decay", (double) sp.decay.load());
    so.setProperty ("sustain", (double) sp.sustain.load());
    so.setProperty ("release", (double) sp.release.load());
    so.setProperty ("cutoff", (double) sp.cutoff.load());
    so.setProperty ("volume", (double) sp.volume.load());
    so.setProperty ("velocity", (double) sp.velocitySens.load());
    so.setProperty ("slices", sp.slices.load());
    if (const int marked = sp.markCount.load(); marked > 0)   // slices that follow the hits
    {
        juce::Array<juce::var> list;
        for (int i = 0; i < marked; ++i)
            list.add ((double) sp.marks[i].load());
        so.setProperty ("sliceMarks", list);
    }
    if (std::abs (stretch - 1.0) > 0.001)
        so.setProperty ("stretch", stretch);   // the time-stretch ratio (applied again to the sample when the project opens)
}

static void readSamplerParams (const juce::var& sv, Sampler::Params& sp)
{
    sp.mode = juce::jlimit (0, 2, (int) sv.getProperty ("mode", sp.mode.load()));
    sp.rootNote = (int) sv.getProperty ("root", sp.rootNote.load());
    sp.tuneCents = (float) (double) sv.getProperty ("tune", (double) sp.tuneCents.load());
    sp.start = (float) (double) sv.getProperty ("start", (double) sp.start.load());
    sp.end = (float) (double) sv.getProperty ("end", (double) sp.end.load());
    sp.loop = (bool) sv.getProperty ("loop", sp.loop.load());
    sp.loopStart = (float) (double) sv.getProperty ("loopStart", (double) sp.loopStart.load());
    sp.loopEnd = (float) (double) sv.getProperty ("loopEnd", (double) sp.loopEnd.load());
    sp.reverse = (bool) sv.getProperty ("reverse", sp.reverse.load());
    sp.attack = (float) (double) sv.getProperty ("attack", (double) sp.attack.load());
    sp.decay = (float) (double) sv.getProperty ("decay", (double) sp.decay.load());
    sp.sustain = (float) (double) sv.getProperty ("sustain", (double) sp.sustain.load());
    sp.release = (float) (double) sv.getProperty ("release", (double) sp.release.load());
    sp.cutoff = (float) (double) sv.getProperty ("cutoff", (double) sp.cutoff.load());
    sp.volume = (float) (double) sv.getProperty ("volume", (double) sp.volume.load());
    sp.velocitySens = (float) (double) sv.getProperty ("velocity", (double) sp.velocitySens.load());
    sp.slices = (int) sv.getProperty ("slices", sp.slices.load());
    if (auto* list = sv["sliceMarks"].getArray())
    {
        int n = 0;
        for (auto& m : *list)
            if (n < Sampler::Params::maxMarks)
                sp.marks[n++].store ((float) (double) m);
        sp.markCount = n;
        if (n > 0)
            sp.slices = n;
    }
    else
        sp.markCount = 0;
}

MainComponent::~MainComponent()
{
    learnWindow.reset();
    pluginHost.onEditorOpened = nullptr;   // (the editor windows close below, with the project half gone)
    pluginHost.onEditorClosed = nullptr;
    mixerWindow.reset();
    meterWindow.reset();
    browserWindow.reset();
    shortcutsWindow.reset();
    palette.reset();
    warpWindow.reset();
    historyWindow.reset();
    stopTimer();
    samplerWindow.reset();   // (it points into the engine's tracks)
    padSamplerWindow.reset();
    drumRackWindow.reset();
    if (! isDirty())
        clearAutosave();   // a normal exit with everything saved leaves no recovery file
    else
        autosaveNow();     // unsaved work is kept for the next start, which offers to restore it
    deviceManager.removeChangeListener (this);
    pluginHost.closeAllEditors();   // editor windows must go before the plugins they show
    midiListConnection = {};
    deviceManager.removeMidiInputDeviceCallback ({}, &midiForwarder);
    deviceManager.removeAudioCallback (&player);
    player.setSource (nullptr);
}

//==============================================================================
double MainComponent::snapTime (double seconds) const
{
    if (! snapButton.getToggleState())
        return seconds;
    const double beats = tempoMap.secondsToBeats (seconds);   // snap in 16th notes, wherever the tempo changes
    return tempoMap.beatsToSeconds (std::round (beats * 4.0) / 4.0);
}

// The lamp in the top bar lights when the master mix, any track, or the input of an armed track has clipped; the tooltip says where.
void MainComponent::updateClipLamp()
{
    juce::StringArray where;
    if (engine.getMaster().clipped.load())
        where.add ("the master mix (over 0 dB before the limiter)");
    for (int i = 0; i < engine.getNumTracks(); ++i)
        if (auto* t = engine.getTrack (i))
        {
            if (t->inputClipped.load())
                where.add (t->name + " (input: lower the gain on the interface)");
            if (t->clipped.load())
                where.add (t->name);
        }

    const auto text = where.joinIntoString ("\n");
    if (text == clipLampText)
        return;
    clipLampText = text;
    clipLamp.setLit (where.size() > 0);
    clipLamp.setTooltip (where.isEmpty() ? juce::String ("Lights up when the mix, a track or an armed input clips (reaches full scale). Click to clear.")
                                         : "Clipped:\n" + text + "\n\nClick to clear.");
}

// A new tempo map: MIDI, the loop and the punch region are re-timed so they keep their place in the music.
void MainComponent::applyTempoMap (const TempoMap& m)
{
    tempoMap = m;
    engine.setTempoMap (m, true);
    clipsChanged();
}

// ---- The sections of the arrangement (verse, chorus...)

void MainComponent::openSectionsPanel()
{
    juce::Component::SafePointer<MainComponent> safe (this);
    auto began = std::make_shared<bool> (false);
    SectionsPanel::Hooks hooks;
    hooks.fetch = [safe] { return safe != nullptr ? safe->engine.getSections() : std::vector<ArrSection>(); };
    hooks.edited = [safe, began] (std::vector<ArrSection> s)   // names, bars, colours: one undo step for everything done in this panel
    {
        if (safe == nullptr)
            return;
        if (! *began)
        {
            *began = true;
            safe->beginEdit();
        }
        safe->engine.setSections (std::move (s));
        safe->ruler->repaint();
    };
    hooks.goTo = [safe] (int i) { if (safe != nullptr) safe->sectionGoTo (i); };
    hooks.loop = [safe] (int i) { if (safe != nullptr) safe->sectionLoop (i); };
    hooks.copy = [safe] (int i) { if (safe != nullptr) safe->sectionCopy (i); };
    hooks.toClipboard = [safe] (int i) { if (safe != nullptr) safe->sectionToClipboard (i); };
    hooks.pasteClipboard = [safe] { if (safe != nullptr) safe->sectionPaste(); };
    hooks.canPaste = [] { return sectionClipboard.valid; };
    hooks.moveEarlier = [safe] (int i) { if (safe != nullptr) safe->sectionSwap (i - 1); };
    hooks.moveLater = [safe] (int i) { if (safe != nullptr) safe->sectionSwap (i); };
    juce::CallOutBox::launchAsynchronously (std::make_unique<SectionsPanel> (std::move (hooks)), sectionsButton.getScreenBounds(), nullptr);
}

void MainComponent::sectionGoTo (int i)
{
    const auto& s = engine.getSections();
    if (! juce::isPositiveAndBelow (i, (int) s.size()))
        return;
    engine.setPositionSeconds (tempoMap.barBeatToSeconds (s[(size_t) i].startBar, 0.0));
    repaint();
}

void MainComponent::sectionLoop (int i)
{
    const auto& s = engine.getSections();
    if (! juce::isPositiveAndBelow (i, (int) s.size()))
        return;
    setLoopRegion (tempoMap.barBeatToSeconds (s[(size_t) i].startBar, 0.0), tempoMap.barBeatToSeconds (s[(size_t) i].startBar + s[(size_t) i].bars, 0.0));
}

// A copy of the section right after it, with all that plays in it; what comes later moves along (tempo changes stay where they are).
void MainComponent::sectionCopy (int i)
{
    auto s = engine.getSections();
    if (! juce::isPositiveAndBelow (i, (int) s.size()))
        return;
    const auto section = s[(size_t) i];
    const int endBar = section.startBar + section.bars;
    const double a = tempoMap.barBeatToSeconds (section.startBar, 0.0), b = tempoMap.barBeatToSeconds (endBar, 0.0);

    beginEdit();
    engine.shiftClipsFrom (b, b - a);
    engine.copyRange (a, b, b);
    for (auto& other : s)
        if (other.startBar >= endBar)
            other.startBar += section.bars;
    ArrSection copy = section;
    copy.startBar = endBar;
    copy.name += " 2";
    s.insert (s.begin() + i + 1, copy);
    engine.setSections (std::move (s));
    clipsChanged();
}

// The section (and what plays in it, on every track) onto the clipboard: for this project or another one.
void MainComponent::sectionToClipboard (int i)
{
    const auto& s = engine.getSections();
    if (! juce::isPositiveAndBelow (i, (int) s.size()))
        return;
    sectionClipboard.section = s[(size_t) i];
    sectionClipboard.bundle = engine.captureRange (tempoMap.barBeatToSeconds (s[(size_t) i].startBar, 0.0),
                                                   tempoMap.barBeatToSeconds (s[(size_t) i].startBar + s[(size_t) i].bars, 0.0));
    sectionClipboard.valid = true;
}

// The section from the clipboard at the bar of the playhead (what plays there is not moved away: it plays along). Tracks that are not here
// (by name) are made. The audio stays the same files, also when it comes from another project.
void MainComponent::sectionPaste()
{
    if (! sectionClipboard.valid)
        return;
    const int bar = tempoMap.barAtSeconds (engine.getPositionSeconds());
    const double dest = tempoMap.barBeatToSeconds (bar, 0.0);

    beginEdit();
    int created = 0;
    const int made = engine.pasteRange (sectionClipboard.bundle, dest, false, &created);
    auto s = engine.getSections();
    ArrSection pasted = sectionClipboard.section;
    pasted.startBar = bar;
    s.push_back (pasted);
    std::stable_sort (s.begin(), s.end(), [] (const ArrSection& a, const ArrSection& b) { return a.startBar < b.startBar; });
    engine.setSections (std::move (s));
    if (created > 0 || made > 0)
        rebuildTrackUI();
    clipsChanged();
}

// Section `i` and the one after it change places (they must touch); the clips of each go with it.
void MainComponent::sectionSwap (int i)
{
    auto s = engine.getSections();
    if (i < 0 || i + 1 >= (int) s.size())
        return;
    auto& first = s[(size_t) i];
    auto& second = s[(size_t) i + 1];
    if (first.startBar + first.bars != second.startBar)
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "The sections do not touch",
                                                "To swap two sections the second one must start where the first one ends. Change the bars in the list first.");
        return;
    }
    const double a0 = tempoMap.barBeatToSeconds (first.startBar, 0.0), a1 = tempoMap.barBeatToSeconds (second.startBar, 0.0),
                 b1 = tempoMap.barBeatToSeconds (second.startBar + second.bars, 0.0);
    beginEdit();
    engine.swapRanges (a0, a1, a1, b1);
    const int start = first.startBar, firstBars = first.bars, secondBars = second.bars;
    std::swap (first, second);
    first.startBar = start;
    second.startBar = start + secondBars;
    juce::ignoreUnused (firstBars);
    engine.setSections (std::move (s));
    clipsChanged();
}

void MainComponent::openTempoPanel()
{
    auto panel = std::make_unique<TempoPanel> (tempoMap, [this] { beginEdit(); }, [this] (const TempoMap& m)
    {
        applyTempoMap (m);
        bpmSlider.setValue (m.getSections().front().bpm, juce::dontSendNotification);
    });
    juce::CallOutBox::launchAsynchronously (std::move (panel), tempoButton.getScreenBounds(), nullptr);
}

void MainComponent::selectClip (Track* t, int id)
{
    if (id != envelopeClipId)
    {
        envelopeClipId = -1;   // editing a volume curve ends when another clip is selected
        envelopeTrack = nullptr;
    }
    selTrack = t;
    selClip = id;

    if (t != nullptr && id >= 0 && engine.isMidiClip (*t, id))
    {
        for (int i = 0; i < engine.getNumTracks(); ++i)   // working on a MIDI clip: the next recording is MIDI
            engine.getTrack (i)->recordArmed = false;
        showRoll (t, id);
    }
    else if (rollVisible)
        roll->showClip (roll->getTrack(), roll->getClipId());   // also drops the selected note

    updateArmedTrack();
    repaint();
}

void MainComponent::enableAllMidiInputs()
{
    juce::StringArray names;
    for (auto& d : juce::MidiInput::getAvailableDevices())
    {
        deviceManager.setMidiInputDeviceEnabled (d.identifier, true);
        if (deviceManager.isMidiInputDeviceEnabled (d.identifier))
            names.add (d.name);
    }
    midiInputText = names.joinIntoString (", ");
    updateArmedTrack();
}

// The track that live notes (and MIDI recording) go to.
void MainComponent::updateArmedTrack()
{
    Track* t = nullptr;
    if (selTrack != nullptr)
    {
        if (selTrack->type == Track::Type::Midi)
            t = selTrack;
    }
    else if (rollVisible && roll->getTrack() != nullptr)
    {
        t = roll->getTrack();
    }
    armedTrack = t;
    refreshLiveTargets();

    statusLabel.setText (audioInfoText + "\n"
                             + (statusLabel.getWidth() < 300 ? "MIDI " : statusLabel.getWidth() < 460 ? "MIDI: " : "MIDI in: ") + (midiInputText.isEmpty() ? juce::String ("none") : midiInputText)
                             + "    Armed: " + (t != nullptr ? t->name : juce::String (statusLabel.getWidth() < 460 ? "none" : "none (select a MIDI clip)")),
                         juce::dontSendNotification);
}

//==============================================================================
// Remembered interface settings

juce::File MainComponent::uiSettingsFile()
{
    auto dir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("UFODAW");
    dir.createDirectory();
    return dir.getChildFile ("ui-settings.xml");
}

void MainComponent::loadUiSettings()
{
    if (auto xml = juce::XmlDocument::parse (uiSettingsFile()))
    {
        UiSettings::panSnap = xml->getBoolAttribute ("panSnap", true);
        UiSettings::countInBars = juce::jlimit (0, 4, xml->getIntAttribute ("countInBars", 0));
        UiSettings::follow = xml->getBoolAttribute ("follow", true);
        UiSettings::exportChoice = juce::jlimit (0, (int) AudioExport::choices().size() - 1, xml->getIntAttribute ("exportChoice", 0));
        UiSettings::autoUpdateCheck = xml->getBoolAttribute ("autoUpdateCheck", true);
        UiSettings::lastUpdateCheck = (juce::int64) xml->getDoubleAttribute ("lastUpdateCheck", 0.0);
        UiSettings::tourDone = xml->getBoolAttribute ("tourDone", true);   // (settings from before the tour existed: no tour)
        UiSettings::latencyMode = juce::jlimit (0, 2, xml->getIntAttribute ("latencyMode", 0));
        UiSettings::latencyMeasuredMs = xml->getDoubleAttribute ("latencyMeasuredMs", 0.0);
        UiSettings::latencyExtraMs = xml->getDoubleAttribute ("latencyExtraMs", 0.0);
        UiSettings::clockOffsetMs = xml->getDoubleAttribute ("clockOffsetMs", 0.0);
        UiSettings::clockDevices.clear();
        for (auto* e : xml->getChildWithTagNameIterator ("CLOCKOUT"))
            UiSettings::clockDevices.add (e->getStringAttribute ("id"));
    }
    updateCountInButton();
    engine.setClockOffsetMs (UiSettings::clockOffsetMs);
    engine.setClockOutputs (UiSettings::clockDevices);   // the hardware that follows UFODAW's tempo
}

void MainComponent::saveUiSettings()
{
    juce::XmlElement xml ("UISETTINGS");
    xml.setAttribute ("panSnap", UiSettings::panSnap);
    xml.setAttribute ("countInBars", UiSettings::countInBars);
    xml.setAttribute ("follow", UiSettings::follow);
    xml.setAttribute ("exportChoice", UiSettings::exportChoice);
    xml.setAttribute ("tourDone", UiSettings::tourDone);
    xml.setAttribute ("autoUpdateCheck", UiSettings::autoUpdateCheck);
    xml.setAttribute ("lastUpdateCheck", (double) UiSettings::lastUpdateCheck);
    xml.setAttribute ("latencyMode", UiSettings::latencyMode);
    xml.setAttribute ("latencyMeasuredMs", UiSettings::latencyMeasuredMs);
    xml.setAttribute ("latencyExtraMs", UiSettings::latencyExtraMs);
    xml.setAttribute ("clockOffsetMs", UiSettings::clockOffsetMs);
    for (auto& id : UiSettings::clockDevices)
        xml.createNewChildElement ("CLOCKOUT")->setAttribute ("id", id);
    xml.writeTo (uiSettingsFile());
}

// How much earlier a recording is placed on the timeline: the round trip through the interface, plus the extra offset.
double MainComponent::recordLatencySeconds() const
{
    double ms = 0.0;
    if (UiSettings::latencyMode == 0)
    {
        if (auto* device = deviceManager.getCurrentAudioDevice())
            ms = (double) (device->getInputLatencyInSamples() + device->getOutputLatencyInSamples()) * 1000.0
                 / juce::jmax (1.0, device->getCurrentSampleRate());
    }
    else if (UiSettings::latencyMode == 1)
    {
        ms = UiSettings::latencyMeasuredMs;
    }
    return (ms + UiSettings::latencyExtraMs) / 1000.0;
}

void MainComponent::updateCountInButton()
{
    countInButton.setButtonText (UiSettings::countInBars == 0 ? juce::String ("Count-in: off")
                                                              : "Count-in: " + juce::String (UiSettings::countInBars) + (UiSettings::countInBars == 1 ? " bar" : " bars"));
    countInButton.setColour (juce::TextButton::buttonColourId, UiSettings::countInBars > 0 ? Theme::orangeDim : Theme::buttonOff);
}

//==============================================================================
// Audio device

juce::File MainComponent::audioSettingsFile()
{
    auto dir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("UFODAW");
    dir.createDirectory();
    return dir.getChildFile ("audio-device.xml");
}

// Any change in the audio setup (driver, device, rate, buffer, channels) is remembered for the next start.
void MainComponent::changeListenerCallback (juce::ChangeBroadcaster*)
{
    if (auto xml = deviceManager.createStateXml())
        xml->writeTo (audioSettingsFile());
    updateAudioInfo();
}

void MainComponent::updateAudioInfo()
{
    if (auto* device = deviceManager.getCurrentAudioDevice())
    {
        const double rate = device->getCurrentSampleRate();
        const int buffer = device->getCurrentBufferSizeSamples();
        // MIDI to hardware leaves this much later, so the analog synth plays in time with what is heard from the speakers
        engine.setOutputLatencyMs ((double) device->getOutputLatencyInSamples() * 1000.0 / juce::jmax (1.0, rate));
        const bool compact = statusLabel.getWidth() < 460, tiny = statusLabel.getWidth() < 300;   // a narrow window: the device name, then the driver, is left out
        audioInfoText = (tiny ? juce::String() : "Audio: " + device->getTypeName() + (compact ? juce::String() : " - " + device->getName()) + "  |  ")
                        + juce::String (rate / 1000.0, 1) + " kHz, " + (compact ? juce::String() : juce::String (buffer) + " smp (")
                        + juce::String (buffer * 1000.0 / juce::jmax (1.0, rate), 1) + (compact ? " ms  |  " : " ms), ")
                        + juce::String (device->getActiveInputChannels().countNumberOfSetBits()) + " in/"
                        + juce::String (device->getActiveOutputChannels().countNumberOfSetBits()) + " out"
                        + "  |  CPU " + juce::String (juce::jmax (0, cpuPercentShown)) + " %";
    }
    else
    {
        audioInfoText = "Audio: no device - click Audio... to choose one";
    }
    updateArmedTrack();   // rebuilds the status text
}

void MainComponent::openAudioSettings()
{
    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned (new AudioSettingsPanel (deviceManager, engine, [this] { saveUiSettings(); }));
    options.dialogTitle = "Audio settings";
    options.dialogBackgroundColour = Theme::panel;
    options.componentToCentreAround = this;
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = false;
    options.launchAsync();
}

void MainComponent::MidiForwarder::handleIncomingMidiMessage (juce::MidiInput* source, const juce::MidiMessage& m)
{
    owner.routeLiveMidi (source != nullptr ? source->getIdentifier() : juce::String(), m);
}

// MIDI thread. A message from one input goes to every target that listens to that input (and channel).
void MainComponent::routeLiveMidi (const juce::String& sourceId, const juce::MidiMessage& m)
{
    if (m.isNoteOnOrOff() && juce::isPositiveAndBelow (m.getNoteNumber(), 128))   // which keys are down, so the piano roll can light them
    {
        auto& mask = m.getNoteNumber() < 64 ? liveMaskLow : liveMaskHigh;
        const unsigned long long bit = 1ull << (m.getNoteNumber() & 63);
        if (m.isNoteOn()) mask.fetch_or (bit); else mask.fetch_and (~bit);
    }

    if (m.isController())   // for MIDI learn: what every controller says now (and which one moved last)
    {
        const int index = (m.getChannel() - 1) * 128 + m.getControllerNumber();
        if (juce::isPositiveAndBelow (index, 16 * 128))
        {
            ccValue[(size_t) index].store (m.getControllerValue());
            ccStamp[(size_t) index].fetch_add (1u);
            if (ccLearning.load())
            {
                ccLearnedChannel = m.getChannel();
                ccLearnedNumber = m.getControllerNumber();
                ccLearning = false;
            }
        }
    }

    if (! (m.isNoteOnOrOff() || m.isController() || m.isPitchWheel() || m.isChannelPressure()))
        return;

    std::vector<LiveTarget> targets;
    {
        const juce::ScopedLock sl (liveTargetsLock);
        targets = liveTargets;
    }
    for (auto& t : targets)
    {
        if (t.deviceId.isNotEmpty() && t.deviceId != sourceId)
            continue;
        if (t.channel > 0 && m.getChannel() != t.channel)
            continue;
        liveEvent (t.track, m);
    }
}

// The computer keyboard: plays the selected MIDI track.
void MainComponent::liveNote (int pitch, bool on, float velocity)
{
    if (juce::isPositiveAndBelow (pitch, 128))
    {
        auto& mask = pitch < 64 ? liveMaskLow : liveMaskHigh;
        const unsigned long long bit = 1ull << (pitch & 63);
        if (on) mask.fetch_or (bit); else mask.fetch_and (~bit);
    }

    if (auto* t = armedTrack.load())
        liveEvent (t, on ? juce::MidiMessage::noteOn (1, pitch, velocity) : juce::MidiMessage::noteOff (1, pitch));
}

// May run on the MIDI thread: plays the message on the track and, while recording, remembers it.
void MainComponent::liveEvent (Track* t, const juce::MidiMessage& m)
{
    if (t == nullptr)
        return;

    int controller = -1;
    float value = 0.0f;
    if (m.isController())          { controller = m.getControllerNumber(); value = (float) m.getControllerValue() / 127.0f; }
    else if (m.isPitchWheel())     { controller = CcEvent::kPitchBend;     value = (float) m.getPitchWheelValue() / 16383.0f; }
    else if (m.isChannelPressure()) { controller = CcEvent::kAftertouch;    value = (float) m.getChannelPressureValue() / 127.0f; }

    if (m.isNoteOnOrOff())
        engine.previewNote (*t, m.getNoteNumber(), m.isNoteOn(), m.getFloatVelocity());
    else if (controller >= 0)
        engine.previewController (*t, controller, value);

    if (! midiRecording || engine.isCountingIn())   // (what is played during the count-in is heard, not recorded)
        return;

    const double now = engine.getPositionSeconds();
    const juce::ScopedLock sl (midiRecLock);
    MidiRec* rec = nullptr;
    for (auto& r : midiRecs)
        if (r.track == t)
            rec = &r;
    if (rec == nullptr)
        return;

    if (controller >= 0)
    {
        // thin out dense streams (a pitch wheel sends hundreds of values a second): keep one every 10 ms, or a bigger jump
        const auto found = rec->lastCc.find (controller);
        if (found == rec->lastCc.end() || now - found->second.first >= 0.01 || std::abs (value - found->second.second) >= 0.02f)
        {
            rec->ccs.push_back ({ now, controller, value });
            rec->lastCc[controller] = { now, value };
        }
        return;
    }

    if (m.isNoteOn())
    {
        rec->held.push_back ({ m.getNoteNumber(), now, m.getFloatVelocity() });
        return;
    }
    for (auto it = rec->held.begin(); it != rec->held.end(); ++it)
        if (it->pitch == m.getNoteNumber())
        {
            Note n;
            n.pitch = m.getNoteNumber();
            n.start = it->start;
            n.length = juce::jmax (0.02, now - it->start);
            n.velocity = it->velocity;
            rec->notes.push_back (n);
            rec->held.erase (it);
            return;
        }
}

// Rebuilds the list of tracks that live MIDI goes to; cheap, called on the timer and whenever the selection changes.
void MainComponent::refreshLiveTargets()
{
    std::vector<LiveTarget> targets;
    auto add = [&] (Track* t)
    {
        if (t == nullptr || t->type != Track::Type::Midi)
            return;
        for (auto& existing : targets)
            if (existing.track == t)
                return;
        targets.push_back ({ t, t->midiInputId, t->midiInputChannel });
    };
    add (armedTrack.load());   // the selected MIDI track listens (as before), through its own input choice
    for (int i = 0; i < engine.getNumTracks(); ++i)
        if (auto* t = engine.getTrack (i); t != nullptr && t->midiArmed)
            add (t);

    const juce::ScopedLock sl (liveTargetsLock);
    const auto same = liveTargets.size() == targets.size()
                      && std::equal (targets.begin(), targets.end(), liveTargets.begin(), [] (auto& a, auto& b)
                                     { return a.track == b.track && a.deviceId == b.deviceId && a.channel == b.channel; });
    if (! same)
        liveTargets = std::move (targets);
}

void MainComponent::showRoll (Track* t, int clipId)
{
    // drum tracks get the step sequencer, every other MIDI track the piano roll
    RollPanel* wanted = (t != nullptr && t->kit != nullptr) ? static_cast<RollPanel*> (drumRoll.get())
                                                           : static_cast<RollPanel*> (pianoRoll.get());
    const bool wasVisible = rollVisible;
    const bool panelChanged = wanted != roll;
    roll = wanted;
    rollVisible = true;
    roll->showClip (t, clipId);
    if (! wasVisible || panelChanged)
        layoutContent();

    // the panel takes space from the lanes: make sure the clip's own track stays in view
    for (int i = 0; i < lanes.size(); ++i)
        if (&lanes[i]->getTrack() == t)
        {
            auto pos = laneViewport.getViewPosition();
            const int top = i < (int) trackTops.size() ? trackTops[(size_t) i] : i * stripHeight, bottom = top + stripHeight, visible = laneViewport.getMaximumVisibleHeight();
            if (top < pos.y)                   pos.y = top;
            else if (bottom > pos.y + visible) pos.y = juce::jmax (0, bottom - visible);
            laneViewport.setViewPosition (pos);
        }
}

void MainComponent::hideRoll()
{
    if (! rollVisible)
        return;
    rollVisible = false;
    layoutContent();
    updateArmedTrack();
}

void MainComponent::addMidiTrack (bool drums)
{
    int count = 1;
    for (int i = 0; i < engine.getNumTracks(); ++i)
        if (auto* other = engine.getTrack (i); other->type == Track::Type::Midi && (other->kit != nullptr) == drums)
            ++count;

    beginEdit();
    auto* t = drums ? engine.addDrumTrack ("Drums " + juce::String (count))
                    : engine.addTrack ("Synth " + juce::String (count), Track::Type::Midi);
    const int barIndex = tempoMap.barAtSeconds (engine.getPositionSeconds());
    const double start = tempoMap.barStartSeconds (barIndex);
    const int id = engine.addMidiClip (*t, start, tempoMap.barStartSeconds (barIndex + 2) - start);
    addTrackUI (*t);
    selectClip (t, id);   // opens the piano roll
    clipsChanged();
}

Track* MainComponent::trackAtY (int y) const
{
    if (y < 0)
        return nullptr;
    for (int i = 0; i < lanes.size() && i < (int) trackTops.size(); ++i)
    {
        int height = stripHeight;
        if (auto& t = lanes[i]->getTrack(); t.subLanesOpen)
            height += subLaneHeight * (int) t.subLanes.size() + (t.subLanes.empty() ? 0 : subFooterHeight);   // (a track's sub-tracks belong to it)
        if (y >= trackTops[(size_t) i] && y < trackTops[(size_t) i] + height)
            return &lanes[i]->getTrack();
    }
    return nullptr;
}

void MainComponent::setLoopRegion (double a, double b)
{
    if (b - a < tempoMap.beatSecondsAt (a) * 0.5)
        return;
    engine.loopStart = a;
    engine.loopEnd = b;
    engine.loopEnabled = true;
    loopButton.setToggleState (true, juce::dontSendNotification);
    ruler->repaint();
    repaint();
}

// A new sampler track (Simpler-style): an empty sample to load, with a two-bar MIDI clip to play it from.
void MainComponent::addSamplerTrack()
{
    int count = 1;
    for (int i = 0; i < engine.getNumTracks(); ++i)
        if (engine.getTrack (i)->sampler != nullptr)
            ++count;

    beginEdit();
    auto* t = engine.addSamplerTrack ("Sampler " + juce::String (count));
    const int barIndex = tempoMap.barAtSeconds (engine.getPositionSeconds());
    const double start = tempoMap.barStartSeconds (barIndex);
    const int id = engine.addMidiClip (*t, start, tempoMap.barStartSeconds (barIndex + 2) - start);
    addTrackUI (*t);
    selectClip (t, id);
    clipsChanged();
    openSampler (*t);   // to load a sample
}

// The sampler's Slice mode as a pattern: a MIDI clip on the sampler's track at the playhead (on the grid), one note per slice from the
// slice key up, in the timing of the sample. The clip is a whole number of bars long. One undo step.
void MainComponent::slicesToMidi (Track& t)
{
    if (t.sampler == nullptr || engine.indexOfTrack (&t) < 0)
        return;
    const auto times = t.sampler->sliceTimes();
    if (times.empty())
        return;

    const double start = snapTime (engine.getPositionSeconds());
    const double span = times.back().first + juce::jmax (0.05, times.back().second);
    const double bar = tempoMap.barSecondsAt (start);
    const double length = juce::jmax (bar, std::ceil (span / bar - 0.02) * bar);

    beginEdit();
    const int id = engine.addMidiClip (t, start, length);
    std::vector<Note> notes;
    const int firstNote = t.sampler->params.sliceNote.load();
    for (size_t i = 0; i < times.size(); ++i)
    {
        if (firstNote + (int) i > 127)
            break;
        Note n;
        n.pitch = firstNote + (int) i;
        n.start = times[i].first;
        n.length = juce::jmax (0.02, times[i].second);
        n.velocity = 0.85f;
        notes.push_back (n);
    }
    engine.setMidiNotes (t, id, std::move (notes));
    clipsChanged();
}

// The tempo of the first section (what the BPM slider shows), e.g. from the sampler's tempo calculator. One undo step.
// Tap tempo: tap in time (also with the key T); the tempo follows the average of the last taps. Two seconds without a tap start over.
void MainComponent::tapTempo()
{
    const auto now = juce::Time::getMillisecondCounter();
    if (tapTimes.empty() || now - tapTimes.back() > 2500u)
        tapTimes.clear();
    tapTimes.push_back (now);
    if (tapTimes.size() > 8)
        tapTimes.erase (tapTimes.begin());
    if (tapTimes.size() < 2)
        return;
    const double meanMs = (double) (tapTimes.back() - tapTimes.front()) / (double) (tapTimes.size() - 1);
    const double bpm = std::round (juce::jlimit (20.0, 400.0, 60000.0 / meanMs) * 10.0) / 10.0;
    setFirstTempo (bpm, tapTimes.size() == 2);   // (one undo step for all the taps of a go)
}

void MainComponent::setFirstTempo (double bpm, bool withUndo)
{
    if (withUndo)
        beginEdit();
    auto sections = tempoMap.getSections();
    sections[0].bpm = bpm;
    TempoMap m;
    m.setSections (sections);
    applyTempoMap (m);
    bpmSlider.setValue (m.getSections().front().bpm, juce::dontSendNotification);
}

void MainComponent::openSampler (Track& t)
{
    if (t.sampler == nullptr)
        return;
    samplerWindow.reset();   // one at a time
    const TrackRef tr (&t);
    juce::Component::SafePointer<MainComponent> safe (this);
    samplerRef = tr;
    samplerWindow = std::make_unique<SamplerWindow> ("Sampler - " + t.name, *t.sampler,
        [safe, tr] (const juce::File& f) { return safe != nullptr && safe->engine.loadSamplerSample (*tr, f); },
        [safe] { if (safe != nullptr) ++safe->editCounter; },   // a change counts as an edit of the project (autosave, unsaved changes)
        [safe, tr] (bool down)                                   // the Play button: the root note, held
        {
            if (safe != nullptr && safe->engine.indexOfTrack (tr) >= 0 && tr->sampler != nullptr)
                safe->engine.previewNote (*tr, tr->sampler->params.rootNote.load(), down, 0.9f);
        },
        [safe] { juce::MessageManager::callAsync ([safe] { if (safe != nullptr) safe->samplerWindow.reset(); }); },
        true,
        [safe, tr] (double ratio, std::function<void (bool)> done)
        {
            if (safe != nullptr && tr->sampler != nullptr)
                safe->startSamplerStretch (tr->sampler, ratio, std::move (done));
            else if (done)
                done (false);
        });
    samplerWindow->getPanel().onBeginEdit = [safe] { if (safe != nullptr) safe->beginEdit(); };
    samplerWindow->getPanel().onAbortEdit = [safe] { if (safe != nullptr) safe->cancelEdit(); };
    samplerWindow->getPanel().onSlicesToMidi = [safe, tr] { if (safe != nullptr && safe->engine.indexOfTrack (tr) >= 0) safe->slicesToMidi (*tr); };
    samplerWindow->getPanel().projectBpm = [safe] { return safe != nullptr ? safe->tempoMap.getSections().front().bpm : 120.0; };
    samplerWindow->getPanel().setProjectBpm = [safe] (double bpm) { if (safe != nullptr) safe->setFirstTempo (bpm); };
    samplerWindow->onKey = [safe] (const juce::KeyPress& k) { return safe != nullptr && safe->handleUndoKey (k); };
}

void MainComponent::openPadSampler (Track& t, int pad)
{
    if (t.kit == nullptr || ! juce::isPositiveAndBelow (pad, DrumKit::numPads) || ! t.kit->hasSample (pad))
        return;
    padSamplerWindow.reset();
    const TrackRef tr (&t);
    juce::Component::SafePointer<MainComponent> safe (this);
    padRef = tr;
    padWindowPad = pad;
    padSamplerWindow = std::make_unique<SamplerWindow> (t.name + " - " + t.kit->getPad (pad).name, t.kit->getPad (pad).sampler,
        [safe, tr, pad] (const juce::File& f) { return safe != nullptr && safe->engine.loadDrumSample (*tr, pad, f); },
        [safe] { if (safe != nullptr) ++safe->editCounter; },
        [safe, tr, pad] (bool down)
        {
            if (safe != nullptr && safe->engine.indexOfTrack (tr) >= 0 && tr->kit != nullptr)
                safe->engine.previewNote (*tr, tr->kit->getPad (pad).note, down, 0.9f);
        },
        [safe] { juce::MessageManager::callAsync ([safe] { if (safe != nullptr) safe->padSamplerWindow.reset(); }); },
        false,   // (no slice mode on a pad)
        [safe, tr, pad] (double ratio, std::function<void (bool)> done)
        {
            if (safe != nullptr && tr->kit != nullptr)
                safe->startSamplerStretch (std::shared_ptr<Sampler> (tr->kit, &tr->kit->getPad (pad).sampler), ratio, std::move (done));   // (keeps the whole kit alive)
            else if (done)
                done (false);
        });
    padSamplerWindow->getPanel().onBeginEdit = [safe] { if (safe != nullptr) safe->beginEdit(); };
    padSamplerWindow->getPanel().onAbortEdit = [safe] { if (safe != nullptr) safe->cancelEdit(); };
    padSamplerWindow->getPanel().projectBpm = [safe] { return safe != nullptr ? safe->tempoMap.getSections().front().bpm : 120.0; };
    padSamplerWindow->getPanel().setProjectBpm = [safe] (double bpm) { if (safe != nullptr) safe->setFirstTempo (bpm); };
    padSamplerWindow->onKey = [safe] (const juce::KeyPress& k) { return safe != nullptr && safe->handleUndoKey (k); };
}

void MainComponent::openMixer()
{
    if (mixerWindow != nullptr)
    {
        mixerWindow->toFront (true);
        return;
    }
    juce::Component::SafePointer<MainComponent> safe (this);
    mixerWindow = std::make_unique<MixerWindow> (engine, [safe] { if (safe != nullptr) safe->beginEdit(); },
                                                 [safe] { juce::MessageManager::callAsync ([safe] { if (safe != nullptr) safe->mixerWindow.reset(); }); });
    mixerWindow->onKey = [safe] (const juce::KeyPress& k) { return safe != nullptr && safe->handleUndoKey (k); };
}

void MainComponent::openDrumRack (Track& t, int selectPad)
{
    if (t.kit == nullptr)
        return;
    if (drumRackWindow != nullptr && drumRackWindow->getTitle() == t.name + " - drum rack")
    {
        if (selectPad >= 0)
            drumRackWindow->getPanel().select (selectPad);
        drumRackWindow->toFront (true);
        return;
    }
    padSamplerWindow.reset();
    drumRackWindow.reset();
    const TrackRef tr (&t);
    juce::Component::SafePointer<MainComponent> safe (this);
    DrumRackPanel::Hooks hooks;
    hooks.preview = [safe, tr] (int note, bool down)
    {
        if (safe != nullptr && safe->engine.indexOfTrack (tr) >= 0)
            safe->engine.previewNote (*tr, note, down, 0.9f);
    };
    hooks.load = [safe, tr] (int pad, const juce::File& f) { return safe != nullptr && safe->engine.loadDrumSample (*tr, pad, f); };
    hooks.loadLayer = [safe, tr] (int pad, int layer, const juce::File& f) { return safe != nullptr && safe->engine.loadDrumLayer (*tr, pad, layer, f); };
    hooks.clearLayer = [safe, tr] (int pad, int layer) { if (safe != nullptr) safe->engine.clearDrumLayer (*tr, pad, layer); };
    hooks.reset = [safe, tr] (int pad) { if (safe != nullptr) safe->engine.resetDrumPad (*tr, pad); };
    hooks.editSampler = [safe, tr] (int pad) { if (safe != nullptr) safe->openPadSampler (*tr, pad); };
    hooks.beginEdit = [safe] { if (safe != nullptr) safe->beginEdit(); };
    hooks.changed = [safe] { if (safe != nullptr) { ++safe->editCounter; if (safe->drumRoll != nullptr) safe->drumRoll->repaint(); } };
    rackRef = tr;
    drumRackWindow = std::make_unique<DrumRackWindow> (t.name + " - drum rack", *t.kit, std::move (hooks),
        [safe] { juce::MessageManager::callAsync ([safe] { if (safe != nullptr) { safe->padSamplerWindow.reset(); safe->drumRackWindow.reset(); } }); });
    drumRackWindow->onKey = [safe] (const juce::KeyPress& k) { return safe != nullptr && safe->handleUndoKey (k); };
    if (selectPad >= 0)
        drumRackWindow->getPanel().select (selectPad);
}

// "Sample this clip": the part of an audio clip that plays becomes the sample of a new sampler track, with one note that plays it.
void MainComponent::sampleClip (Track& t, int clipId)
{
    Clip c;
    if (! engine.getClip (t, clipId, c) || c.audio == nullptr)
        return;

    const int from = juce::jlimit (0, c.audio->getNumSamples(), (int) std::llround (c.offset * c.fileRate));
    const int to = juce::jlimit (from, c.audio->getNumSamples(), (int) std::llround ((c.offset + c.length) * c.fileRate));
    if (to - from < 2)
        return;

    auto buffer = std::make_shared<juce::AudioBuffer<float>> (c.audio->getNumChannels(), to - from);
    for (int ch = 0; ch < buffer->getNumChannels(); ++ch)
        buffer->copyFrom (ch, 0, *c.audio, ch, from, to - from);
    if (c.reverse)
        buffer->reverse (0, buffer->getNumSamples());

    // the sample is kept as a file (so it is saved with the project and found again): in the recordings folder
    const auto sampleName = c.sourceFile != juce::File() ? c.sourceFile.getFileNameWithoutExtension() : juce::String ("Sampled clip");
    const auto folder = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("UFODAW Recordings").getChildFile ("Samples");
    folder.createDirectory();
    const auto sampleFile = folder.getChildFile (juce::File::createLegalFileName (sampleName) + " " + juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H-%M-%S") + ".wav");
    if (auto stream = sampleFile.createOutputStream())
    {
        juce::WavAudioFormat wav;
        if (std::unique_ptr<juce::AudioFormatWriter> writer (wav.createWriterFor (stream.get(), c.fileRate, (unsigned int) buffer->getNumChannels(), 24, {}, 0)); writer != nullptr)
        {
            stream.release();
            writer->writeFromAudioSampleBuffer (*buffer, 0, buffer->getNumSamples());
        }
    }

    beginEdit();
    auto* s = engine.addSamplerTrack (sampleName);
    engine.setSamplerSample (*s, buffer, c.fileRate, sampleName, sampleFile);
    s->sampler->params.mode = Sampler::OneShot;   // it plays out whole, like the clip
    addTrackUI (*s);

    const int barIndex = tempoMap.barAtSeconds (c.start);
    const double clipStart = tempoMap.barStartSeconds (barIndex);
    const double clipLength = juce::jmax (tempoMap.barStartSeconds (barIndex + 1) - clipStart, c.start - clipStart + c.length);
    const int id = engine.addMidiClip (*s, clipStart, clipLength);
    Note n;
    n.pitch = s->sampler->params.rootNote.load();
    n.start = c.start - clipStart;
    n.length = c.length;
    n.velocity = 1.0f;
    engine.setMidiNotes (*s, id, { n });
    selectClip (s, id);
    clipsChanged();
}

void MainComponent::openInstrumentEditor (Track& t)
{
    if (t.sampler != nullptr)
        openSampler (t);
    else if (t.kit != nullptr)
        openDrumRack (t);
    else if (t.instrument != nullptr)
        pluginHost.showEditor (*t.instrument);
    else if (! t.inserts.empty())
        pluginHost.showEditor (*t.inserts.front());
    else
        for (auto* header : headers)   // no plugin yet: show the panel where one can be chosen
            if (&header->getTrack() == &t)
                openPlugins (t, *header);
}

void MainComponent::openPlugins (Track& t, juce::Component& anchor)
{
    juce::Component::SafePointer<MainComponent> safe (this);
    const TrackRef ref (&t);
    juce::CallOutBox::launchAsynchronously (std::make_unique<PluginPanel> (t, engine, pluginHost, [this] { beginEdit(); },
                                                                            [safe, ref]
                                                                            {
                                                                                if (safe != nullptr && safe->engine.indexOfTrack (ref) >= 0)
                                                                                    if (ref->sampler != nullptr || ref->kit != nullptr)
                                                                                        safe->openInstrumentEditor (*ref);
                                                                            }),
                                            anchor.getScreenBounds(), nullptr);
}

// The "A" menu of a track header: which envelope to edit in the lane, and housekeeping.
void MainComponent::openAutomation (Track& t, juce::Component& anchor)
{
    auto* track = &t;
    auto actions = std::make_shared<std::vector<std::function<void()>>>();   // menu item id - 1 -> what to do
    auto add = [&] (juce::PopupMenu& m, const juce::String& text, bool enabled, bool ticked, std::function<void()> action)
    {
        actions->push_back (std::move (action));
        m.addItem ((int) actions->size(), text, enabled, ticked);
    };
    auto edit = [this, track] (const juce::String& id)
    {
        return [this, track, id] { track->autoEditId = id; };
    };

    juce::PopupMenu menu;

    // what can be edited: volume, pan, then the built-in effects/synth and every hosted plugin's parameters
    add (menu, "Volume", true, t.autoEditId == "volume", edit ("volume"));
    if (! engine.isMaster (t))   // the master channel has no pan
        add (menu, "Pan", true, t.autoEditId == "pan", edit ("pan"));

    std::map<juce::String, juce::PopupMenu> groups;
    std::vector<juce::String> groupOrder;
    for (auto& target : engine.listAutomationTargets (t))
    {
        if (groups.find (target.group) == groups.end())
            groupOrder.push_back (target.group);
        add (groups[target.group], target.name, true, t.autoEditId == target.id, edit (target.id));
    }
    menu.addSeparator();
    for (auto& group : groupOrder)
        menu.addSubMenu ("Automate: " + group, groups[group]);

    // what already has envelopes: switch it on/off, clear it
    const auto existing = engine.getAutomationIds (t);
    if (! existing.empty())
    {
        menu.addSeparator();
        for (auto& id : existing)
        {
            const auto info = engine.getAutomationInfo (t, id);
            juce::PopupMenu sub;
            add (sub, "Edit", true, t.autoEditId == id, edit (id));
            add (sub, "Active", true, engine.isAutomationActive (t, id), [this, track, id]
            {
                beginEdit();
                engine.setAutomationActive (*track, id, ! engine.isAutomationActive (*track, id));
            });
            add (sub, "Clear", true, false, [this, track, id]
            {
                beginEdit();
                engine.setAutomation (*track, id, {});
                if (track->autoEditId == id)
                    track->autoEditId = {};
            });
            menu.addSubMenu ("Envelope: " + (info.valid ? info.name : id), sub);
        }
    }

    if (t.autoEditId.isNotEmpty())
    {
        // riding: arm the envelope that is being edited, play, and move the control; every pass becomes a take
        menu.addSeparator();
        const auto editId = t.autoEditId;
        AutoRecMode armedMode = AutoRecMode::Touch;
        const bool armed = engine.isAutomationArmed (t, editId, &armedMode);
        juce::PopupMenu rec;
        add (rec, "Touch (records while you hold the control)", true, armed && armedMode == AutoRecMode::Touch, [this, track, editId] { engine.armAutomation (*track, editId, AutoRecMode::Touch); });
        add (rec, "Latch (from your first move until you stop)", true, armed && armedMode == AutoRecMode::Latch, [this, track, editId] { engine.armAutomation (*track, editId, AutoRecMode::Latch); });
        add (rec, "Write (the control's value, the whole time)", true, armed && armedMode == AutoRecMode::Write, [this, track, editId] { engine.armAutomation (*track, editId, AutoRecMode::Write); });
        rec.addSeparator();
        add (rec, "Off", armed, false, [this, track, editId] { engine.disarmAutomation (*track, editId); });
        menu.addSubMenu (armed ? "Record rides: armed" : "Record rides (takes)", rec);
        add (menu, "Comp view (a row per take)", true, t.autoCompView, [track] { track->autoCompView = ! track->autoCompView; });
    }

    if (t.autoEditId.isNotEmpty())
    {
        menu.addSeparator();
        add (menu, "Stop editing automation", true, false, [track] { track->autoEditId = {}; });
    }

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&anchor), [this, track, actions] (int result)
    {
        if (result <= 0 || result > (int) actions->size() || engine.indexOfTrack (track) < 0)
            return;
        (*actions)[(size_t) result - 1]();
        clipsChanged();
        for (auto* lane : lanes)   // (the arm state and the REC mark are drawn in the lanes)
            lane->repaint();
    });
}

// Plugin instrument + inserts of a track, for the project file.
juce::var MainComponent::pluginsToVar (const Track& t) const
{
    auto* o = new juce::DynamicObject();
    if (t.instrument != nullptr)
        o->setProperty ("instrument", pluginHost.slotToVar (*t.instrument));

    juce::Array<juce::var> list;
    for (auto& p : t.inserts)
        list.add (pluginHost.slotToVar (*p));
    o->setProperty ("inserts", list);

    juce::Array<juce::var> midiFxList;
    for (auto& p : t.midiFx)
        midiFxList.add (pluginHost.slotToVar (*p));
    o->setProperty ("midiFx", midiFxList);
    return juce::var (o);
}

void MainComponent::pluginsFromVar (Track& t, const juce::var& trackVar, juce::StringArray& problems)
{
    const auto plugins = trackVar["plugins"];
    if (! plugins.isObject())
        return;

    juce::String error;
    if (plugins.hasProperty ("instrument") && t.type == Track::Type::Midi)
    {
        if (auto slot = pluginHost.slotFromVar (plugins["instrument"], engine, error))
            engine.setInstrument (t, slot);
        else
            problems.add (t.name + " (instrument): " + error);
    }

    if (auto* inserts = plugins["inserts"].getArray())
        for (auto& v : *inserts)
        {
            if (auto slot = pluginHost.slotFromVar (v, engine, error))
                engine.addInsert (t, slot);
            else
                problems.add (t.name + ": " + error);
        }

    if (auto* midiFx = plugins["midiFx"].getArray())
        for (auto& v : *midiFx)
        {
            if (auto slot = pluginHost.slotFromVar (v, engine, error))
                engine.addMidiFx (t, slot);
            else
                problems.add (t.name + " (MIDI effect): " + error);
        }
}

// Effects, plugins and automation of a track (or of the master channel), as they go into the project file.
void MainComponent::writeEffectsAndAutomation (juce::DynamicObject& o, Track& t)
{
    auto* fo = new juce::DynamicObject();
    t.fx.forEachFloat ([fo] (const char* name, std::atomic<float>& v) { fo->setProperty (name, (double) v.load()); });
    t.fx.forEachBool ([fo] (const char* name, std::atomic<bool>& v) { fo->setProperty (name, v.load()); });
    o.setProperty ("fx", juce::var (fo));
    o.setProperty ("plugins", pluginsToVar (t));

    // automation, keyed by envelope id ("volume", "pan", "fx.reverbMix", "plugin:3:12", ...)
    auto* ao = new juce::DynamicObject();
    for (auto& id : engine.getAutomationIds (t))
    {
        juce::Array<juce::var> points;
        for (auto& p : engine.getAutomation (t, id))
        {
            juce::Array<juce::var> pair;
            pair.add (p.time);
            pair.add ((double) p.value);
            points.add (juce::var (pair));
        }
        auto* lo = new juce::DynamicObject();
        lo->setProperty ("active", engine.isAutomationActive (t, id));
        lo->setProperty ("points", points);
        const auto data = engine.getAutomationData (t, id);
        if (! data.takes.empty())   // the rides: every take, which take plays where, and the hand-drawn curve under them
        {
            auto pointsToVar = [] (const std::vector<AutoPoint>& pts)
            {
                juce::Array<juce::var> list;
                for (auto& p : pts)
                {
                    juce::Array<juce::var> pair;
                    pair.add (p.time);
                    pair.add ((double) p.value);
                    list.add (juce::var (pair));
                }
                return list;
            };
            juce::Array<juce::var> takes, comp;
            for (auto& tk : data.takes)
            {
                auto* to = new juce::DynamicObject();
                to->setProperty ("id", tk.id);
                to->setProperty ("name", tk.name);
                to->setProperty ("start", tk.start);
                to->setProperty ("end", tk.end);
                to->setProperty ("points", pointsToVar (tk.points));
                takes.add (juce::var (to));
            }
            for (auto& s : data.comp)
            {
                juce::Array<juce::var> seg;
                seg.add (s.start);
                seg.add (s.end);
                seg.add (s.takeId);
                comp.add (juce::var (seg));
            }
            lo->setProperty ("base", pointsToVar (data.base));
            lo->setProperty ("takes", takes);
            lo->setProperty ("comp", comp);
            lo->setProperty ("nextTake", data.nextTakeId);
            lo->setProperty ("crossfade", data.crossfade);
        }
        ao->setProperty (juce::Identifier (id), juce::var (lo));
    }
    o.setProperty ("automation", juce::var (ao));
}

void MainComponent::readEffectsAndAutomation (Track& t, const juce::var& item, juce::StringArray& problems)
{
    const auto fxv = item["fx"];
    if (fxv.isObject())
    {
        t.fx.forEachFloat ([&fxv] (const char* name, std::atomic<float>& v)
                           { if (fxv.hasProperty (name)) v = (float) (double) fxv.getProperty (name, 0.0); });
        t.fx.forEachBool ([&fxv] (const char* name, std::atomic<bool>& v)
                          { if (fxv.hasProperty (name)) v = (bool) fxv.getProperty (name, false); });
    }

    pluginsFromVar (t, item, problems);

    // (plugins are loaded first, so lanes bound to plugin parameters can find them)
    if (auto* automation = item["automation"].getDynamicObject())
        for (auto& entry : automation->getProperties())
        {
            const juce::String id = entry.name.toString();
            std::vector<AutoPoint> points;
            if (auto* arr = entry.value["points"].getArray())
                for (auto& pv : *arr)
                    if (auto* pair = pv.getArray(); pair != nullptr && pair->size() >= 2)
                        points.push_back ({ (double) (*pair)[0], (float) (double) (*pair)[1] });

            if (engine.setAutomation (t, id, std::move (points)))
                engine.setAutomationActive (t, id, (bool) entry.value.getProperty ("active", true));
            if (entry.value.hasProperty ("takes"))
            {
                auto readPoints = [] (const juce::var& arrVar)
                {
                    std::vector<AutoPoint> pts;
                    if (auto* arr = arrVar.getArray())
                        for (auto& pv : *arr)
                            if (auto* pair = pv.getArray(); pair != nullptr && pair->size() >= 2)
                                pts.push_back ({ (double) (*pair)[0], (float) (double) (*pair)[1] });
                    return pts;
                };
                AutoData data;
                data.base = readPoints (entry.value["base"]);
                data.nextTakeId = (int) entry.value.getProperty ("nextTake", 1);
                data.crossfade = (double) entry.value.getProperty ("crossfade", 0.03);
                if (auto* takes = entry.value["takes"].getArray())
                    for (auto& tv : *takes)
                    {
                        AutoTake tk;
                        tk.id = (int) tv.getProperty ("id", 0);
                        tk.name = tv.getProperty ("name", "Take").toString();
                        tk.start = (double) tv.getProperty ("start", 0.0);
                        tk.end = (double) tv.getProperty ("end", 0.0);
                        tk.points = readPoints (tv["points"]);
                        data.takes.push_back (std::move (tk));
                    }
                if (auto* comp = entry.value["comp"].getArray())
                    for (auto& cv : *comp)
                        if (auto* seg = cv.getArray(); seg != nullptr && seg->size() >= 3)
                            data.comp.push_back ({ (double) (*seg)[0], (double) (*seg)[1], (int) (*seg)[2] });
                engine.setAutomationData (t, id, std::move (data));
            }
        }
}

void MainComponent::toggleLoop()
{
    const bool on = loopButton.getToggleState();
    if (on && engine.loopEnd <= engine.loopStart)   // no region yet: use the project, at least 4 bars
    {
        const int endBar = juce::jmax (4, tempoMap.barAtSeconds (engine.getTotalLengthSeconds()) + 1);   // whole bars, at least 4
        engine.loopStart = 0.0;
        engine.loopEnd = tempoMap.barStartSeconds (endBar);
    }
    engine.loopEnabled = on;
    ruler->repaint();
    repaint();
}

void MainComponent::togglePunch()
{
    const bool on = punchButton.getToggleState();
    if (on && engine.punchOut <= engine.punchIn)   // no region yet: the selected clip, else two bars from the playhead's bar
    {
        double a = 0.0, b = 0.0;
        Clip ac;
        MidiClip mc;
        if (selTrack != nullptr && selClip >= 0 && engine.getClip (*selTrack, selClip, ac))
            a = ac.start, b = ac.end();
        else if (selTrack != nullptr && selClip >= 0 && engine.getMidiClip (*selTrack, selClip, mc))
            a = mc.start, b = mc.end();
        else
        {
            const int barIndex = tempoMap.barAtSeconds (engine.getPositionSeconds());
            a = tempoMap.barStartSeconds (barIndex);
            b = tempoMap.barStartSeconds (barIndex + 2);
        }
        engine.punchIn = a;
        engine.punchOut = b;
    }
    engine.punchEnabled = on;
    ruler->repaint();
    repaint();
}

void MainComponent::setPunchRegion (double a, double b)
{
    if (b - a < tempoMap.beatSecondsAt (a) * 0.5)
        return;
    engine.punchIn = a;
    engine.punchOut = b;
    engine.punchEnabled = true;
    punchButton.setToggleState (true, juce::dontSendNotification);
    ruler->repaint();
    repaint();
}

void MainComponent::openTuner()
{
    if (tunerWindow != nullptr)
    {
        tunerWindow.reset();
        return;
    }

    auto* device = deviceManager.getCurrentAudioDevice();
    juce::StringArray names;
    if (device != nullptr)
    {
        const auto active = device->getActiveInputChannels();
        const auto all = device->getInputChannelNames();
        for (int i = 0, n = 0; i < all.size(); ++i)
            if (active[i])
                names.add ("Input " + juce::String (++n) + " (" + all[i] + ")");
    }
    if (names.isEmpty())
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "No audio input",
                                                "The tuner needs an audio input. Check the device under Audio...");
        return;
    }

    int first = 0;   // the input of the first armed audio track
    for (int i = 0; i < engine.getNumTracks(); ++i)
        if (auto* t = engine.getTrack (i); t->type == Track::Type::Audio && t->recordArmed)
        {
            first = juce::jlimit (0, names.size() - 1, t->inputFirst);
            break;
        }

    juce::Component::SafePointer<MainComponent> safe (this);
    tunerWindow = std::make_unique<TunerWindow> (engine, names, first, [safe]
    {
        juce::MessageManager::callAsync ([safe] { if (safe != nullptr) safe->tunerWindow.reset(); });
    });
}

void MainComponent::loopToSelection()
{
    if (selTrack == nullptr || selClip < 0)
        return;
    Clip a;
    MidiClip m;
    if (engine.getClip (*selTrack, selClip, a))
        setLoopRegion (a.start, a.end());
    else if (engine.getMidiClip (*selTrack, selClip, m))
        setLoopRegion (m.start, m.end());
}

void MainComponent::setPlayhead (double seconds)
{
    engine.setPositionSeconds (seconds);
    repaint();
}

void MainComponent::zoomBy (double factor, int anchorX)
{
    const double old = pixelsPerSecond;
    const double next = juce::jlimit (8.0, 800.0, old * factor);
    if (next == old)
        return;

    const double anchorTime = (laneViewport.getViewPositionX() + anchorX) / old;
    pixelsPerSecond = next;
    layoutContent();
    laneViewport.setViewPosition (juce::jmax (0, (int) (anchorTime * next - anchorX)), laneViewport.getViewPositionY());
    ruler->repaint();
    roll->repaintGrid();
    repaint();
}

void MainComponent::clipsChanged()
{
    for (auto* row : subRows)
        row->repaint();   // (the notes of the sub-tracks)
    layoutContent();
    ruler->repaint();
    roll->repaintGrid();
    repaint();
}

//==============================================================================
// Undo/redo: before each change a snapshot of the project is pushed; undo swaps the live
// project with the latest snapshot (and keeps the state it replaced for redo).

void MainComponent::beginEdit()
{
    ++editCounter;   // (the project differs from what was saved)
    undoStack.push_back (engine.captureState());
    if (undoStack.size() > 100)
        undoStack.erase (undoStack.begin());
    redoStack.clear();
    updateUndoButtons();
}

void MainComponent::cancelEdit()
{
    if (! undoStack.empty())
    {
        undoStack.pop_back();
        --editCounter;
    }
    updateUndoButtons();
}

void MainComponent::updateUndoButtons()
{
    undoButton.setEnabled (! undoStack.empty());
    redoButton.setEnabled (! redoStack.empty());
}

// The plugins whose state a snapshot holds: going to it, the state they have now is kept for the way back
static std::vector<std::shared_ptr<PluginSlot>> pluginsOf (const ProjectState& state)
{
    std::vector<std::shared_ptr<PluginSlot>> slots;
    for (auto& p : state.pluginStates)
        slots.push_back (p.first);
    return slots;
}

void MainComponent::undo()
{
    if (undoStack.empty())
        return;
    auto state = std::move (undoStack.back());
    undoStack.pop_back();
    redoStack.push_back (engine.captureState (pluginsOf (state)));
    restoreSnapshot (state);
}

void MainComponent::redo()
{
    if (redoStack.empty())
        return;
    auto state = std::move (redoStack.back());
    redoStack.pop_back();
    undoStack.push_back (engine.captureState (pluginsOf (state)));
    restoreSnapshot (state);
}

// What is done inside a plugin's own window is part of undo: opening the window takes an undo step that holds the plugin's state of that
// moment (Ctrl+Z puts it back); when the window closes with nothing changed and nothing else done, that step is dropped again.
void MainComponent::pluginEditorOpened (PluginSlot& slot)
{
    auto shared = engine.findPluginSlot (&slot);
    if (shared == nullptr || slot.instance == nullptr)
        return;
    PluginOpen info;
    slot.instance->getStateInformation (info.before);
    ++editCounter;
    undoStack.push_back (engine.captureState ({ shared }));
    if (undoStack.size() > 100)
        undoStack.erase (undoStack.begin());
    redoStack.clear();
    updateUndoButtons();
    info.undoSize = undoStack.size();
    pluginOpen[&slot] = std::move (info);
}

void MainComponent::pluginEditorClosed (PluginSlot& slot)
{
    auto it = pluginOpen.find (&slot);
    if (it == pluginOpen.end())
        return;
    const auto info = std::move (it->second);
    pluginOpen.erase (it);
    if (slot.instance == nullptr)
        return;
    juce::MemoryBlock after;
    slot.instance->getStateInformation (after);
    if (after == info.before && undoStack.size() == info.undoSize)   // looked, did not touch: no undo step
        cancelEdit();
}

// Ctrl+Z / Ctrl+Y / Ctrl+Shift+Z in the sampler and drum rack windows undo the project, like in the main window.
bool MainComponent::handleUndoKey (const juce::KeyPress& k)
{
    const auto mods = k.getModifiers();
    if (! mods.isCommandDown())
        return false;
    const int code = k.getKeyCode();
    const bool isZ = code == 'Z' || code == 26, isY = code == 'Y' || code == 25;   // (Ctrl+letter can arrive as the control character, too)
    const bool isUndo = isZ && ! mods.isShiftDown();
    const bool isRedo = (isZ && mods.isShiftDown()) || isY;
    if (! isUndo && ! isRedo)
        return false;
    // later: undo closes and opens these windows again
    juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<MainComponent> (this), isUndo]
    {
        if (safe == nullptr)
            return;
        if (isUndo) safe->undo(); else safe->redo();
    });
    return true;
}

void MainComponent::restoreSnapshot (const ProjectState& state)
{
    // The tool windows stay open when the sampler / drum rack they edit still exists after the undo (it is the same object, with the
    // old settings put back): they are pointed at the rebuilt track and refreshed. Otherwise they are closed.
    const int idxSampler = samplerWindow != nullptr ? engine.indexOfTrack (samplerRef) : -1;
    const int idxPad = padSamplerWindow != nullptr ? engine.indexOfTrack (padRef) : -1;
    const int idxRack = drumRackWindow != nullptr ? engine.indexOfTrack (rackRef) : -1;
    const Sampler* samplerObject = idxSampler >= 0 ? samplerRef->sampler.get() : nullptr;
    const DrumKit* padKit = idxPad >= 0 ? padRef->kit.get() : nullptr;
    const DrumKit* rackKit = idxRack >= 0 ? rackRef->kit.get() : nullptr;
    // Remember what was selected so it can be selected again (and the piano roll reopened).
    const int selTrackIndex = selTrack != nullptr ? engine.indexOfTrack (selTrack) : -1;
    const int selClipId = selClip;

    hideRoll();
    selTrack = nullptr;
    selClip = -1;
    headers.clear();   // they reference the Track objects that restoreState() replaces
    lanes.clear();
    subRows.clear();
    subHeaders.clear();
    subFooters.clear();
    subSignature = {};

    if (mixerWindow != nullptr)
        mixerWindow->getPanel().detach();   // (the strips point at tracks that are replaced)
    engine.restoreState (state);
    if (mixerWindow != nullptr)
        mixerWindow->getPanel().rebuild();

    for (int i = 0; i < engine.getNumTracks(); ++i)
        addTrackUI (*engine.getTrack (i));

    tempoMap = engine.getTempoMap();
    bpmSlider.setValue (tempoMap.getSections().front().bpm, juce::dontSendNotification);
    masterHeader->syncControls();   // the master channel was restored in place

    if (selTrackIndex >= 0)
        if (auto* t = engine.getTrack (selTrackIndex))
        {
            Clip a;
            MidiClip m;
            if (engine.getClip (*t, selClipId, a) || engine.getMidiClip (*t, selClipId, m))
                selectClip (t, selClipId);
        }

    ++editCounter;        // undo / redo changed the project, too
    updateUndoButtons();
    updateArmedTrack();   // the old Track pointer is gone
    clipsChanged();

    if (samplerWindow != nullptr)
    {
        auto* nt = engine.getTrack (idxSampler);
        if (nt != nullptr && nt->sampler.get() == samplerObject)
        {
            *samplerRef.p = nt;
            samplerWindow->getPanel().refresh();
        }
        else
            samplerWindow.reset();
    }
    if (padSamplerWindow != nullptr)
    {
        auto* nt = engine.getTrack (idxPad);
        if (nt != nullptr && nt->kit.get() == padKit)
        {
            *padRef.p = nt;
            padSamplerWindow->getPanel().refresh();
        }
        else
            padSamplerWindow.reset();
    }
    if (drumRackWindow != nullptr)
    {
        auto* nt = engine.getTrack (idxRack);
        if (nt != nullptr && nt->kit.get() == rackKit)
        {
            *rackRef.p = nt;
            drumRackWindow->getPanel().refreshControls();
            drumRackWindow->getPanel().repaint();
        }
        else
        {
            padSamplerWindow.reset();
            drumRackWindow.reset();
        }
    }
}


//==============================================================================
// Right-click on a track header: the things you do to a whole track.
// The colours a track can get (right-click the header > Colour)
static const std::array<juce::uint32, 8> trackPalette = { 0xffd9702a, 0xffff5a47, 0xffffc933, 0xffe86a9a, 0xff9ad06a, 0xff4fb3bf, 0xff6a8cff, 0xffa07cff };
static const std::array<const char*, 8> trackPaletteNames = { "Orange", "Red", "Yellow", "Pink", "Green", "Teal", "Blue", "Purple" };

void MainComponent::showTrackMenu (Track& t)
{
    const int index = engine.indexOfTrack (&t);
    if (index < 0)
        return;
    juce::Component::SafePointer<MainComponent> safe (this);
    const TrackRef ref (&t);

    juce::PopupMenu menu;
    menu.addItem (1, "Rename");
    menu.addItem (2, "Duplicate track");
    menu.addItem (3, "Delete track");
    menu.addSeparator();
    menu.addItem (4, "Move up", index > 0);
    menu.addItem (5, "Move down", index + 1 < engine.getNumTracks());
    menu.addSeparator();
    menu.addItem (6, "Mute", true, t.mute.load());
    menu.addItem (7, "Solo", true, t.solo.load());
    if (! t.bus)
        menu.addItem (8, "Arm for recording", true, t.type == Track::Type::Midi ? t.midiArmed : t.recordArmed);
    menu.addSeparator();
    menu.addItem (9, t.sampler != nullptr ? "Open the sampler" : t.kit != nullptr ? "Open the drum rack" : "Instrument and plugins...");
    {
        juce::PopupMenu colours;   // the colour of the track: header, mixer strip and clips
        for (int i = 0; i < (int) trackPalette.size(); ++i)
        {
            juce::PopupMenu::Item item (trackPaletteNames[(size_t) i]);
            item.itemID = 40 + i;
            item.colour = juce::Colour (trackPalette[(size_t) i]);
            item.isTicked = t.colour == trackPalette[(size_t) i];
            colours.addItem (item);
        }
        colours.addSeparator();
        colours.addItem (49, "Default", true, t.colour == 0);
        menu.addSeparator();
        menu.addSubMenu ("Colour", colours);
        menu.addItem (30, "Save as a template...");
        showTrackGroupMenuItems (menu, t);
        if (! t.bus)
        {
            menu.addSeparator();
            menu.addItem (t.frozen.load() ? 61 : 60,
                          t.frozen.load() ? "Unfreeze (play the track itself again)" : "Freeze: bounce to audio and switch it off (saves CPU)",
                          t.frozen.load() || engine.trackHasOutput (t));
        }
    }
    if (t.type == Track::Type::Midi && ! t.bus)
    {
        juce::PopupMenu sub;
        sub.addItem (21, t.subLanesOpen ? "Hide the sub-tracks" : "Show the sub-tracks", ! t.subLanes.empty());
        sub.addSeparator();
        sub.addItem (22, "Create from the notes used (one per pitch)");
        sub.addItem (23, "Create drum sub-tracks (kick, snare, hats...)");
        sub.addItem (24, "Add a sub-track");
        sub.addSeparator();
        sub.addItem (25, "Remove all sub-tracks", ! t.subLanes.empty());
        menu.addSeparator();
        menu.addSubMenu ("Sub-tracks (a lane per pitch)", sub);
    }

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea (juce::Rectangle<int> (1, 1).withCentre (juce::Desktop::getMousePosition())),
                        [safe, ref] (int result)
    {
        if (safe == nullptr || result == 0 || safe->engine.indexOfTrack (ref) < 0)
            return;
        auto& self = *safe;
        auto& track = *ref;

        switch (result)
        {
            case 1:
                for (auto* header : self.headers)
                    if (&header->getTrack() == &track)
                        header->startRename();
                break;
            case 2:   // a copy right after it (plugins are made again, their settings carried over)
            {
                self.beginEdit();
                auto* copy = self.engine.duplicateTrack (track);
                if (copy == nullptr)
                {
                    self.cancelEdit();
                    break;
                }
                juce::StringArray problems;
                auto* holder = new juce::DynamicObject();
                holder->setProperty ("plugins", self.pluginsToVar (track));
                self.pluginsFromVar (*copy, juce::var (holder), problems);
                self.rebuildTrackUI();
                self.selectClip (nullptr, -1);
                if (problems.size() > 0)
                    juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Some plugins could not be copied",
                                                            problems.joinIntoString ("\n"));
                break;
            }
            case 3:
                juce::MessageManager::callAsync ([safe, ref] { if (safe != nullptr && safe->engine.indexOfTrack (ref) >= 0) safe->removeTrackUI (ref); });
                break;
            case 4:
            case 5:
                self.beginEdit();
                if (self.engine.moveTrack (track, result == 4 ? -1 : 1))
                    self.rebuildTrackUI();
                else
                    self.cancelEdit();
                break;
            case 6: self.beginEdit(); track.mute = ! track.mute.load(); break;
            case 7: self.beginEdit(); track.solo = ! track.solo.load(); break;
            case 8: self.armTrack (track); break;
            case 9: self.openInstrumentEditor (track); break;
            case 21: case 22: case 23: case 24: case 25: self.applySubLaneChoice (track, result - 20); break;
            case 30: self.saveTrackTemplate (track); break;
            case 60: self.freezeTrack (track); break;
            case 61: self.unfreezeTrack (track); break;
            case 70:   // a new group with this track
            {
                self.beginEdit();
                const int id = self.engine.createGroup ("Group " + juce::String ((int) self.engine.getGroups().size() + 1));
                self.engine.setTrackGroup (track, id);
                self.groupChanged();
                break;
            }
            case 71:
                self.beginEdit();
                self.engine.setTrackGroup (track, 0);
                self.groupChanged();
                break;
            default:
                if (result >= 80 && result < 80 + (int) self.engine.getGroups().size())   // into an existing group
                {
                    self.beginEdit();
                    self.engine.setTrackGroup (track, self.engine.getGroups()[(size_t) (result - 80)].id);
                    self.groupChanged();
                }
                else if (result >= 40 && result < 40 + (int) trackPalette.size())   // a colour
                {
                    self.beginEdit();
                    track.colour = trackPalette[(size_t) (result - 40)];
                    self.clipsChanged();
                }
                else if (result == 49)
                {
                    self.beginEdit();
                    track.colour = 0;
                    self.clipsChanged();
                }
                break;
        }
    });
}

// The headers and lanes again, in the order of the engine's tracks (after a track was moved or copied).
void MainComponent::rebuildTrackUI()
{
    subSignature = {};   // (the sub-track rows are made again too)
    subRows.clear();
    subHeaders.clear();
    subFooters.clear();
    if (mixerWindow != nullptr)
        mixerWindow->getPanel().detach();
    headers.clear();
    lanes.clear();
    subRows.clear();
    subHeaders.clear();
    subFooters.clear();
    subSignature = {};
    for (int i = 0; i < engine.getNumTracks(); ++i)
        addTrackUI (*engine.getTrack (i));
    clipsChanged();
}

// Dragging a track's header: shows where it would land (a line), and moves the track there when the mouse is released.
void MainComponent::dragTrackHeader (Track& t, int y, bool finished)
{
    int from = -1;
    for (int i = 0; i < lanes.size(); ++i)
        if (&lanes[i]->getTrack() == &t)
            from = i;
    if (from < 0 || trackTops.size() != (size_t) lanes.size())
        return;

    const int n = lanes.size(), total = trackRowsEnd;
    int to = 0, lineY = 0;
    bool placed = false;
    for (int j = 0; j < n; ++j)
    {
        if (j == from)
            continue;
        const int top = trackTops[(size_t) j];
        const int bottom = j + 1 < n ? trackTops[(size_t) j + 1] : total;
        if (! placed && y < (top + bottom) / 2)
        {
            lineY = top;
            placed = true;
            break;
        }
        ++to;
        lineY = bottom;
    }

    if (! finished)
    {
        if (dropLine.getParentComponent() != &headerHolder)
            headerHolder.addAndMakeVisible (dropLine);
        dropLine.setBounds (4, juce::jmax (0, lineY - 2), controlsWidth - 8, 4);
        dropLine.toFront (false);
        return;
    }

    headerHolder.removeChildComponent (&dropLine);
    int steps = to - from;
    if (steps == 0)
        return;
    beginEdit();
    const int dir = steps < 0 ? -1 : 1;
    bool moved = false;
    for (; steps != 0; steps -= dir)
        moved = engine.moveTrack (t, dir) || moved;
    if (moved)
        juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<MainComponent> (this)] { if (safe != nullptr) safe->rebuildTrackUI(); });
    else
        cancelEdit();
}

std::unique_ptr<TrackHeader> MainComponent::makeHeader (Track& t, bool master)
{
    auto header = makeHeaderRaw (t, master);
    if (! master)
        header->reorder = [this] (Track& tr, int y, bool finished) { dragTrackHeader (tr, y, finished); };
    if (master)
        header->openMeter = [this] { openMasterMeter(); };
    header->openFxPanel = [this, master] (Track& tr, juce::Component& anchor)
    {
        juce::CallOutBox::launchAsynchronously (std::make_unique<FxPanel> (tr, [this] { beginEdit(); }, master ? nullptr : &engine),
                                                anchor.getScreenBounds(), nullptr);
    };
    return header;
}

std::unique_ptr<TrackHeader> MainComponent::makeHeaderRaw (Track& t, bool master)
{
    return std::make_unique<TrackHeader> (t, [this] (Track* tr)
    {
        // Deferred: this runs inside a button owned by the header being deleted.
        juce::MessageManager::callAsync ([this, tr] { removeTrackUI (tr); });
    }, [this] { beginEdit(); },
       [this] (Track& tr, juce::Component& anchor) { openPlugins (tr, anchor); },
       [this] (Track& tr, juce::Component& anchor) { openAutomation (tr, anchor); },
       [this] { return engine.getPositionSeconds(); },
       [this] (Track& tr) { openInstrumentEditor (tr); },
       [this] (Track& tr) { armTrack (tr); },
       [this] (Track& tr, juce::Component& anchor) { openInputMenu (tr, anchor); },
       [this] (Track& tr, juce::Component& anchor) { openRouting (tr, anchor); },
       [this] (Track& tr) { showTrackMenu (tr); },
       [this] (Track& tr) { toggleSubLanes (tr); },
       master);
}

void MainComponent::addTrackUI (Track& t)
{
    auto* header = headers.add (makeHeader (t, false).release());
    headerHolder.addAndMakeVisible (header);

    auto* lane = lanes.add (new TrackLane (*this, t));
    laneHolder.addAndMakeVisible (lane);
    layoutContent();
}

void MainComponent::removeTrackUI (Track* t)
{
    if (mixerWindow != nullptr)
        mixerWindow->getPanel().detach();   // (the strips would point at the removed track)
    samplerWindow.reset();   // (they point into the track)
    padSamplerWindow.reset();
    drumRackWindow.reset();
    for (int i = 0; i < lanes.size(); ++i)
        if (&lanes[i]->getTrack() == t)
        {
            beginEdit();
            if (selTrack == t)
                selectClip (nullptr, -1);
            if (roll->getTrack() == t)
                hideRoll();
            if (t->instrument != nullptr)
                pluginHost.closeEditorsFor (t->instrument.get());
            for (auto& p : t->inserts)
                pluginHost.closeEditorsFor (p.get());
            for (auto& p : t->midiFx)
                pluginHost.closeEditorsFor (p.get());
            headers.remove (i);
            lanes.remove (i);
            engine.removeTrack (t);
            layoutContent();
            return;
        }
}

void MainComponent::clearTracksUI()
{
    if (mixerWindow != nullptr)
        mixerWindow->getPanel().detach();
    samplerWindow.reset();
    padSamplerWindow.reset();
    drumRackWindow.reset();
    pluginHost.closeAllEditors();
    hideRoll();
    selectClip (nullptr, -1);
    headers.clear();
    lanes.clear();
    subRows.clear();
    subHeaders.clear();
    subFooters.clear();
    subSignature = {};
    engine.removeAllTracks();
    layoutContent();
}

void MainComponent::layoutContent()
{
    if (getWidth() <= 0)
        return;

    const int y0 = topBarHeight + rulerHeight;
    constexpr int R = railWidth;
    const int laneW = getWidth() - R - controlsWidth;
    const int rollH = rollVisible ? juce::jmin (rollHeight, getHeight() / 2) : 0;
    const int lanesH = juce::jmax (0, getHeight() - y0 - bottomBarHeight - rollH - masterHeight - 2);   // 2 px for the line above the master strip
    laneViewport.setBounds (R + controlsWidth, y0, laneW, lanesH);

    // master channel strip pinned under the tracks
    const int masterY = y0 + lanesH + 2;
    masterHeader->setBounds (R, masterY, controlsWidth, masterHeight);
    masterArea.setBounds (R + controlsWidth, masterY, laneW, masterHeight);
    for (juce::Component* panel : std::initializer_list<juce::Component*> { pianoRoll.get(), drumRoll.get() })
    {
        panel->setBounds (R, getHeight() - bottomBarHeight - rollH, getWidth() - R, rollH);
        panel->setVisible (rollVisible && panel == roll);
    }
    ruler->setBounds (R + controlsWidth, topBarHeight, laneW, rulerHeight);

    const int visibleW = laneViewport.getMaximumVisibleWidth();
    const int visibleH = laneViewport.getMaximumVisibleHeight();
    headerViewport.setBounds (R, y0, controlsWidth, visibleH);

    syncSubLaneUI();   // (sub-track rows come and go with the tracks)
    const double seconds = engine.getTotalLengthSeconds() + 10.0;
    const int contentW = juce::jmax (visibleW, (int) (seconds * pixelsPerSecond));

    syncGroupUI();   // (group header rows come and go with the groups)

    // the rows from the top: a group header (above the first track of a group), a track, then its sub-tracks (when they are shown), then the next track.
    // The tracks of a collapsed group are hidden.
    trackTops.assign ((size_t) lanes.size(), 0);
    int y = 0, subIndex = 0, footerIndex = 0, groupIndex = 0, previousGroup = 0;
    for (int i = 0; i < lanes.size(); ++i)
    {
        const int gid = lanes[i]->getTrack().groupId;
        bool hidden = false;
        if (gid != 0)
        {
            auto* group = engine.findGroup (gid);
            if (gid != previousGroup && group != nullptr && groupIndex < groupHeaders.size())
            {
                groupHeaders[groupIndex]->setBounds (0, y, controlsWidth, groupRowHeight);
                groupBands[groupIndex]->setBounds (0, y, contentW, groupRowHeight);
                y += groupRowHeight;
                ++groupIndex;
            }
            hidden = group != nullptr && group->collapsed;
        }
        previousGroup = gid;

        trackTops[(size_t) i] = y;
        lanes[i]->setVisible (! hidden);
        headers[i]->setVisible (! hidden);
        if (! hidden)
        {
            lanes[i]->setBounds (0, y, contentW, stripHeight);
            headers[i]->setBounds (0, y, controlsWidth, stripHeight);
            y += stripHeight;
        }
        while (subIndex < subRows.size() && subRows[subIndex]->getTrack() == &lanes[i]->getTrack())
        {
            subRows[subIndex]->setVisible (! hidden);
            subHeaders[subIndex]->setVisible (! hidden);
            if (! hidden)
            {
                subRows[subIndex]->setBounds (0, y, contentW, subLaneHeight);
                subHeaders[subIndex]->setBounds (0, y, controlsWidth, subLaneHeight);
                y += subLaneHeight;
            }
            ++subIndex;
        }
        if (auto& t = lanes[i]->getTrack(); t.type == Track::Type::Midi && t.subLanesOpen && ! t.subLanes.empty() && footerIndex < subFooters.size()
                                            && subFooters[footerIndex]->getTrack() == &t)
        {
            subFooters[footerIndex]->setVisible (! hidden);
            if (! hidden)
            {
                subFooters[footerIndex]->setBounds (0, y, controlsWidth, subFooterHeight);
                y += subFooterHeight;
            }
            ++footerIndex;
        }
    }
    trackRowsEnd = y;   // (the bottom of the last track, with its sub-tracks)
    const int contentH = juce::jmax (y, visibleH);
    laneHolder.setSize (contentW, contentH);
    headerHolder.setSize (controlsWidth, contentH);
    layoutMasterLane();
}

// The master strip's lane is as wide as the arrangement and slides sideways with it (the strip clips it).
void MainComponent::layoutMasterLane()
{
    if (masterLane != nullptr)
        masterLane->setBounds (-getScrollX(), 0, juce::jmax (laneHolder.getWidth(), masterArea.getWidth()), masterHeight);
}

void MainComponent::resized()
{
    constexpr int R = railWidth;

    // icon rail: file, add, plugins, tuner at the top; audio settings at the bottom
    constexpr int rb = 46;
    railFile.setBounds (0, 8, R, rb);
    railAdd.setBounds (0, 8 + rb + 4, R, rb);
    railPlugins.setBounds (0, 8 + 2 * (rb + 4), R, rb);
    railTuner.setBounds (0, 8 + 3 * (rb + 4), R, rb);
    railMixer.setBounds (0, 8 + 4 * (rb + 4), R, rb);
    railAudio.setBounds (0, getHeight() - rb - 10, R, rb);
    fileButton.setBounds (railFile.getBounds());          // (the hidden buttons sit over their icons)
    addButton.setBounds (railAdd.getBounds());
    pluginsButton.setBounds (railPlugins.getBounds());
    tunerButton.setBounds (railTuner.getBounds());
    audioButton.setBounds (railAudio.getBounds());

    // top bar: time and tempo, then undo / redo, the clip lamp, and STOP ALL at the far right
    topSeparators.clear();
    auto top = juce::Rectangle<int> (R, 0, getWidth() - R, topBarHeight - 3).reduced (12, 9);
    stopAllButton.setBounds (top.removeFromRight (104));   // (far right, away from the transport)
    updateButton.setVisible (updateVersion.isNotEmpty());
    if (updateVersion.isNotEmpty())
    {
        top.removeFromRight (10);
        updateButton.setBounds (top.removeFromRight (150));
    }
    timeLabel.setBounds (top.removeFromLeft (150));
    top.removeFromLeft (14);
    bpmLabel.setBounds (top.removeFromLeft (36));
    bpmSlider.setBounds (top.removeFromLeft (70));
    top.removeFromLeft (6);
    tapButton.setBounds (top.removeFromLeft (46));
    top.removeFromLeft (6);
    tempoButton.setBounds (top.removeFromLeft (62));
    top.removeFromLeft (8);
    sectionsButton.setBounds (top.removeFromLeft (78));
    top.removeFromLeft (8);
    countInButton.setBounds (top.removeFromLeft (128));
    top.removeFromLeft (8);
    followButton.setBounds (top.removeFromLeft (70));
    top.removeFromLeft (20);
    undoButton.setBounds (top.removeFromLeft (56));
    top.removeFromLeft (6);
    redoButton.setBounds (top.removeFromLeft (56));
    top.removeFromLeft (16);
    clipLamp.setBounds (top.removeFromLeft (64));

    // bottom: MIDI/armed status on the left, computer-keys toggle on the right, the transport as a floating pill in the middle
    const int barTop = getHeight() - bottomBarHeight;
    keysButton.setBounds (getWidth() - 100, barTop + 18, 90, 24);

    constexpr int tb = 40, gap = 8;
    const int extras = 6 + 60 + 6 + 60 + 6 + 56;   // loop, punch and click after the five transport buttons
    const int groupW = 5 * tb + 4 * gap + extras;
    int bx = R + (getWidth() - R - groupW) / 2;
    const int by = barTop + (bottomBarHeight - tb) / 2;
    transportPill = { bx - 16, by - 6, groupW + 32, tb + 12 };
    statusLabel.setBounds (R + 10, barTop + 4, juce::jmax (100, transportPill.getX() - R - 22), bottomBarHeight - 8);
    const int statusLevel = statusLabel.getWidth() < 300 ? 2 : statusLabel.getWidth() < 460 ? 1 : 0;
    if (statusLevel != statusCompact)   // the status text has shorter forms for narrow windows
    {
        statusCompact = statusLevel;
        updateAudioInfo();
    }
    for (auto* b : std::initializer_list<juce::Component*> { &rewindButton, &playButton, &pauseButton, &stopButton, &recordButton })
    {
        b->setBounds (bx, by, tb, tb);
        bx += tb + gap;
    }
    loopButton.setBounds (bx + 6, by + 6, 60, tb - 12);
    punchButton.setBounds (bx + 6 + 60 + 6, by + 6, 60, tb - 12);
    metroButton.setBounds (bx + 6 + 60 + 6 + 60 + 6, by + 6, 56, tb - 12);
    for (auto* pill : { &loopButton, &punchButton, &metroButton })
        pill->getProperties().set ("pill", true);

    // snap + zoom live in the corner above the track headers
    auto corner = juce::Rectangle<int> (R, topBarHeight, controlsWidth, rulerHeight).reduced (6, 2);
    snapButton.setBounds (corner.removeFromLeft (62));
    corner.removeFromLeft (6);
    panSnapButton.setBounds (corner.removeFromLeft (112));
    zoomInButton.setBounds (corner.removeFromRight (30));
    corner.removeFromRight (4);
    zoomOutButton.setBounds (corner.removeFromRight (30));

    if (tour != nullptr)
        tour->setBounds (getLocalBounds());
    layoutContent();
}

void MainComponent::paint (juce::Graphics& g)
{
    constexpr int R = railWidth;
    g.fillAll (Theme::background);

    // the icon rail, and the top bar with a thin red - orange - yellow strip along its lower edge
    g.setColour (Theme::rowDark);
    g.fillRect (0, 0, R, getHeight());
    g.setColour (Theme::border);
    g.fillRect (R - 1, 0, 1, getHeight());

    g.setColour (Theme::panel);
    g.fillRect (R, 0, getWidth() - R, topBarHeight);
    juce::ColourGradient strip (Theme::red, (float) R, 0.0f, Theme::yellow, (float) getWidth(), 0.0f, false);
    strip.addColour (0.5, Theme::orange);
    g.setGradientFill (strip);
    g.fillRect (R, topBarHeight - 2, getWidth() - R, 2);

    // the transport floats on the bottom as a rounded pill
    g.setColour (Theme::panel);
    g.fillRoundedRectangle (transportPill.toFloat(), (float) transportPill.getHeight() * 0.5f);
    g.setColour (Theme::gridLineStrong.withAlpha (0.6f));
    g.drawRoundedRectangle (transportPill.toFloat().reduced (0.5f), (float) transportPill.getHeight() * 0.5f, 1.0f);

    // a yellow line above the master channel strip sets it apart from the tracks
    if (masterArea.isVisible())
    {
        g.setColour (Theme::yellowDim);
        g.fillRect (R, masterArea.getY() - 2, getWidth() - R, 2);
    }
}

void MainComponent::paintOverChildren (juce::Graphics& g)
{
    if (tour != nullptr)
        return;   // (the quick tour is on top: its dimming should cover everything)
    // loop region, as a faint wash over the lanes
    if (engine.loopEnabled && engine.loopEnd > engine.loopStart)
    {
        const auto lanesArea = laneViewport.getBounds();
        const int x1 = juce::jmax (lanesArea.getX(), lanesArea.getX() + (int) (engine.loopStart * pixelsPerSecond) - getScrollX());
        const int x2 = juce::jmin (lanesArea.getRight(), lanesArea.getX() + (int) (engine.loopEnd * pixelsPerSecond) - getScrollX());
        if (x2 > x1)
        {
            g.setColour (Theme::orange.withAlpha (0.07f));
            g.fillRect (x1, lanesArea.getY(), x2 - x1, lanesArea.getHeight());
        }
    }

    // punch region: where a recording is captured, as a faint red wash
    if (engine.punchEnabled && engine.punchOut > engine.punchIn)
    {
        const auto lanesArea = laneViewport.getBounds();
        const int x1 = juce::jmax (lanesArea.getX(), lanesArea.getX() + (int) (engine.punchIn * pixelsPerSecond) - getScrollX());
        const int x2 = juce::jmin (lanesArea.getRight(), lanesArea.getX() + (int) (engine.punchOut * pixelsPerSecond) - getScrollX());
        if (x2 > x1)
        {
            g.setColour (Theme::redBright.withAlpha (0.10f));
            g.fillRect (x1, lanesArea.getY(), x2 - x1, lanesArea.getHeight());
            g.setColour (Theme::redBright.withAlpha (0.6f));
            g.fillRect (x1, lanesArea.getY(), 1, lanesArea.getHeight());
            g.fillRect (x2 - 1, lanesArea.getY(), 1, lanesArea.getHeight());
        }
    }

    const int x = laneViewport.getX() + (int) (engine.getPositionSeconds() * pixelsPerSecond) - getScrollX();
    if (x >= laneViewport.getX() && x < laneViewport.getRight())
    {
        g.setColour (Theme::playhead);
        g.fillRect (x - 1, ruler->getY(), 2, masterArea.getBottom() - ruler->getY());   // through the lanes and the master strip
        juce::Path head;   // small flag on the ruler so the playhead is easy to spot and grab
        head.addTriangle ((float) x - 6.0f, (float) ruler->getY() + 1.0f, (float) x + 6.0f, (float) ruler->getY() + 1.0f,
                          (float) x, (float) ruler->getY() + 11.0f);
        g.fillPath (head);
    }

    if (engine.getNumTracks() == 0)   // empty project: say what to do
    {
        auto area = juce::Rectangle<int> (railWidth, topBarHeight + rulerHeight, getWidth() - railWidth, getHeight() - topBarHeight - rulerHeight - bottomBarHeight);
        auto block = area.withSizeKeepingCentre (800, 292);
        g.setColour (Theme::background.withAlpha (0.85f));
        g.fillRoundedRectangle (block.toFloat().expanded (24.0f, 16.0f), 14.0f);

        g.setColour (Theme::yellow);
        g.setFont (juce::FontOptions (26.0f, juce::Font::bold));
        g.drawText ("Start making music", block.removeFromTop (40), juce::Justification::centred);
        g.setColour (Theme::text);
        g.setFont (juce::FontOptions (15.0f));
        g.drawText ("Drag audio files into this window, or add a track with the buttons at the top left:",
                    block.removeFromTop (28), juce::Justification::centred);
        block.removeFromTop (8);

        struct Chip { const char* button; const char* what; juce::Colour colour; };
        const Chip chips[] = { { "+ Track", "audio track - from a wav / mp3 / flac file", Theme::orange },
                               { "+ MIDI",  "synth track - play or draw notes in the piano roll", Theme::redBright },
                               { "+ Drums", "drum machine - click steps in a grid", Theme::yellow } };
        for (auto& c : chips)
        {
            auto row = block.removeFromTop (26);
            g.setColour (c.colour);
            g.fillEllipse ((float) row.getX() + 40.0f, (float) row.getCentreY() - 5.0f, 10.0f, 10.0f);
            g.setFont (juce::FontOptions (15.0f, juce::Font::bold));
            g.drawText (c.button, row.withTrimmedLeft (60).withWidth (80), juce::Justification::centredLeft);
            g.setColour (Theme::textDim);
            g.setFont (juce::FontOptions (15.0f));
            g.drawText (c.what, row.withTrimmedLeft (140), juce::Justification::centredLeft);
        }
        block.removeFromTop (6);
        g.setColour (Theme::yellow);
        g.setFont (juce::FontOptions (14.0f, juce::Font::bold));
        g.drawText ("Or start from a ready-made project: top-left menu > New from a template",
                    block.removeFromTop (26), juce::Justification::centred);
        g.setColour (Theme::textDim);
        g.setFont (juce::FontOptions (13.0f));
        g.drawText ("Sounds to play with: menu > Sample browser (a starter pack of drums, bass and loops, free to use)",
                    block.removeFromTop (22), juce::Justification::centred);
        block.removeFromTop (8);
        g.setColour (Theme::textFaint);
        g.setFont (juce::FontOptions (13.0f));
        g.drawText ("Space = play / pause    R = record    S = split    Ctrl+Z = undo    hover over a button for a hint",
                    block.removeFromTop (22), juce::Justification::centred);
    }
}

//==============================================================================
void MainComponent::splitAtPlayhead()
{
    const double t = engine.getPositionSeconds();
    beginEdit();
    bool changed = false;

    if (selTrack != nullptr && selClip >= 0)
    {
        changed = engine.splitClip (*selTrack, selClip, t) >= 0;
    }
    else
    {
        for (int i = 0; i < engine.getNumTracks(); ++i)
            if (auto* tr = engine.getTrack (i))
            {
                for (auto& c : engine.getClips (*tr))
                    changed = engine.splitClip (*tr, c.id, t) >= 0 || changed;
                for (auto& m : engine.getMidiClips (*tr))
                    changed = engine.splitClip (*tr, m.id, t) >= 0 || changed;
            }
    }

    if (! changed)
        cancelEdit();   // playhead wasn't inside any clip
    clipsChanged();
}

void MainComponent::deleteSelected()
{
    if (rollVisible && roll->deleteSelectedNote())   // a selected note goes before the clip
        return;

    if (selTrack != nullptr && selClip >= 0)
    {
        beginEdit();
        engine.deleteClip (*selTrack, selClip);
        selectClip (nullptr, -1);
        clipsChanged();
    }
}

void MainComponent::duplicateSelected()
{
    if (selTrack != nullptr && selClip >= 0)
    {
        beginEdit();
        const int id = engine.duplicateClip (*selTrack, selClip);
        if (id >= 0)
            selectClip (selTrack, id);
        clipsChanged();
    }
}

void MainComponent::copySelected (bool cut)
{
    if (selTrack == nullptr || selClip < 0)
        return;

    ClipboardItem item;
    if (engine.isMidiClip (*selTrack, selClip))
        item.valid = engine.getMidiClip (*selTrack, selClip, item.midi), item.isMidi = true;
    else
        item.valid = engine.getClip (*selTrack, selClip, item.audio);

    if (! item.valid)
        return;
    clipboard = std::move (item);

    if (cut)
    {
        beginEdit();
        engine.deleteClip (*selTrack, selClip);
        selectClip (nullptr, -1);
        clipsChanged();
    }
}

Track* MainComponent::pasteTargetTrack() const
{
    const auto type = clipboard.isMidi ? Track::Type::Midi : Track::Type::Audio;
    if (selTrack != nullptr && selTrack->type == type && ! engine.isMaster (*selTrack))
        return selTrack;
    for (int i = 0; i < engine.getNumTracks(); ++i)
        if (auto* t = engine.getTrack (i); t != nullptr && t->type == type)
            return t;
    return nullptr;
}

void MainComponent::pasteClip (Track* target, double at)
{
    if (! clipboard.valid || target == nullptr || engine.isMaster (*target))
        return;
    if (target->type != (clipboard.isMidi ? Track::Type::Midi : Track::Type::Audio))
        return;

    beginEdit();
    int id = -1;
    if (clipboard.isMidi)
    {
        id = engine.addMidiClip (*target, at, clipboard.midi.length);
        if (id >= 0)
            engine.setMidiNotes (*target, id, clipboard.midi.notes);
    }
    else
    {
        Clip c = clipboard.audio;
        c.takeGroup = 0;   // a pasted take is an ordinary clip
        c.takeIndex = 0;
        c.takeActive = true;
        c.start = juce::jmax (0.0, at);
        id = engine.addClip (*target, std::move (c));
    }

    if (id < 0)
    {
        cancelEdit();
        return;
    }
    selectClip (target, id);
    clipsChanged();
}

void MainComponent::showClipMenu (Track& track, int clipId, double at, juce::Component& source)
{
    if (engine.isMaster (track))
        return;

    const bool onClip = clipId >= 0;
    const bool canPaste = clipboard.valid && track.type == (clipboard.isMidi ? Track::Type::Midi : Track::Type::Audio);
    const double when = snapTime (at);

    juce::PopupMenu m;
    m.addItem (1, "Cut\tCtrl+X", onClip);
    m.addItem (2, "Copy\tCtrl+C", onClip);
    m.addItem (3, "Paste here\tCtrl+V", canPaste);
    m.addSeparator();
    m.addItem (4, "Split here\tS", onClip);
    m.addItem (5, "Duplicate\tCtrl+D", onClip);
    m.addItem (6, "Delete\tDel", onClip);
    if (onClip && track.type == Track::Type::Midi)
    {
        m.addSeparator();
        m.addItem (9, "Quantize / swing / humanize...\tQ");
    }
    Clip audioClip;
    if (onClip && track.type == Track::Type::Audio && engine.getClip (track, clipId, audioClip))
    {
        m.addSeparator();
        m.addItem (10, "Reverse", true, audioClip.reverse);
        m.addItem (11, "Crossfade with overlapping clip(s)");
        m.addItem (12, "Remove fades", audioClip.fadeIn > 0.0 || audioClip.fadeOut > 0.0);
        m.addSeparator();
        m.addItem (16, "Sample this clip (new sampler track)");
        m.addItem (13, "Gain, stretch and pitch...");
        m.addItem (17, "Warp (markers on the hits, drag them to the grid)...");
        m.addItem (14, "Edit volume curve", true, envelopeClipId == clipId && envelopeTrack == &track);
        m.addItem (15, "Remove volume curve", ! audioClip.envelope.empty());
    }

    // takes of a loop recording (or of a clip that was split from one): pick which plays
    std::vector<Clip> takes;
    if (onClip && track.type == Track::Type::Audio)
        takes = engine.getTakes (track, clipId);
    if (takes.size() > 1)
    {
        m.addSeparator();
        juce::PopupMenu takeMenu;
        for (size_t i = 0; i < takes.size(); ++i)
            takeMenu.addItem (100 + (int) i, "Take " + juce::String (takes[i].takeIndex), true, takes[i].takeActive);
        m.addSubMenu ("Choose take", takeMenu);
        m.addItem (30, "Comp takes (swipe over a take to play it there)");
        m.addItem (7, "Keep this take, drop the others");
        m.addItem (8, "Delete only this take");
    }

    juce::Component::SafePointer<MainComponent> safe (this);
    Track* tr = &track;
    juce::ignoreUnused (source);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea (
                         juce::Rectangle<int> (1, 1).withCentre (juce::Desktop::getMousePosition())),
                     [safe, tr, clipId, when, takes] (int r)
    {
        if (safe == nullptr || r == 0)
            return;
        auto& self = *safe;

        if (r >= 100 && r - 100 < (int) takes.size())
        {
            self.beginEdit();
            const int chosen = takes[(size_t) (r - 100)].id;
            self.engine.setActiveTake (*tr, chosen);
            self.selectClip (tr, chosen);
            self.clipsChanged();
            return;
        }

        switch (r)
        {
            case 7:
            case 8:
            {
                self.beginEdit();
                if (r == 7)
                    self.engine.flattenTakes (*tr, clipId);
                else
                {
                    self.engine.deleteTake (*tr, clipId);
                    self.selectClip (nullptr, -1);
                }
                self.clipsChanged();
                break;
            }
            case 1: self.copySelected (true); break;
            case 2: self.copySelected (false); break;
            case 3: self.pasteClip (tr, when); break;
            case 4:
            {
                self.beginEdit();
                if (self.engine.splitClip (*tr, clipId, when) < 0)
                    self.cancelEdit();
                self.clipsChanged();
                break;
            }
            case 5: self.duplicateSelected(); break;
            case 6: self.deleteSelected(); break;
            case 9: self.openQuantize (*tr, clipId); break;
            case 30:
            {
                double spanFrom = 1.0e12, spanTo = 0.0;   // the stretch the takes of this group cover
                for (auto& tk : takes)
                {
                    spanFrom = juce::jmin (spanFrom, tk.start);
                    spanTo = juce::jmax (spanTo, tk.end());
                }
                tr->audioCompView = true;
                tr->compFrom = spanFrom;
                tr->compTo = spanTo;
                self.clipsChanged();
                for (auto* lane : self.lanes)
                    lane->repaint();
                break;
            }
            case 10:
            {
                Clip c;
                if (self.engine.getClip (*tr, clipId, c))
                {
                    self.beginEdit();
                    self.engine.setClipReverse (*tr, clipId, ! c.reverse);
                    self.clipsChanged();
                }
                break;
            }
            case 11:
            {
                self.beginEdit();
                if (self.engine.crossfadeOverlaps (*tr, clipId) == 0)
                {
                    self.cancelEdit();
                    juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Crossfade",
                                                            "This clip does not overlap another clip on its track. Drag it over the end (or start) of a neighbour "
                                                            "so they overlap a little, then choose this again.");
                }
                self.clipsChanged();
                break;
            }
            case 12:
            {
                self.beginEdit();
                self.engine.setClipFades (*tr, clipId, 0.0, 0.0);
                self.clipsChanged();
                break;
            }
            case 13: self.openClipPanel (*tr, clipId); break;
            case 17: self.openWarpEditor (*tr, clipId); break;
            case 16: self.sampleClip (*tr, clipId); break;
            case 14:
            {
                const bool already = self.envelopeClipId == clipId && self.envelopeTrack == tr;
                self.envelopeTrack = already ? nullptr : tr;
                self.envelopeClipId = already ? -1 : clipId;
                self.repaint();
                self.laneHolder.repaint();
                break;
            }
            case 15:
            {
                self.beginEdit();
                self.engine.setClipEnvelope (*tr, clipId, {});
                self.clipsChanged();
                break;
            }
            default: break;
        }
    });
}

namespace
{
    // Computer keyboard as a piano: index in this string == semitones above the base C
    const char pianoKeyChars[] = "AWSEDFTGYHUJKOLP";
}

bool MainComponent::keyStateChanged (bool)
{
    if (! keysButton.getToggleState())
        return false;

    const auto mods = juce::ModifierKeys::currentModifiers;
    const bool blocked = mods.isCommandDown() || mods.isAltDown();
    bool handled = false;

    for (int i = 0; i < 16; ++i)
    {
        const bool down = ! blocked && juce::KeyPress::isKeyCurrentlyDown (pianoKeyChars[i]);
        if (down && ! pianoKeyDown[i])
        {
            pianoKeyDown[i] = true;
            pianoKeyPitch[i] = juce::jlimit (0, 127, 60 + 12 * pianoOctave + i);
            liveNote (pianoKeyPitch[i], true, 0.8f);
            handled = true;
        }
        else if (! down && pianoKeyDown[i])
        {
            pianoKeyDown[i] = false;
            liveNote (pianoKeyPitch[i], false, 0.0f);
            handled = true;
        }
    }
    return handled;
}

void MainComponent::focusLost (FocusChangeType)
{
    for (int i = 0; i < 16; ++i)   // don't leave notes hanging when keys are released elsewhere
        if (pianoKeyDown[i])
        {
            pianoKeyDown[i] = false;
            liveNote (pianoKeyPitch[i], false, 0.0f);
        }
}

// ---- The command list: what the palette (Ctrl+K) searches and what the keyboard shortcuts come from

void MainComponent::buildCommands()
{
    commands.clear();
    auto add = [this] (const char* id, const char* name, const char* category, juce::KeyPress key, std::function<void()> run,
                       std::function<bool()> enabled = nullptr, bool hidden = false)
    {
        Command c;
        c.id = id;
        c.name = name;
        c.category = category;
        c.defaultKey = key;
        c.run = std::move (run);
        c.enabled = std::move (enabled);
        c.hidden = hidden;
        commands.push_back (std::move (c));
    };
    using K = juce::KeyPress;
    const int ctrl = juce::ModifierKeys::ctrlModifier | 0, shift = juce::ModifierKeys::shiftModifier;
    const auto none = K();
    auto hasClip = [this] { return selTrack != nullptr && selClip >= 0; };
    auto hasAudioClip = [this] { Clip c; return selTrack != nullptr && selClip >= 0 && engine.getClip (*selTrack, selClip, c); };
    auto hasMidiClip = [this] { return selTrack != nullptr && selClip >= 0 && engine.isMidiClip (*selTrack, selClip); };

    // transport
    add ("play", "Play / pause", "Transport", K (K::spaceKey), [this] { if (engine.isPlaying()) pausePlayback(); else startPlayback(); });
    add ("rewind", "Back to the start", "Transport", K (K::returnKey), [this] { rewindButton.triggerClick(); });
    add ("record", "Record", "Transport", K ('R'), [this] { toggleRecording(); });
    add ("stopall", "STOP ALL (stops everything, cuts every sound)", "Transport", K (K::spaceKey, ctrl | shift, 0), [this] { stopAllButton.triggerClick(); });
    add ("loop", "Loop on / off", "Transport", K ('L'), [this] { loopButton.triggerClick(); });
    add ("loopsel", "Loop the selected clip", "Transport", K ('L', shift, 0), [this] { loopToSelection(); }, hasClip);
    add ("metronome", "Metronome (click) on / off", "Transport", K ('M'), [this]
        {
            metroButton.setToggleState (! metroButton.getToggleState(), juce::dontSendNotification);
            engine.metronome = metroButton.getToggleState();
        });
    add ("punch", "Punch in / out on / off", "Transport", K ('P'), [this] { punchButton.setToggleState (! punchButton.getToggleState(), juce::dontSendNotification); togglePunch(); });
    add ("countin", "Count-in: change the number of bars", "Transport", none, [this] { countInButton.triggerClick(); });
    add ("follow", "Follow the playhead on / off", "Transport", none, [this] { followButton.triggerClick(); });
    add ("snap", "Snap to the grid on / off", "Transport", none, [this] { snapButton.triggerClick(); });
    add ("pianokeys", "Computer keyboard as a piano on / off", "Transport", none, [this] { keysButton.triggerClick(); });

    // editing
    add ("undo", "Undo", "Edit", K ('Z', ctrl, 0), [this] { undo(); });
    add ("redo", "Redo", "Edit", K ('Y', ctrl, 0), [this] { redo(); });
    add ("duplicate", "Duplicate the selected clip", "Edit", K ('D', ctrl, 0), [this] { duplicateSelected(); }, hasClip);
    add ("cut", "Cut the selected clip", "Edit", K ('X', ctrl, 0), [this] { copySelected (true); }, hasClip);
    add ("copy", "Copy the selected clip", "Edit", K ('C', ctrl, 0), [this] { copySelected (false); }, hasClip);
    add ("paste", "Paste the clip at the playhead", "Edit", K ('V', ctrl, 0), [this] { pasteClip (pasteTargetTrack(), engine.getPositionSeconds()); });
    add ("delete", "Delete the selected clip", "Edit", K (K::deleteKey), [this] { deleteSelected(); });
    add ("delete2", "Delete the selected clip (second key)", "Edit", K (K::backspaceKey), [this] { deleteSelected(); }, nullptr, true);
    add ("split", "Split at the playhead", "Edit", K ('S'), [this] { splitAtPlayhead(); });
    add ("quantize", "Quantize / swing / humanize the MIDI clip", "Edit", K ('Q'), [this] { if (selTrack != nullptr) openQuantize (*selTrack, selClip); }, hasMidiClip);
    add ("clipgain", "Gain, stretch and pitch of the audio clip...", "Edit", none, [this] { openClipPanel (*selTrack, selClip); }, hasAudioClip);
    add ("warp", "Warp the audio clip (markers on the hits)...", "Edit", none, [this] { openWarpEditor (*selTrack, selClip); }, hasAudioClip);

    // file
    add ("new", "New project", "File", K ('N', ctrl, 0), [this] { newProject(); });
    add ("open", "Open a project...", "File", K ('O', ctrl, 0), [this] { openButton.triggerClick(); });
    add ("save", "Save", "File", K ('S', ctrl, 0), [this] { saveButton.triggerClick(); });
    add ("saveas", "Save as...", "File", none, [this] { saveProjectAs(); });
    add ("newversion", "Save a new version (v2, v3...)", "File", none, [this] { saveNewVersion(); });
    add ("collect", "Save with all audio in one folder...", "File", none, [this] { collectProject(); });
    add ("history", "Version history...", "File", none, [this] { openVersionHistory(); });
    add ("exportaudio", "Export audio (WAV, FLAC, MP3...)...", "File", none, [this] { exportDialog(); });
    add ("exportstems", "Export stems...", "File", none, [this] { exportStemsDialog(); });
    add ("closetab", "Close this project (tab)", "File", K ('W', ctrl, 0), [this] { if (requestCloseTab) requestCloseTab(); }, [this] { return (bool) requestCloseTab; });
    add ("nexttab", "Next project (tab)", "File", K (K::tabKey, ctrl, 0), [this] { if (requestSwitchTab) requestSwitchTab (1); }, [this] { return (bool) requestSwitchTab; });
    add ("prevtab", "Previous project (tab)", "File", K (K::tabKey, ctrl | shift, 0), [this] { if (requestSwitchTab) requestSwitchTab (-1); }, [this] { return (bool) requestSwitchTab; });

    // tracks
    add ("addaudio", "Add an audio track", "Tracks", none, [this] { addEmptyAudioTrack(); });
    add ("addaudiofile", "Add audio track(s) from file(s)...", "Tracks", none, [this] { addTrackDialog(); });
    add ("addmidi", "Add a MIDI track", "Tracks", none, [this] { addMidiTrack (false); });
    add ("adddrums", "Add a drum track", "Tracks", none, [this] { addMidiTrack (true); });
    add ("addsampler", "Add a sampler track", "Tracks", none, [this] { addSamplerTrack(); });
    add ("addbus", "Add a bus (group / effects return)", "Tracks", none, [this] { addBusTrack(); });

    // windows and panels
    add ("mixer", "Open the mixer", "Windows", none, [this] { openMixer(); });
    add ("midilearn", "MIDI controllers: tie knobs and faders to parameters (MIDI learn)...", "Windows", none, [this] { openMidiLearn(); });
    add ("samples", "Sample browser: the starter pack and your own samples, with preview...", "Windows", none, [this] { openSampleBrowser(); });
    add ("meter", "Master meter: loudness (LUFS) and spectrum...", "Windows", none, [this] { openMasterMeter(); });
    add ("refab", "Reference track: switch between your mix and the reference", "Windows", K ('B'), [this]
        {
            if (engine.hasReference())
                engine.setReferenceOn (! engine.isReferenceOn());
            else
                openMasterMeter();   // (no reference yet: the window where one is loaded)
        });
    add ("tuner", "Open the tuner", "Windows", none, [this] { openTuner(); });
    add ("plugins", "Plugins (scan, manage)...", "Windows", none, [this] { pluginsButton.triggerClick(); });
    add ("audiosettings", "Audio settings (driver, interface, buffer)...", "Windows", none, [this] { openAudioSettings(); });
    add ("tempo", "Tempo and time signature...", "Windows", none, [this] { openTempoPanel(); });
    add ("taptempo", "Tap tempo", "Transport", K ('T'), [this] { tapTempo(); });
    add ("sections", "Sections of the arrangement (verse, chorus...)...", "Windows", none, [this] { openSectionsPanel(); });
    add ("zoomin", "Zoom in", "View", none, [this] { zoomBy (1.4, laneViewport.getMaximumVisibleWidth() / 2); });
    add ("zoomout", "Zoom out", "View", none, [this] { zoomBy (1.0 / 1.4, laneViewport.getMaximumVisibleWidth() / 2); });
    add ("tour", "Quick tour of the window", "Help", none, [this] { startTour(); });
    add ("palette", "Command palette (this list)", "Help", K ('K', ctrl, 0), [this] { openPalette(); });
    add ("shortcuts", "Keyboard shortcuts: see and change...", "Help", none, [this] { openShortcuts(); });
}

// The master meter (loudness, spectrum) and the reference track
// ---- updates: the latest release on GitHub

void MainComponent::checkForUpdates (bool userAsked)
{
    juce::Component::SafePointer<MainComponent> safe (this);
    UiSettings::lastUpdateCheck = juce::Time::currentTimeMillis();
    saveUiSettings();
    // (UFODAW_VERSION_OVERRIDE: pretend to be another version, to try this out)
    UpdateCheck::checkAsync (juce::SystemStats::getEnvironmentVariable ("UFODAW_VERSION_OVERRIDE", JUCE_APPLICATION_VERSION_STRING), [safe, userAsked] (UpdateCheck::Result r)
    {
        if (safe == nullptr)
            return;
        auto& self = *safe;
        if (r.ok && r.newer)
        {
            self.updateVersion = r.version;
            self.updateNotes = r.notes;
            self.updateDownload = r.downloadUrl;
            self.resized();
            self.updateButton.setTooltip ("UFODAW " + r.version + " is available. Click to see what is new and to download it.");
            if (userAsked)
                self.showUpdateInfo();
        }
        else if (userAsked)
        {
            juce::AlertWindow::showMessageBoxAsync (r.ok ? juce::MessageBoxIconType::InfoIcon : juce::MessageBoxIconType::WarningIcon,
                                                    r.ok ? "You are up to date" : "Could not check for updates",
                                                    r.ok ? "UFODAW " + juce::String (JUCE_APPLICATION_VERSION_STRING) + " is the newest version."
                                                         : "No answer from GitHub. Are you online? You can also look at\nhttps://github.com/Thulehunter/ufodaw/releases");
        }
    });
}

void MainComponent::showUpdateInfo()
{
    if (updateVersion.isEmpty())
        return;
    // the release notes are markdown: plain text lines of at most about 90 characters, the first ones only
    juce::StringArray shownLines;
    for (auto line : juce::StringArray::fromLines (updateNotes))
    {
        line = line.replace ("**", "").replace ("`", "").trim();
        if (line.startsWithChar ('#'))
            line = line.trimCharactersAtStart ("# ").toUpperCase();
        else if (line.startsWith ("- "))
            line = juce::String (juce::CharPointer_UTF8 ("\xe2\x80\xa2 ")) + line.substring (2);
        if (line.isEmpty() || line.startsWithIgnoreCase ("DOWNLOAD"))
            continue;
        while (line.length() > 90)   // wrap at a space
        {
            int cut = line.substring (0, 90).lastIndexOfChar (' ');
            if (cut < 40)
                cut = 90;
            shownLines.add (line.substring (0, cut));
            line = "   " + line.substring (cut).trim();
        }
        shownLines.add (line);
    }
    if (shownLines.size() > 14)
    {
        shownLines.removeRange (14, shownLines.size() - 14);
        shownLines.add (juce::String (juce::CharPointer_UTF8 ("\xe2\x80\xa6")));
    }
    const auto notes = shownLines.joinIntoString ("\n");
    const auto download = updateDownload;
    juce::AlertWindow::showAsync (juce::MessageBoxOptions::makeOptionsOkCancel (juce::MessageBoxIconType::InfoIcon, "UFODAW " + updateVersion + " is available",
                                      "You have " + juce::SystemStats::getEnvironmentVariable ("UFODAW_VERSION_OVERRIDE", JUCE_APPLICATION_VERSION_STRING) + ".\n\n" + notes + "\n\nDownload the new version? Your projects open as before.",
                                      "Download", "Later", this),
                                  [download] (int result)
    {
        if (result == 1)
            juce::URL (download).launchInDefaultBrowser();
    });
}

void MainComponent::openSampleBrowser()
{
    if (browserWindow != nullptr)
    {
        browserWindow->toFront (true);
        return;
    }
    juce::Component::SafePointer<MainComponent> safe (this);
    SampleBrowserPanel::Hooks hooks;
    hooks.addToProject = [safe] (const juce::File& f) { if (safe != nullptr) safe->importFiles (juce::StringArray (f.getFullPathName()), nullptr, 0.0); };
    auto closeIt = [safe] { juce::MessageManager::callAsync ([safe] { if (safe != nullptr) safe->browserWindow.reset(); }); };
    browserWindow = std::make_unique<SampleBrowserWindow> (new SampleBrowserPanel (engine, std::move (hooks)), closeIt);
}

void MainComponent::openMasterMeter()
{
    if (meterWindow != nullptr)
    {
        meterWindow->toFront (true);
        return;
    }
    juce::Component::SafePointer<MainComponent> safe (this);
    auto closeIt = [safe] { juce::MessageManager::callAsync ([safe] { if (safe != nullptr) safe->meterWindow.reset(); }); };
    meterWindow = std::make_unique<MasterMeterWindow> (engine, [safe] { if (safe != nullptr) ++safe->editCounter; }, closeIt);
}

void MainComponent::openPalette()
{
    if (palette != nullptr)
        return;
    std::vector<CommandPalette::Entry> entries;
    for (auto& c : commands)
    {
        if (c.hidden)
            continue;
        CommandPalette::Entry e;
        e.name = c.name;
        e.category = c.category;
        e.shortcut = Shortcuts::describe (Shortcuts::keyFor (c));
        e.run = c.run;
        e.enabled = c.isEnabled();
        entries.push_back (std::move (e));
    }
    juce::Component::SafePointer<MainComponent> safe (this);
    palette = std::make_unique<CommandPalette> (std::move (entries), [safe]
    {
        if (safe == nullptr)
            return;
        safe->palette.reset();
        safe->grabKeyboardFocus();
    });
    addAndMakeVisible (*palette);
    palette->setBounds (getLocalBounds());
    palette->toFront (true);
    palette->focusInput();
}

void MainComponent::openShortcuts()
{
    if (shortcutsWindow != nullptr)
    {
        shortcutsWindow->toFront (true);
        return;
    }
    juce::Component::SafePointer<MainComponent> safe (this);
    auto closeIt = [safe] { juce::MessageManager::callAsync ([safe] { if (safe != nullptr) safe->shortcutsWindow.reset(); }); };
    shortcutsWindow = std::make_unique<ShortcutsWindow> (new ShortcutsPanel (commands, [] {}, closeIt), closeIt);
}

bool MainComponent::keyPressed (const juce::KeyPress& k)
{
    const auto mods = k.getModifiers();
    const auto ch = juce::CharacterFunctions::toLowerCase (k.getTextCharacter());

    if (keysButton.getToggleState() && ! mods.isCommandDown() && ! mods.isAltDown())
    {
        const int code = k.getKeyCode();
        if (code == 'Z') { pianoOctave = juce::jmax (-3, pianoOctave - 1); return true; }
        if (code == 'X') { pianoOctave = juce::jmin (3, pianoOctave + 1); return true; }
        for (int i = 0; i < 16; ++i)
            if (code == pianoKeyChars[i])
                return true;   // played in keyStateChanged(); not a shortcut
    }

    // Ctrl+Z / Ctrl+Y can arrive as control characters (kept as they are)
    if (mods.isCommandDown())
    {
        if (k.getKeyCode() == 26) { undo(); return true; }
        if (k.getKeyCode() == 25) { redo(); return true; }
        if (k.getKeyCode() == 'Z' && mods.isShiftDown()) { redo(); return true; }
    }

    // everything else comes from the command list: the key each command has now (its default, or the one the user chose)
    for (auto& c : commands)
        if (Shortcuts::matches (k, Shortcuts::keyFor (c)))
        {
            if (c.isEnabled() && c.run)
                c.run();
            return true;
        }

    if (k == juce::KeyPress::escapeKey && envelopeClipId >= 0)   // done editing the volume curve
    {
        envelopeClipId = -1;
        envelopeTrack = nullptr;
        laneHolder.repaint();
        return true;
    }
    juce::ignoreUnused (ch);    return false;
}

// The mappings of MIDI learn: a controller that moved moves its parameter (also what the learn window waits for)
void MainComponent::applyMidiMappings()
{
    if (onCcLearned && ccLearnedNumber.load() >= 0)
    {
        const int cc = ccLearnedNumber.exchange (-1), channel = ccLearnedChannel.load();
        auto callback = std::move (onCcLearned);
        onCcLearned = nullptr;
        if (callback)
            callback (cc, channel);
    }

    for (auto& mp : midiMappings)
    {
        if (mp.cc < 0 || mp.cc > 127)
            continue;
        Track* track = mp.trackIndex < 0 ? &engine.getMaster() : engine.getTrack (mp.trackIndex);
        if (track == nullptr)
            continue;
        for (int ch = 0; ch < 16; ++ch)
        {
            if (mp.channel > 0 && mp.channel - 1 != ch)
                continue;
            const auto stamp = ccStamp[(size_t) (ch * 128 + mp.cc)].load();
            if (stamp == mp.seen[ch])
                continue;
            mp.seen[ch] = stamp;
            engine.setParameterNormalised (*track, mp.id, (double) ccValue[(size_t) (ch * 128 + mp.cc)].load() / 127.0);
        }
    }
}

void MainComponent::openMidiLearn()
{
    if (learnWindow != nullptr)
    {
        learnWindow->toFront (true);
        return;
    }
    juce::Component::SafePointer<MainComponent> safe (this);
    MidiLearnPanel::Hooks hooks;
    hooks.startLearn = [safe] (std::function<void (int, int)> done)
    {
        if (safe == nullptr)
            return;
        safe->ccLearnedNumber = -1;
        safe->onCcLearned = std::move (done);
        safe->ccLearning = true;
    };
    hooks.stopLearn = [safe] { if (safe != nullptr) { safe->ccLearning = false; safe->onCcLearned = nullptr; } };
    hooks.changed = [safe] { if (safe != nullptr) ++safe->editCounter; };
    auto closeIt = [safe] { juce::MessageManager::callAsync ([safe] { if (safe != nullptr) { safe->ccLearning = false; safe->onCcLearned = nullptr; safe->learnWindow.reset(); } }); };
    learnWindow = std::make_unique<MidiLearnWindow> (new MidiLearnPanel (engine, midiMappings, std::move (hooks)), closeIt);
}

void MainComponent::timerCallback()
{
    applyMidiMappings();
    if (updateVersion.isNotEmpty() != updateButton.isVisible())   // (a check that finished while another tab was shown)
    {
        updateButton.setTooltip ("UFODAW " + updateVersion + " is available. Click to see what is new and to download it.");
        resized();
    }
    if (juce::String crashed; engine.takeCrashedPlugin (crashed))   // a plugin crashed while playing: it was caught and switched off
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "A plugin crashed",
                                                crashed + " crashed while it was playing. UFODAW caught it and switched the plugin off, so the program could go on: "
                                                          "an effect now lets the sound through, an instrument is silent.\n\nSave your project and open it again to load the plugin again. "
                                                          "If it keeps happening, the plugin has a problem (an update may help).");
    if (++subAutoTicks % 20 == 0)
        autoSubLanes();   // about once a second: a drum plugin may just have been loaded
    {
        const int takesBefore = engine.recordedTakeCounter;
        engine.automationRecordTick();   // rides of armed automation lanes become takes
        if (engine.recordedTakeCounter != takesBefore)
            ++editCounter;                // (unsaved changes, autosave)
        for (auto* lane : lanes)
        {
            lane->checkArmed();
            lane->tickLive();
        }
        static int armedTicks = 0;
        if (engine.anyAutomationArmed() && (++armedTicks % 3 == 0 || engine.recordedTakeCounter != takesBefore))
            for (auto* lane : lanes)   // the REC mark and the new takes show up while riding
                if (lane->getTrack().autoEditId.isNotEmpty())
                    lane->repaint();
    }
    const double p = engine.getPositionSeconds();
    followPlayhead (p);
    const int mins = (int) (p / 60.0);
    refreshLiveTargets();   // picks up red-dot arming and input choices made anywhere
    updateClipLamp();

    if (juce::Time::getMillisecondCounter() - lastAutosaveMs > 120000u)   // every two minutes
    {
        lastAutosaveMs = juce::Time::getMillisecondCounter();
        autosaveNow();
    }
    if (juce::Time::getMillisecondCounter() - lastTimedBackupMs > 600000u)   // every ten minutes: a version of the unsaved work, in the history
    {
        lastTimedBackupMs = juce::Time::getMillisecondCounter();
        if (currentProject != juce::File() && isDirty() && editCounter != backedUpEdit && ! engine.isRecording() && ! midiRecording)
        {
            writeBackup (currentProject);
            backedUpEdit = editCounter;
        }
    }

    if (engine.isCountingIn())
        timeLabel.setText ("COUNT  " + juce::String (engine.countInBeatsLeft()), juce::dontSendNotification);
    else
        timeLabel.setText (juce::String::formatted ("%02d:%06.3f", mins, p - mins * 60.0), juce::dontSendNotification);

    if (engine.isRecording() && engine.punchEnabled && engine.punchFinished())
        finishRecording();   // the punch-out has passed: close the recording, playback carries on

    for (auto* h : headers)
        h->tick();
    masterHeader->tick();

    ++timerTick;
    const bool moved = p != lastTimerPosition;
    const auto maskLow = liveMaskLow.load(), maskHigh = liveMaskHigh.load();
    const bool keysChanged = maskLow != lastMaskLow || maskHigh != lastMaskHigh;
    lastTimerPosition = p;
    lastMaskLow = maskLow;
    lastMaskHigh = maskHigh;

    if (timerTick % 10 == 0 || moved)
        masterLane->repaint();   // the master lane shows its automation and follows the zoom

    if (rollVisible)
    {
        if (roll->stillValid())
        {
            if (moved || keysChanged || engine.isPlaying() || timerTick % 15 == 0)
                roll->repaintGrid();   // follows the playhead and the keys that are played
        }
        else
        {
            hideRoll();
        }
    }

    const bool recording = engine.isRecording() || midiRecording;
    playButton.setLit (engine.isPlaying());
    pauseButton.setLit (! engine.isPlaying() && p > 0.0);
    recordButton.setLit (recording && (juce::Time::getMillisecondCounter() / 500) % 2 == 0);   // blinks

    // The playhead is the only thing on the main view that moves by itself: repaint its strip (where it was and where it is),
    // not the whole window. Everything else repaints when it changes.
    const int playheadX = laneViewport.getX() + (int) (p * pixelsPerSecond) - getScrollX();
    if (playheadX != lastPlayheadX)
    {
        const int top = ruler->getY(), height = masterArea.getBottom() - top;
        repaint (lastPlayheadX - 8, top, 16, height);
        repaint (playheadX - 8, top, 16, height);
        lastPlayheadX = playheadX;
    }

    // the processor load of the audio device, in the status line (about once a second)
    if (timerTick % 30 == 0)
    {
        const int cpu = (int) std::lround (deviceManager.getCpuUsage() * 100.0);
        if (cpu != cpuPercentShown)
        {
            cpuPercentShown = cpu;
            updateAudioInfo();   // rebuilds the status text, which includes the load
        }
    }
}

//==============================================================================
// Transport. Play/Pause keep the position; Stop returns to where playback started
// (or to the beginning if the transport is already stopped).
void MainComponent::startPlayback()
{
    if (! engine.isPlaying())
    {
        playStartSeconds = engine.getPositionSeconds();
        engine.clearClipIndicators();   // a fresh run: only clipping from now on counts
        engine.play();
    }
}

void MainComponent::pausePlayback()
{
    finishRecording();
    engine.stop();
}

void MainComponent::stopPlayback()
{
    finishRecording();
    if (engine.isPlaying())
    {
        engine.stop();
        engine.setPositionSeconds (playStartSeconds);
    }
    else
    {
        engine.setPositionSeconds (0.0);
        playStartSeconds = 0.0;
    }
}

void MainComponent::toggleRecording()
{
    if (engine.isRecording() || midiRecording)
    {
        finishRecording();   // keeps playing, like most DAWs
        return;
    }

    // an armed audio track (red dot) takes the recording; otherwise it becomes a new track
    Track* audioArmed = nullptr;
    for (int i = 0; i < engine.getNumTracks() && audioArmed == nullptr; ++i)
        if (auto* candidate = engine.getTrack (i); candidate->type == Track::Type::Audio && candidate->recordArmed)
            audioArmed = candidate;

    // MIDI tracks are armed and no audio track is: record what is played, not the audio input
    if (audioArmed == nullptr && beginMidiRecording (true))
    {
        recStartSeconds = engine.getPositionSeconds();
        if (! engine.isPlaying())
            engine.startCountIn (UiSettings::countInBars);   // clicks first; the notes are recorded once the transport runs
        startPlayback();
        return;
    }

    auto* device = deviceManager.getCurrentAudioDevice();
    const int inputs = device != nullptr ? device->getActiveInputChannels().countNumberOfSetBits() : 0;
    if (inputs == 0)
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "No audio input",
                                                "No input device is available. Check your microphone or interface and "
                                                "Windows' microphone privacy settings.");
        return;
    }

    // Every armed audio track records its own file from its own input; with none armed, inputs 1+2 go to a new track.
    const auto folder = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("UFODAW Recordings");
    const auto stamp = juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H-%M-%S");

    std::vector<Engine::RecordTarget> targets;
    recordings.clear();
    for (int i = 0; i < engine.getNumTracks(); ++i)
        if (auto* t = engine.getTrack (i); t->type == Track::Type::Audio && t->recordArmed)
        {
            Engine::RecordTarget target;
            target.firstChannel = juce::jlimit (0, inputs - 1, t->inputFirst);               // clamp to the inputs that are switched on
            target.numChannels = juce::jlimit (1, juce::jmin (2, inputs - target.firstChannel), t->inputCount);
            target.file = folder.getChildFile ("Recording " + stamp + " - " + juce::File::createLegalFileName (t->name) + ".wav");
            targets.push_back (target);
            recordings.push_back ({ t, target.file });
        }

    if (targets.empty())
    {
        Engine::RecordTarget target;
        target.numChannels = juce::jmin (inputs, 2);
        target.file = folder.getChildFile ("Recording " + stamp + ".wav");
        targets.push_back (target);
        recordings.push_back ({ nullptr, target.file });
    }

    if (! engine.startRecording (targets))
    {
        recordings.clear();
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Cannot record",
                                                "Could not create the recording files in " + folder.getFullPathName());
        return;
    }

    beginMidiRecording (false);   // MIDI tracks armed with their red dot record at the same time as the audio

    // Punch: from a stop (or from after the region) start a bar ahead of it, so there is a run-up.
    if (engine.punchEnabled && engine.punchOut > engine.punchIn && ! engine.isPlaying()
        && engine.getPositionSeconds() > engine.punchIn - 0.001)
        setPlayhead (juce::jmax (0.0, tempoMap.barStartSeconds (juce::jmax (0, tempoMap.barAtSeconds (engine.punchIn) - 1))));

    recStartSeconds = engine.getPositionSeconds();

    // Recording while looping: every pass of the loop becomes a take (cut up when the recording ends).
    recLoopStart = engine.loopStart;
    recLoopEnd = engine.loopEnd;
    recLoopTakes = ! engine.punchEnabled && engine.loopEnabled && recLoopEnd > recLoopStart + 0.05 && recStartSeconds < recLoopEnd;

    if (! engine.isPlaying())
        engine.startCountIn (UiSettings::countInBars);   // the clicks come first, then the recording starts at the playhead
    startPlayback();   // recording only captures while the transport runs
}

// Turns the notes played during MIDI recording into a clip on the armed track, starting on the bar
// where recording began and lasting a whole number of bars.
void MainComponent::finishMidiRecording()
{
    midiRecording = false;
    const double now = engine.getPositionSeconds();

    std::vector<MidiRec> recs;
    {
        const juce::ScopedLock sl (midiRecLock);
        for (auto& r : midiRecs)
        {
            for (auto& h : r.held)   // notes still held when recording stops end here
            {
                Note n;
                n.pitch = h.pitch;
                n.start = h.start;
                n.length = juce::jmax (0.02, now - h.start);
                n.velocity = h.velocity;
                r.notes.push_back (n);
            }
            r.held.clear();
        }
        recs.swap (midiRecs);
    }

    // every track that recorded something gets a clip, starting on the bar where recording began and lasting whole bars
    const int startBar = tempoMap.barAtSeconds (recStartSeconds);
    const double clipStart = tempoMap.barStartSeconds (startBar);

    beginEdit();
    Track* selectTrack = nullptr;
    int selectId = -1;
    for (auto& r : recs)
    {
        if ((r.notes.empty() && r.ccs.empty()) || r.track == nullptr || engine.indexOfTrack (r.track) < 0)
            continue;

        double end = 0.0;
        for (auto& n : r.notes)
        {
            n.start = juce::jmax (0.0, n.start - clipStart);
            end = juce::jmax (end, n.start + n.length);
        }
        for (auto& cc : r.ccs)
        {
            cc.time = juce::jmax (0.0, cc.time - clipStart);
            end = juce::jmax (end, cc.time);
        }

        const double absoluteEnd = clipStart + end;
        int endBar = tempoMap.barAtSeconds (absoluteEnd);
        if (absoluteEnd > tempoMap.barStartSeconds (endBar) + 1.0e-6)
            ++endBar;
        const double clipLength = tempoMap.barStartSeconds (juce::jmax (startBar + 1, endBar)) - clipStart;

        const int id = engine.addMidiClip (*r.track, clipStart, clipLength);
        engine.setMidiNotes (*r.track, id, std::move (r.notes));
        engine.setMidiCcs (*r.track, id, std::move (r.ccs));
        if (selectTrack == nullptr)
        {
            selectTrack = r.track;
            selectId = id;
        }
    }

    if (selectTrack == nullptr)
    {
        cancelEdit();   // nothing was played
        return;
    }
    selectClip (selectTrack, selectId);
    clipsChanged();
}

// MIDI tracks that record: the ones armed with their red dot, and (when asked) the selected track.
bool MainComponent::beginMidiRecording (bool includeSelected)
{
    std::vector<MidiRec> recs;
    auto add = [&] (Track* t)
    {
        if (t == nullptr || t->type != Track::Type::Midi)
            return;
        for (auto& r : recs)
            if (r.track == t)
                return;
        MidiRec r;
        r.track = t;
        recs.push_back (std::move (r));
    };
    if (includeSelected)
        add (armedTrack.load());
    for (int i = 0; i < engine.getNumTracks(); ++i)
        if (auto* t = engine.getTrack (i); t != nullptr && t->midiArmed)
            add (t);

    if (recs.empty())
        return false;

    {
        const juce::ScopedLock sl (midiRecLock);
        midiRecs = std::move (recs);
    }
    recStartSeconds = engine.getPositionSeconds();
    refreshLiveTargets();   // (the selected track must be a live target, whatever the timer did last)
    midiRecording = true;
    return true;
}

// Closes the recording file and puts it on the armed audio track (or a new track) where recording began.
void MainComponent::finishRecording()
{
    if (midiRecording)
        finishMidiRecording();   // (audio that is recorded at the same time is finished below)

    if (! engine.isRecording())
        return;

    // the first recorded sample is where the clip starts (with punch that is the punch-in, or later)
    const double recordedStart = engine.recordedStartSeconds();
    const double sampleRate = engine.getSampleRate();

    // Latency compensation: what was recorded came back through the interface later than it was played, so the clip is
    // placed earlier by that round trip. At the very start of the project the part that would lie before 0 is trimmed.
    const double rawStart = (recordedStart >= 0.0 ? recordedStart : recStartSeconds) - recordLatencySeconds();
    const double clipStartSeconds = juce::jmax (0.0, rawStart);
    const auto trimSamples = (juce::int64) std::llround (juce::jmax (0.0, -rawStart) * sampleRate);
    engine.stopRecording();

    // each recorded file becomes a clip where recording began: on its armed track, or on a new track
    beginEdit();
    bool anyClip = false;
    for (auto& rec : recordings)
    {
        Track* t = rec.track;
        const bool intoArmedTrack = t != nullptr && engine.indexOfTrack (t) >= 0 && t->type == Track::Type::Audio;
        if (! intoArmedTrack)
            t = engine.addTrack (rec.file.getFileNameWithoutExtension());

        int id = engine.addClipFromFile (*t, rec.file, clipStartSeconds);
        if (id < 0)
        {
            if (! intoArmedTrack)
                engine.removeTrack (t);
            rec.file.deleteFile();
            continue;
        }

        Clip whole;
        if (trimSamples > 0 && engine.getClip (*t, id, whole))   // the start of the file lies before the beginning of the project
        {
            const double trim = (double) trimSamples / sampleRate;
            engine.setClipRegion (*t, id, 0.0, trim, juce::jmax (0.01, whole.sourceLength() - trim));
        }

        // A loop recording is cut at the loop end into one take per pass; the newest plays, the others can be chosen.
        if (recLoopTakes && engine.getClip (*t, id, whole) && whole.audio != nullptr)
        {
            const double sr = sampleRate;
            const auto loopStartS = (juce::int64) (recLoopStart * sr), loopEndS = (juce::int64) (recLoopEnd * sr);
            const auto passS = loopEndS - loopStartS;
            const auto firstS = loopEndS - (juce::int64) std::llround (rawStart * sr);   // file sample where the first pass ends
            const auto totalS = (juce::int64) whole.audio->getNumSamples();

            if (passS > 0 && firstS > trimSamples && totalS > firstS + (juce::int64) (0.05 * sr))
            {
                std::vector<Clip> takes;
                auto take = whole;
                take.length = (double) (firstS - trimSamples) / sr;   // the first pass: from where recording began to the loop end
                takes.push_back (take);
                for (juce::int64 off = firstS; off < totalS; off += passS)
                {
                    const auto len = juce::jmin (passS, totalS - off);
                    if (len < (juce::int64) (0.05 * sr))
                        break;
                    take = whole;
                    take.start = recLoopStart;
                    take.offset = (double) off / sr;
                    take.length = (double) len / sr;
                    takes.push_back (take);
                }
                if (takes.size() > 1)
                {
                    engine.deleteClip (*t, id);
                    id = engine.addTakes (*t, std::move (takes));
                }
            }
        }

        if (! intoArmedTrack)
            addTrackUI (*t);
        selectClip (t, id);
        anyClip = true;
    }
    recordings.clear();

    if (! anyClip)
        cancelEdit();
    clipsChanged();
}

//==============================================================================
// "+ Track" asks what kind of audio track: a new empty one (to record on or drop files into), or one per file.
void MainComponent::addTrackMenu()
{
    juce::PopupMenu menu;
    menu.addItem (1, "Audio track");
    menu.addItem (2, "Audio track from file(s)...");
    menu.addItem (3, "MIDI track (built-in synth or VST)");
    menu.addItem (4, "Drum track");
    menu.addItem (5, "Sampler (plays one sample)");
    menu.addSeparator();
    menu.addItem (6, "Bus (group / effects return)");
    const auto templates = templatesFolder().findChildFiles (juce::File::findFiles, false, "*.mdaw");
    juce::PopupMenu templateMenu;
    for (int i = 0; i < templates.size(); ++i)
        templateMenu.addItem (200 + i, templates[i].getFileNameWithoutExtension());
    menu.addSeparator();
    menu.addSubMenu ("From a template", templateMenu, ! templates.isEmpty());
    menu.addItem (7, "Show the templates folder (right-click a track > Save as a template)");
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&railAdd), [this, templates] (int result)
    {
        if (result >= 200 && result - 200 < templates.size())
            addFromTemplate (templates[result - 200]);
        if (result == 7)
        {
            templatesFolder().createDirectory();
            templatesFolder().revealToUser();
        }
        if (result == 1) addEmptyAudioTrack();
        if (result == 2) addTrackDialog();
        if (result == 3) addMidiTrack (false);
        if (result == 4) addMidiTrack (true);
        if (result == 5) addSamplerTrack();
        if (result == 6) addBusTrack();
    });
}

void MainComponent::addBusTrack()
{
    int count = 1;
    for (int i = 0; i < engine.getNumTracks(); ++i)
        if (engine.getTrack (i)->bus)
            ++count;

    beginEdit();
    auto* t = engine.addBus ("Bus " + juce::String (count));
    addTrackUI (*t);
    clipsChanged();
}

void MainComponent::openRouting (Track& t, juce::Component& anchor)
{
    juce::CallOutBox::launchAsynchronously (std::make_unique<RoutingPanel> (engine, t, [this] { beginEdit(); }),
                                            anchor.getScreenBounds(), nullptr);
}

// Time-stretch / pitch-shift runs behind a progress window with a Cancel button (a long clip takes a few seconds).
class ClipProcessJob : public juce::ThreadWithProgressWindow
{
public:
    ClipProcessJob (MainComponent& m, Engine& e, Track& t, int id, double stretchRatio, double semitoneShift)
        : juce::ThreadWithProgressWindow ("Processing audio", true, true, 5000, "Cancel", &m),
          main (&m), engine (e), track (t), clipId (id), stretch (stretchRatio), semitones (semitoneShift)
    {
    }

    void run() override
    {
        ok = engine.processClip (track, clipId, stretch, semitones, [this] (double fraction)
        {
            setProgress (fraction);
            return ! threadShouldExit();
        });
    }

    // Message thread, when it is done or cancelled (this object deletes itself here, like the stem export).
    void threadComplete (bool) override
    {
        if (main != nullptr)
        {
            if (! ok)
                main->cancelEdit();   // cancelled, or the clip is gone: the undo step taken before is dropped again
            main->clipsChanged();
        }
        delete this;
    }

    bool ok = false;

private:
    juce::Component::SafePointer<MainComponent> main;
    Engine& engine;
    Track& track;
    int clipId;
    double stretch, semitones;
};

// Warping a clip (stretching the audio between its markers) runs behind a progress window too.
class ClipWarpJob : public juce::ThreadWithProgressWindow
{
public:
    ClipWarpJob (MainComponent& m, Engine& e, Track& t, int id, std::vector<WarpMarker> markers)
        : juce::ThreadWithProgressWindow ("Warping audio", true, true, 5000, "Cancel", &m),
          main (&m), engine (e), track (t), clipId (id), warps (std::move (markers))
    {
    }

    void run() override
    {
        ok = engine.warpClip (track, clipId, warps, [this] (double fraction)
        {
            setProgress (fraction);
            return ! threadShouldExit();
        });
    }

    void threadComplete (bool) override
    {
        if (main != nullptr)
        {
            if (! ok)
                main->cancelEdit();   // cancelled, or the clip is gone: the undo step taken before is dropped again
            main->clipsChanged();
        }
        delete this;
    }

    bool ok = false;

private:
    juce::Component::SafePointer<MainComponent> main;
    Engine& engine;
    Track& track;
    int clipId;
    std::vector<WarpMarker> warps;
};

// The warp editor of an audio clip: markers on the hits, dragged to the grid; Apply stretches the audio between them.
void MainComponent::openWarpEditor (Track& t, int clipId)
{
    Clip clip;
    std::shared_ptr<const juce::AudioBuffer<float>> audio;
    double regionStart = 0.0, regionLength = 0.0;
    if (! engine.getClip (t, clipId, clip) || ! engine.getWarpSource (t, clipId, audio, regionStart, regionLength) || audio == nullptr)
        return;

    juce::Component::SafePointer<MainComponent> safe (this);
    const TrackRef tr (&t);
    auto* panel = new WarpPanel (audio, clip.fileRate, regionStart, regionLength, clip.warps, tempoMap,
                                 clip.processed() ? clip.start - clip.offset : clip.start,
                                 [safe, tr, clipId] (std::vector<WarpMarker> markers)
                                 {
                                     juce::MessageManager::callAsync ([safe, tr, clipId, markers]
                                     {
                                         if (safe == nullptr)
                                             return;
                                         safe->warpWindow.reset();
                                         if (safe->engine.indexOfTrack (tr) < 0)
                                             return;
                                         safe->beginEdit();
                                         (new ClipWarpJob (*safe, safe->engine, *tr, clipId, markers))->launchThread();   // deletes itself when done
                                     });
                                 },
                                 [safe] { juce::MessageManager::callAsync ([safe] { if (safe != nullptr) safe->warpWindow.reset(); }); });
    warpWindow = std::make_unique<WarpWindow> ("Warp - " + t.name, panel, [safe] { juce::MessageManager::callAsync ([safe] { if (safe != nullptr) safe->warpWindow.reset(); }); });
}

// A sampler's time-stretch runs in the background behind a progress window with a Cancel button (a long sample takes a while).
class SamplerStretchJob : public juce::ThreadWithProgressWindow
{
public:
    SamplerStretchJob (MainComponent& m, Engine& e, std::shared_ptr<Sampler> s, double stretchRatio, std::function<void (bool)> onDone)
        : juce::ThreadWithProgressWindow ("Stretching sample", true, true, 5000, "Cancel", &m),
          engine (e), sampler (std::move (s)), ratio (stretchRatio), done (std::move (onDone)),
          original (sampler->getOriginal()), rate (sampler->getSampleRate())
    {
    }

    void run() override
    {
        if (original == nullptr)
            return;
        if (std::abs (ratio - 1.0) < 0.001)
        {
            result = original;   // back to the sample as loaded
            return;
        }
        result = Stretch::process (*original, 0, original->getNumSamples(), rate, ratio, 0.0, [this] (double fraction)
        {
            setProgress (fraction);
            return ! threadShouldExit();
        });
    }

    // Message thread, when it is done or cancelled (this object deletes itself here, like the stem export).
    void threadComplete (bool) override
    {
        const bool applied = result != nullptr && engine.applySamplerStretch (*sampler, original, result, ratio);
        if (done)
            done (applied);
        delete this;
    }

private:
    Engine& engine;
    std::shared_ptr<Sampler> sampler;   // (keeps the sampler alive if its track is removed meanwhile)
    double ratio;
    std::function<void (bool)> done;
    std::shared_ptr<const juce::AudioBuffer<float>> original, result;
    double rate;
};

void MainComponent::startSamplerStretch (std::shared_ptr<Sampler> sampler, double ratio, std::function<void (bool)> done)
{
    // later, not inside the panel's own slider handler (the progress window takes the focus)
    juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<MainComponent> (this), sampler, ratio, done]
    {
        if (safe == nullptr || sampler == nullptr)
        {
            if (done)
                done (false);
            return;
        }
        (new SamplerStretchJob (*safe, safe->engine, sampler, ratio, done))->launchThread();   // deletes itself when done
    });
}

// Gain, stretch and pitch of an audio clip.
void MainComponent::openClipPanel (Track& t, int clipId)
{
    Clip clip;
    if (! engine.getClip (t, clipId, clip))
        return;

    juce::Component::SafePointer<MainComponent> safe (this);
    Track* tr = &t;

    auto panel = std::make_unique<ClipPanel> (engine, t, clipId, tempoMap.bpmAt (clip.start), [safe] { if (safe != nullptr) safe->beginEdit(); },
                                              [safe] { if (safe != nullptr) safe->clipsChanged(); },
                                              [safe, tr, clipId] (double stretch, double semitones)
    {
        // later, not inside the panel's own button handler (the panel goes away when the progress window takes the focus)
        juce::MessageManager::callAsync ([safe, tr, clipId, stretch, semitones]
        {
            if (safe == nullptr || safe->engine.indexOfTrack (tr) < 0)
                return;
            safe->beginEdit();
            (new ClipProcessJob (*safe, safe->engine, *tr, clipId, stretch, semitones))->launchThread();   // deletes itself when done
        });
    });

    juce::CallOutBox::launchAsynchronously (std::move (panel), juce::Rectangle<int> (1, 1).withCentre (juce::Desktop::getMousePosition()), nullptr);
}

// Quantize / swing / humanize: every Apply works from the notes the clip had when the panel opened.
void MainComponent::openQuantize (Track& t, int clipId)
{
    MidiClip clip;
    if (! engine.getMidiClip (t, clipId, clip))
        return;

    auto original = std::make_shared<const std::vector<Note>> (clip.notes);
    auto edited = std::make_shared<bool> (false);
    juce::Component::SafePointer<MainComponent> safe (this);
    Track* tr = &t;
    const double clipStart = clip.start, clipLength = clip.length;

    auto panel = std::make_unique<QuantizePanel> ([safe, tr, clipId, original, edited, clipStart, clipLength]
                                                  (const QuantizeSettings& s)
    {
        if (safe == nullptr || safe->engine.indexOfTrack (tr) < 0)
            return;
        if (! *edited)
        {
            safe->beginEdit();
            *edited = true;
        }
        auto notes = *original;
        juce::Random rng;
        quantizeNotes (notes, clipStart, clipLength, s, safe->tempoMap, rng);
        safe->engine.setMidiNotes (*tr, clipId, std::move (notes));
        safe->clipsChanged();
        if (safe->rollVisible)
            safe->roll->showClip (safe->roll->getTrack(), safe->roll->getClipId());   // the piano roll shows the new notes
    });

    const auto mouse = juce::Desktop::getMousePosition();
    juce::CallOutBox::launchAsynchronously (std::move (panel), juce::Rectangle<int> (1, 1).withCentre (mouse), nullptr);
}

void MainComponent::addEmptyAudioTrack()
{
    int count = 1;
    for (int i = 0; i < engine.getNumTracks(); ++i)
        if (engine.getTrack (i)->type == Track::Type::Audio && ! engine.getTrack (i)->bus)
            ++count;

    beginEdit();
    auto* t = engine.addTrack ("Audio " + juce::String (count));
    addTrackUI (*t);
    armAudioTrack (*t, true);   // ready to record straight away
    clipsChanged();
}

void MainComponent::armAudioTrack (Track& t, bool armed)
{
    t.recordArmed = armed && t.type == Track::Type::Audio;
    for (auto* lane : lanes)   // (an empty armed track says so in its lane)
        lane->repaint();
}

void MainComponent::armTrack (Track& t)
{
    if (t.type == Track::Type::Midi)
    {
        t.midiArmed = ! t.midiArmed;
        refreshLiveTargets();
    }
    else
    {
        armAudioTrack (t, ! t.recordArmed);
    }
}

// The inputs of the audio interface that are switched on (Audio... window), as names: choose one for this track, or a pair.
void MainComponent::openInputMenu (Track& t, juce::Component& anchor)
{
    juce::StringArray names;
    if (auto* device = deviceManager.getCurrentAudioDevice())
    {
        const auto all = device->getInputChannelNames();
        const auto active = device->getActiveInputChannels();
        for (int c = active.findNextSetBit (0); c >= 0; c = active.findNextSetBit (c + 1))
            names.add (juce::isPositiveAndBelow (c, all.size()) ? all[c] : "Input " + juce::String (c + 1));
    }

    juce::PopupMenu menu;
    if (names.isEmpty())
    {
        menu.addItem (-1, "No input channels are switched on", false);
    }
    else
    {
        for (int i = 0; i < names.size(); ++i)   // mono
            menu.addItem (100 + i, "Input " + juce::String (i + 1) + ":  " + names[i], true, t.inputCount == 1 && t.inputFirst == i);
        if (names.size() > 1)
        {
            menu.addSeparator();
            for (int i = 0; i + 1 < names.size(); i += 2)   // stereo pairs: 1+2, 3+4, ...
                menu.addItem (200 + i, "Inputs " + juce::String (i + 1) + "+" + juce::String (i + 2) + ":  " + names[i] + " / " + names[i + 1],
                              true, t.inputCount == 2 && t.inputFirst == i);
        }
    }
    menu.addSeparator();
    menu.addItem (900, "Switch on more inputs (Audio settings)...");

    auto* track = &t;
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&anchor), [this, track] (int result)
    {
        if (result <= 0 || engine.indexOfTrack (track) < 0)
            return;
        if (result == 900)
        {
            openAudioSettings();
            return;
        }
        beginEdit();
        if (result >= 200) { track->inputFirst = result - 200; track->inputCount = 2; }
        else               { track->inputFirst = result - 100; track->inputCount = 1; }
    });
}

void MainComponent::addTrackDialog()
{
    chooser = std::make_unique<juce::FileChooser> ("Choose audio files", juce::File(), "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectMultipleItems
                              | juce::FileBrowserComponent::canSelectFiles,
                          [this] (const juce::FileChooser& fc)
                          {
                              juce::StringArray paths;
                              for (auto& f : fc.getResults())
                                  paths.add (f.getFullPathName());
                              importFiles (paths, nullptr, 0.0);
                          });
}

// Dropped on an audio track, the files become clips on that track, one after the other from `time`
// (handy for an empty track); otherwise every file gets its own new track starting at 0.
void MainComponent::importFiles (const juce::StringArray& paths, Track* target, double time)
{
    juce::StringArray failed;

    if (target != nullptr && target->sampler != nullptr && paths.size() > 0)   // dropped on a sampler track: it becomes the sample
    {
        beginEdit();
        if (engine.loadSamplerSample (*target, juce::File (paths[0])))
        {
            target->sampler->params.start = 0.0f;
            target->sampler->params.end = 1.0f;
            openSampler (*target);
            clipsChanged();
        }
        else
        {
            cancelEdit();
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Cannot load the sample", paths[0]);
        }
        return;
    }

    const bool intoTrack = target != nullptr && target->type == Track::Type::Audio;
    double nextStart = time;
    beginEdit();

    for (auto& p : paths)
    {
        const juce::File f (p);
        if (intoTrack)
        {
            const int id = engine.addClipFromFile (*target, f, nextStart);
            Clip added;
            if (id >= 0 && engine.getClip (*target, id, added))
            {
                nextStart = added.end();
                selectClip (target, id);
            }
            else
            {
                failed.add (p);
            }
        }
        else
        {
            auto* t = engine.addTrack (f.getFileNameWithoutExtension());
            if (engine.addClipFromFile (*t, f, 0.0) < 0)
            {
                engine.removeTrack (t);
                failed.add (p);
            }
            else
            {
                addTrackUI (*t);
            }
        }
    }

    if (failed.size() == paths.size())
        cancelEdit();   // nothing was imported
    clipsChanged();
    if (failed.size() > 0)
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                                                "Cannot open file(s)", failed.joinIntoString ("\n"));
}

//==============================================================================
void MainComponent::saveProject (const juce::File& file, SaveKind kind)
{
    const auto baseDir = file.getParentDirectory();

    juce::Array<juce::var> trackList;
    for (int i = 0; i < engine.getNumTracks(); ++i)
    {
        auto* t = engine.getTrack (i);
        if (templateOnly != nullptr && t != templateOnly)
            continue;   // (a template holds just one track)

        juce::Array<juce::var> clipList;
        for (auto& c : engine.getClips (*t))
        {
            auto* co = new juce::DynamicObject();
            co->setProperty ("file", projectFilePath ? projectFilePath (c.sourceFile) : c.sourceFile.getRelativePathFrom (baseDir));
            co->setProperty ("start", c.start);
            co->setProperty ("offset", c.offset);
            co->setProperty ("length", c.length);
            if (std::abs (c.gain - 1.0f) > 1.0e-6f)
                co->setProperty ("gain", (double) c.gain);
            if (! c.envelope.empty())
            {
                juce::Array<juce::var> curve;   // the clip's volume curve: [seconds from the clip start, multiplier]
                for (auto& p : c.envelope)
                    curve.add (juce::var (juce::Array<juce::var> { juce::var (p.time), juce::var ((double) p.value) }));
                co->setProperty ("envelope", curve);
            }
            if (c.processed())   // time-stretched / pitch-shifted: the settings and which part of the file, not the processed audio
            {
                co->setProperty ("stretch", c.stretch);
                co->setProperty ("pitch", c.pitch);
                co->setProperty ("regionStart", c.processedFrom);
                co->setProperty ("regionLength", c.processedLength);
                if (! c.warps.empty())   // warp markers: [original seconds, clip seconds] pairs
                {
                    juce::Array<juce::var> warpList;
                    for (auto& w : c.warps)
                        warpList.add (juce::var (juce::Array<juce::var> { juce::var (w.src), juce::var (w.dest) }));
                    co->setProperty ("warps", warpList);
                }
            }
            if (c.fadeIn > 0.0)  co->setProperty ("fadeIn", c.fadeIn);
            if (c.fadeOut > 0.0) co->setProperty ("fadeOut", c.fadeOut);
            if (c.reverse)       co->setProperty ("reverse", true);
            if (c.takeGroup != 0)
            {
                co->setProperty ("takeGroup", c.takeGroup);
                co->setProperty ("takeIndex", c.takeIndex);
                co->setProperty ("takeActive", c.takeActive);
            }
            clipList.add (juce::var (co));
        }

        auto* o = new juce::DynamicObject();
        o->setProperty ("name", t->name);
        if (t->colour != 0)
            o->setProperty ("colour", (juce::int64) t->colour);
        if (t->scaleType != 0 || t->scaleChord != 0 || t->scaleSnap)   // the key of its piano roll
        {
            o->setProperty ("scaleRoot", t->scaleRoot);
            o->setProperty ("scaleType", t->scaleType);
            o->setProperty ("scaleChord", t->scaleChord);
            o->setProperty ("scaleSnap", t->scaleSnap);
        }
        o->setProperty ("type", t->type == Track::Type::Midi ? "midi" : "audio");
        o->setProperty ("gain", (double) t->gain.load());
        o->setProperty ("pan", (double) t->pan.load());
        o->setProperty ("mute", t->mute.load());
        o->setProperty ("solo", t->solo.load());
        o->setProperty ("clips", clipList);

        if (t->type == Track::Type::Midi)
        {
            auto* s = new juce::DynamicObject();
            s->setProperty ("waveform", t->synth.waveform.load());
            s->setProperty ("attack", (double) t->synth.attack.load());
            s->setProperty ("decay", (double) t->synth.decay.load());
            s->setProperty ("sustain", (double) t->synth.sustain.load());
            s->setProperty ("release", (double) t->synth.release.load());
            s->setProperty ("cutoff", (double) t->synth.cutoff.load());
            o->setProperty ("synth", juce::var (s));

            juce::Array<juce::var> midiList;
            for (auto& m : engine.getMidiClips (*t))
            {
                juce::Array<juce::var> noteList;
                for (auto& n : m.notes)
                {
                    auto* no = new juce::DynamicObject();
                    no->setProperty ("pitch", n.pitch);
                    no->setProperty ("start", n.start);
                    no->setProperty ("length", n.length);
                    no->setProperty ("velocity", (double) n.velocity);
                    noteList.add (juce::var (no));
                }
                auto* mo = new juce::DynamicObject();
                mo->setProperty ("start", m.start);
                mo->setProperty ("length", m.length);
                mo->setProperty ("notes", noteList);
                if (! m.ccs.empty())
                {
                    juce::Array<juce::var> ccList;   // compact: [time, controller, value] per event
                    for (auto& cc : m.ccs)
                    {
                        juce::Array<juce::var> e;
                        e.add (cc.time);
                        e.add (cc.controller);
                        e.add ((double) cc.value);
                        ccList.add (juce::var (e));
                    }
                    mo->setProperty ("ccs", ccList);
                }
                midiList.add (juce::var (mo));
            }
            o->setProperty ("midiClips", midiList);
            if (t->subAutoDone)
                o->setProperty ("subAuto", true);   // (the instrument was looked at: not again)
            if (! t->subLanes.empty())   // the sub-tracks (a lane per pitch) and whether they are shown
            {
                juce::Array<juce::var> lanesList;
                for (auto& sl : t->subLanes)
                {
                    auto* lo = new juce::DynamicObject();
                    lo->setProperty ("name", sl.name);
                    lo->setProperty ("lo", sl.pitchLo);
                    lo->setProperty ("hi", sl.pitchHi);
                    lo->setProperty ("mute", sl.mute);
                    lo->setProperty ("solo", sl.solo);
                    lo->setProperty ("vol", (double) sl.volume);
                    lanesList.add (juce::var (lo));
                }
                o->setProperty ("subLanes", lanesList);
                o->setProperty ("subLanesOpen", t->subLanesOpen);
            }

            if (t->kit != nullptr)   // only the pads with a user sample need saving; the others are generated
            {
                juce::Array<juce::var> padFiles;
                for (int pad = 0; pad < DrumKit::numPads; ++pad)
                {
                    const auto f = t->kit->getPad (pad).file;
                    padFiles.add (f == juce::File() ? juce::String() : (projectFilePath ? projectFilePath (f) : f.getRelativePathFrom (baseDir)));
                }
                o->setProperty ("drumPads", padFiles);

                juce::Array<juce::var> rack;   // how each pad plays: level, pan, mute, solo, choke group and its Simpler settings
                for (int pad = 0; pad < DrumKit::numPads; ++pad)
                {
                    auto& pd = t->kit->getPad (pad);
                    auto* po = new juce::DynamicObject();
                    po->setProperty ("gain", (double) pd.gain.load());
                    po->setProperty ("pan", (double) pd.pan.load());
                    po->setProperty ("mute", pd.mute.load());
                    po->setProperty ("solo", pd.solo.load());
                    po->setProperty ("choke", pd.chokeGroup.load());
                    writeSamplerParams (*po, pd.sampler.params, pd.sampler.getStretch());
                    if (pd.velLo.load() > 0.0f || pd.velHi.load() < 1.0f)
                    {
                        po->setProperty ("velLo", (double) pd.velLo.load());
                        po->setProperty ("velHi", (double) pd.velHi.load());
                    }
                    juce::Array<juce::var> layerList;   // the extra samples (velocity layers / round-robin)
                    for (int li = 0; li < DrumKit::maxLayers; ++li)
                    {
                        auto& ly = pd.layers[(size_t) li];
                        if (! ly.used.load() || ly.file == juce::File())
                            continue;
                        auto* lo = new juce::DynamicObject();
                        lo->setProperty ("slot", li);
                        lo->setProperty ("file", projectFilePath ? projectFilePath (ly.file) : ly.file.getRelativePathFrom (baseDir));
                        lo->setProperty ("velLo", (double) ly.velLo.load());
                        lo->setProperty ("velHi", (double) ly.velHi.load());
                        layerList.add (juce::var (lo));
                    }
                    if (! layerList.isEmpty())
                        po->setProperty ("layers", layerList);
                    rack.add (juce::var (po));
                }
                o->setProperty ("drumRack", rack);
            }

            if (t->sampler != nullptr)   // the sampler: which sample (a file) and how it plays
            {
                auto* so = new juce::DynamicObject();
                if (t->sampler->getFile() != juce::File())
                    so->setProperty ("file", projectFilePath ? projectFilePath (t->sampler->getFile()) : t->sampler->getFile().getRelativePathFrom (baseDir));
                writeSamplerParams (*so, t->sampler->params, t->sampler->getStretch());
                o->setProperty ("sampler", juce::var (so));
            }
        }
        writeEffectsAndAutomation (*o, *t);   // effects, plugins, automation
        if (t->type == Track::Type::Midi)
        {
            if (t->midiInputId.isNotEmpty())
            {
                o->setProperty ("midiInput", t->midiInputId);
                for (auto& d : juce::MidiInput::getAvailableDevices())
                    if (d.identifier == t->midiInputId)
                        o->setProperty ("midiInputName", d.name);
            }
            if (t->midiInputChannel > 0)
                o->setProperty ("midiChannel", t->midiInputChannel);
        }
        o->setProperty ("sequencer", t->sequencer);
        if (t->groupId != 0)
            o->setProperty ("group", t->groupId);
        o->setProperty ("inputFirst", t->inputFirst);   // audio tracks: the interface input(s) it records from
        o->setProperty ("inputCount", t->inputCount);
        if (t->bus)
            o->setProperty ("bus", true);
        if (t->output != nullptr)
            o->setProperty ("output", engine.indexOfTrack (t->output));   // buses and sends point at tracks by index
        if (t->sidechainSource.load() != nullptr)                         // the track the compressor listens to
            o->setProperty ("sidechain", engine.indexOfTrack (t->sidechainSource.load()));
        if (t->frozen.load())                                             // bounced: the track that plays the bounce
        {
            o->setProperty ("frozen", true);
            if (t->freezeTwin != nullptr)
                o->setProperty ("freezeTwin", engine.indexOfTrack (t->freezeTwin));
        }
        if (! t->sends.empty())
        {
            juce::Array<juce::var> sendList;
            for (auto& s : t->sends)
            {
                auto* so = new juce::DynamicObject();
                so->setProperty ("bus", engine.indexOfTrack (s.bus));
                so->setProperty ("level", (double) s.level);
                so->setProperty ("pre", s.preFader);
                sendList.add (juce::var (so));
            }
            o->setProperty ("sends", sendList);
        }
        juce::Array<juce::var> outs;
        for (auto& route : t->midiOuts)
        {
            auto* ro = new juce::DynamicObject();
            ro->setProperty ("track", engine.indexOfTrack (route.target));
            ro->setProperty ("channel", route.channel);
            outs.add (juce::var (ro));
        }
        o->setProperty ("midiOuts", outs);

        juce::Array<juce::var> hardware;   // hardware MIDI outputs
        for (auto& route : t->externalOuts)
        {
            auto* ho = new juce::DynamicObject();
            ho->setProperty ("device", route.deviceId);
            ho->setProperty ("name", route.deviceName);
            ho->setProperty ("channel", route.channel);
            ho->setProperty ("outChannel", route.outChannel);
            hardware.add (juce::var (ho));
        }
        o->setProperty ("externalOuts", hardware);

        trackList.add (juce::var (o));
    }

    // the master channel: volume, effects, plugins, automation
    auto* masterObject = new juce::DynamicObject();
    masterObject->setProperty ("gain", (double) engine.getMaster().gain.load());
    writeEffectsAndAutomation (*masterObject, engine.getMaster());

    auto* root = new juce::DynamicObject();
    root->setProperty ("version", 3);
    root->setProperty ("bpm", (double) engine.bpm.load());   // (the first section's tempo, for older versions)
    {
        juce::Array<juce::var> sectionList;
        for (auto& s : tempoMap.getSections())
        {
            auto* so = new juce::DynamicObject();
            so->setProperty ("bar", s.bar);
            so->setProperty ("bpm", s.bpm);
            so->setProperty ("numerator", s.numerator);
            so->setProperty ("denominator", s.denominator);
            if (! s.name.empty())
                so->setProperty ("name", juce::String (s.name));
            sectionList.add (juce::var (so));
        }
        root->setProperty ("tempoSections", sectionList);
    }
    if (! engine.getSections().empty())   // the sections of the arrangement (verse, chorus...)
    {
        juce::Array<juce::var> arrangementList;
        for (auto& s : engine.getSections())
        {
            auto* so = new juce::DynamicObject();
            so->setProperty ("name", juce::String (s.name));
            so->setProperty ("startBar", s.startBar);
            so->setProperty ("bars", s.bars);
            if (s.colour != 0)
                so->setProperty ("colour", (juce::int64) s.colour);
            arrangementList.add (juce::var (so));
        }
        root->setProperty ("arrangement", arrangementList);
    }
    if (! engine.getGroups().empty())   // track groups (folders)
    {
        juce::Array<juce::var> groupList;
        for (auto& g : engine.getGroups())
        {
            auto* go = new juce::DynamicObject();
            go->setProperty ("id", g.id);
            go->setProperty ("name", g.name);
            go->setProperty ("collapsed", g.collapsed);
            groupList.add (juce::var (go));
        }
        root->setProperty ("groups", groupList);
    }
    root->setProperty ("master", juce::var (masterObject));
    if (! midiMappings.empty())   // MIDI learn: which controller moves which parameter
    {
        juce::Array<juce::var> mapList;
        for (auto& mp : midiMappings)
            if (mp.cc >= 0)
            {
                auto* mo = new juce::DynamicObject();
                mo->setProperty ("cc", mp.cc);
                mo->setProperty ("channel", mp.channel);
                mo->setProperty ("track", mp.trackIndex);
                mo->setProperty ("id", mp.id);
                mapList.add (juce::var (mo));
            }
        root->setProperty ("midiMap", mapList);
    }
    if (engine.hasReference())   // the reference track (a file) and how it is heard
    {
        auto* ro = new juce::DynamicObject();
        const auto refFile = engine.getReferenceFile();
        ro->setProperty ("file", projectFilePath ? projectFilePath (refFile) : refFile.getRelativePathFrom (baseDir));
        ro->setProperty ("gainDb", (double) engine.getReferenceGainDb());
        ro->setProperty ("offset", engine.getReferenceOffset());
        root->setProperty ("reference", juce::var (ro));
    }
    root->setProperty ("metronome", engine.metronome.load());
    root->setProperty ("loopEnabled", engine.loopEnabled.load());
    root->setProperty ("loopStart", engine.loopStart.load());
    root->setProperty ("loopEnd", engine.loopEnd.load());
    root->setProperty ("punchIn", engine.punchIn.load());
    root->setProperty ("punchOut", engine.punchOut.load());
    root->setProperty ("punchEnabled", engine.punchEnabled.load());
    root->setProperty ("tracks", trackList);

    if (kind != SaveKind::Normal)
    {
        file.getParentDirectory().createDirectory();
        if (file.replaceWithText (juce::JSON::toString (juce::var (root))) && kind == SaveKind::Autosave)
        {
            auto* info = new juce::DynamicObject();   // what the recovery offer shows, and where the project belongs
            info->setProperty ("original", currentProject.getFullPathName());
            info->setProperty ("name", currentProject == juce::File() ? juce::String ("Untitled") : currentProject.getFileNameWithoutExtension());
            info->setProperty ("time", juce::Time::getCurrentTime().toISO8601 (true));
            autosaveInfoFile().replaceWithText (juce::JSON::toString (juce::var (info)));
        }
        return;   // (quietly: nothing else about the open project changes)
    }

    if (file.replaceWithText (juce::JSON::toString (juce::var (root))))
    {
        currentProject = file;
        savedEdit = editCounter;
        clearAutosave();
        if (onTitleChanged)
            onTitleChanged();
        writeBackup (file);
        addRecentProject (file);
    }
    else
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                                                "Cannot save project", file.getFullPathName());
    }
}

//==============================================================================
// Backups, new versions, and collecting all audio into the project folder

juce::File MainComponent::backupFolder (const juce::File& project)
{
    return project.getParentDirectory().getChildFile (project.getFileNameWithoutExtension() + " backups");
}

// Every save also leaves a time-stamped copy next to the project; the newest twenty are kept.
void MainComponent::writeBackup (const juce::File& project)
{
    const auto folder = backupFolder (project);
    folder.createDirectory();
    const auto stamp = juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H-%M-%S");
    saveProject (folder.getChildFile (project.getFileNameWithoutExtension() + " " + stamp + ".mdaw"), SaveKind::Copy);

    auto backups = folder.findChildFiles (juce::File::findFiles, false, "*.mdaw");
    std::sort (backups.begin(), backups.end(), [] (const juce::File& a, const juce::File& b) { return a.getFileName() > b.getFileName(); });
    for (int i = 40; i < backups.size(); ++i)
        backups.getReference (i).deleteFile();
}

// "Save a new version": jam.mdaw -> jam v2.mdaw -> jam v3.mdaw (the next number that is free); the new file becomes the open project.
void MainComponent::saveNewVersion()
{
    if (currentProject == juce::File())
    {
        saveProjectAs();
        return;
    }

    auto base = currentProject.getFileNameWithoutExtension();
    int version = 1;
    const auto tail = base.fromLastOccurrenceOf (" v", false, false);
    if (tail.isNotEmpty() && tail.containsOnly ("0123456789") && base.contains (" v"))
    {
        version = tail.getIntValue();
        base = base.upToLastOccurrenceOf (" v", false, false);
    }

    juce::File next;
    do
    {
        ++version;
        next = currentProject.getParentDirectory().getChildFile (base + " v" + juce::String (version) + ".mdaw");
    }
    while (next.exists());

    saveProject (next);
}

// What goes into a collected project: a folder with the project file and an Audio folder holding a copy of every audio file it uses.
class CollectJob : public juce::ThreadWithProgressWindow
{
public:
    CollectJob (MainComponent& m, std::vector<juce::File> filesToCopy, juce::File audioFolder, juce::File projectFileToWrite)
        : juce::ThreadWithProgressWindow ("Collecting audio", true, true, 3000, "Cancel", &m),
          main (&m), files (std::move (filesToCopy)), audioDir (std::move (audioFolder)), projectFile (std::move (projectFileToWrite))
    {
    }

    void run() override
    {
        audioDir.createDirectory();
        for (size_t i = 0; i < files.size() && ! threadShouldExit(); ++i)
        {
            const auto& source = files[i];
            setStatusMessage ("Copying " + source.getFileName() + "   (" + juce::String ((int) i + 1) + " of " + juce::String ((int) files.size()) + ")");

            auto target = audioDir.getChildFile (source.getFileName());
            for (int n = 2; target.exists() && target.getSize() != source.getSize() ; ++n)   // another file with the same name: number it
                target = audioDir.getChildFile (source.getFileNameWithoutExtension() + " (" + juce::String (n) + ")" + source.getFileExtension());

            if (target.getSize() == source.getSize() && target.existsAsFile() ? true : source.copyFileTo (target))
            {
                paths[source.getFullPathName()] = "Audio/" + target.getFileName();
                bytes += source.getSize();
            }
            else
            {
                failed.add (source.getFullPathName());
            }
            setProgress ((double) (i + 1) / (double) files.size());
        }
    }

    void threadComplete (bool userPressedCancel) override
    {
        if (main != nullptr)
            main->finishCollect (projectFile, paths, failed, bytes, userPressedCancel);
        delete this;
    }

private:
    juce::Component::SafePointer<MainComponent> main;
    std::vector<juce::File> files;
    juce::File audioDir, projectFile;
    std::map<juce::String, juce::String> paths;
    juce::StringArray failed;
    juce::int64 bytes = 0;
};

void MainComponent::collectProject()
{
    // every audio file the project uses: clips (also processed ones: their original) and drum samples
    std::vector<juce::File> files;
    auto add = [&] (const juce::File& f)
    {
        if (f != juce::File() && f.existsAsFile() && std::find (files.begin(), files.end(), f) == files.end())
            files.push_back (f);
    };
    for (int i = 0; i < engine.getNumTracks(); ++i)
        if (auto* t = engine.getTrack (i))
        {
            for (auto& c : engine.getClips (*t))
                add (c.sourceFile);
            if (t->kit != nullptr)
                for (int pad = 0; pad < DrumKit::numPads; ++pad)
                {
                    add (t->kit->getPad (pad).file);
                    for (auto& ly : t->kit->getPad (pad).layers)
                        if (ly.used.load())
                            add (ly.file);
                }
            if (t->sampler != nullptr)
                add (t->sampler->getFile());
        }

    chooser = std::make_unique<juce::FileChooser> ("Choose where the project folder goes", currentProject.getParentDirectory(), "");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                          [this, files] (const juce::FileChooser& fc)
                          {
                              const auto parent = fc.getResult();
                              if (parent == juce::File() || ! parent.isDirectory())
                                  return;

                              const auto name = juce::File::createLegalFileName (currentProject == juce::File() ? juce::String ("Untitled")
                                                                                                                 : currentProject.getFileNameWithoutExtension());
                              auto folder = parent.getChildFile (name);
                              for (int n = 2; folder.exists() && folder.getNumberOfChildFiles (juce::File::findFilesAndDirectories) > 0; ++n)
                                  folder = parent.getChildFile (name + " " + juce::String (n));
                              folder.createDirectory();

                              (new CollectJob (*this, files, folder.getChildFile ("Audio"), folder.getChildFile (name + ".mdaw")))->launchThread();
                          });
}

// Back on the message thread, the audio is copied: write the project with the new paths and open it from there.
void MainComponent::finishCollect (const juce::File& projectFile, const std::map<juce::String, juce::String>& paths,
                                   const juce::StringArray& failed, juce::int64 bytes, bool cancelled)
{
    if (cancelled)
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Collecting cancelled",
                                                "The files that were copied stay in " + projectFile.getParentDirectory().getFullPathName()
                                                    + ". The open project was not changed.");
        return;
    }

    projectFilePath = [paths] (const juce::File& f)   // the copy inside the project folder, or (not copied) the old relative path
    {
        const auto found = paths.find (f.getFullPathName());
        return found != paths.end() ? found->second : f.getFullPathName();
    };
    saveProject (projectFile);
    projectFilePath = nullptr;
    loadProject (projectFile);   // continue from the collected project, so later saves keep pointing at the copies

    juce::String message = juce::String ((int) paths.size()) + " audio file(s), " + juce::String ((double) bytes / (1024.0 * 1024.0), 1)
                           + " MB, were copied into:\n" + projectFile.getParentDirectory().getFullPathName()
                           + "\n\nThis folder is now the open project, and can be moved or copied as a whole.";
    if (failed.size() > 0)
        message << "\n\nCould not be copied (they stay where they were):\n" << failed.joinIntoString ("\n");
    juce::AlertWindow::showMessageBoxAsync (failed.isEmpty() ? juce::MessageBoxIconType::InfoIcon : juce::MessageBoxIconType::WarningIcon,
                                            "Project collected", message);
}

// The "Save As" button: its menu has the other ways to save.
void MainComponent::saveMenu()
{
    juce::PopupMenu menu;
    menu.addItem (1, "Save as...");
    menu.addItem (2, "Save a new version (adds v2, v3...)");
    menu.addItem (3, "Save with all audio in one folder...");
    menu.addSeparator();
    menu.addItem (5, "Version history...", currentProject != juce::File() && backupFolder (currentProject).isDirectory());
    menu.addItem (4, "Show the backups folder", currentProject != juce::File() && backupFolder (currentProject).isDirectory());
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&saveAsButton), [this] (int result)
    {
        if (result == 5) openVersionHistory();
        if (result == 1) saveProjectAs();
        if (result == 2) saveNewVersion();
        if (result == 3) collectProject();
        if (result == 4) backupFolder (currentProject).revealToUser();
    });
}

//==============================================================================
// Autosave and recovery

juce::File MainComponent::autosaveFile() const
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("UFODAW").getChildFile ("autosave")
                                                                                    .getChildFile (tabSlot == 0 ? juce::String ("recovery.mdaw")
                                                                                                                : "recovery-" + juce::String (tabSlot) + ".mdaw");
}

juce::File MainComponent::autosaveInfoFile() const { return autosaveFile().withFileExtension ("json"); }

void MainComponent::autosaveNow()
{
    if (! isDirty() || editCounter == autosavedEdit)
        return;
    if (engine.isRecording() || midiRecording)   // not while recording: try again at the next round
        return;
    saveProject (autosaveFile(), SaveKind::Autosave);
    autosavedEdit = editCounter;
}

void MainComponent::clearAutosave()
{
    autosaveFile().deleteFile();
    autosaveInfoFile().deleteFile();
    autosavedEdit = editCounter;
}

void MainComponent::offerRecovery()
{
    juce::Component::SafePointer<MainComponent> safe (this);
    juce::MessageManager::callAsync ([safe]
    {
        if (safe == nullptr || ! safe->autosaveFile().existsAsFile())
            return;

        const auto info = juce::JSON::parse (safe->autosaveInfoFile());
        const auto name = info["name"].toString().isNotEmpty() ? info["name"].toString() : juce::String ("Untitled");
        const auto when = juce::Time::fromISO8601 (info["time"].toString());
        const auto original = info["original"].toString();

        juce::String text = "UFODAW found an autosaved version of \"" + name + "\" from " + when.toString (true, true) + ", "
                            "from a session that was not closed properly or was closed with unsaved changes.\n\nRestore it?";
        juce::AlertWindow::showAsync (juce::MessageBoxOptions::makeOptionsOkCancel (juce::MessageBoxIconType::QuestionIcon, "Restore your work?",
                                                                                    text, "Restore", "Discard", safe.getComponent()),
                                      [safe, original, name] (int result)
        {
            if (safe == nullptr)
                return;
            if (result != 1)
            {
                safe->clearAutosave();
                return;
            }

            safe->loadProject (safe->autosaveFile());
            safe->currentProject = original.isNotEmpty() ? juce::File (original) : juce::File();   // saving goes back to where it belongs
            ++safe->editCounter;                                                                   // still unsaved
            if (safe->onTitleChanged)
                safe->onTitleChanged();
        });
    });
}

// ---- Track templates: one track (settings, effects, plugins with their state, clips) saved as a small project file, added to any project later

juce::File MainComponent::templatesFolder()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("UFODAW").getChildFile ("templates");
}

void MainComponent::saveTrackTemplate (Track& t)
{
    juce::Component::SafePointer<MainComponent> safe (this);
    const TrackRef ref (&t);
    auto* box = new juce::AlertWindow ("Save as a template", "A name for this template (it is added from + > From a template):", juce::MessageBoxIconType::NoIcon);
    box->addTextEditor ("name", t.name);
    box->addButton ("Save", 1, juce::KeyPress (juce::KeyPress::returnKey));
    box->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    box->enterModalState (true, juce::ModalCallbackFunction::create ([safe, ref, box] (int ok)
    {
        if (ok != 1 || safe == nullptr || safe->engine.indexOfTrack (ref) < 0)
            return;
        const auto name = juce::File::createLegalFileName (box->getTextEditorContents ("name").trim());
        if (name.isEmpty())
            return;
        const auto file = templatesFolder().getChildFile (name + ".mdaw");
        templatesFolder().createDirectory();
        safe->templateOnly = ref.operator->();
        safe->saveProject (file, SaveKind::Copy);
        safe->templateOnly = nullptr;
    }), true);
}

void MainComponent::addFromTemplate (const juce::File& file)
{
    const int before = engine.getNumTracks();
    loadProject (file, true);
    if (engine.getNumTracks() == before)
        cancelEdit();   // nothing was added
}

// The versions of the project: the copies in its backups folder, with a way to open one in a tab or to go back to it.
void MainComponent::openVersionHistory()
{
    if (currentProject == juce::File() || ! backupFolder (currentProject).isDirectory())
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "No versions yet", "Save the project first: every save leaves a version here.");
        return;
    }
    juce::Component::SafePointer<MainComponent> safe (this);
    auto closeIt = [safe] { juce::MessageManager::callAsync ([safe] { if (safe != nullptr) safe->historyWindow.reset(); }); };
    auto* panel = new VersionHistoryPanel (backupFolder (currentProject),
        [safe] (const juce::File& version)
        {
            if (safe == nullptr)
                return;
            if (safe->requestOpenInTab)
                safe->requestOpenInTab (version);
            else
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Open the version", "Use File > Open... and choose:\n" + version.getFullPathName());
        },
        [safe] (const juce::File& version)
        {
            if (safe == nullptr)
                return;
            juce::AlertWindow::showAsync (juce::MessageBoxOptions::makeOptionsOkCancel (juce::MessageBoxIconType::QuestionIcon, "Go back to this version?",
                                              "The project is replaced by the version of " + version.getLastModificationTime().formatted ("%d %b %Y %H:%M:%S")
                                                  + ". What you have now is kept as a copy in the history first.",
                                              "Go back", "Cancel", safe.getComponent()),
                                          [safe, version] (int result)
            {
                if (safe == nullptr || result != 1)
                    return;
                const auto original = safe->currentProject;
                safe->writeBackup (original);   // (the present state stays reachable)
                safe->historyWindow.reset();
                safe->loadProject (version);
                safe->currentProject = original;
                ++safe->editCounter;   // (differs from what is saved)
                if (safe->onTitleChanged)
                    safe->onTitleChanged();
            });
        },
        closeIt);
    historyWindow = std::make_unique<VersionHistoryWindow> ("Version history - " + currentProject.getFileNameWithoutExtension(), panel, closeIt);
}

void MainComponent::saveProjectAs()
{
    chooser = std::make_unique<juce::FileChooser> ("Save project", currentProject, "*.mdaw");
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::warnAboutOverwriting,
                          [this] (const juce::FileChooser& fc)
                          {
                              auto f = fc.getResult();
                              if (f != juce::File())
                                  saveProject (f.withFileExtension ("mdaw"));
                          });
}

//==============================================================================
// File menu, new project, recent projects

juce::File MainComponent::recentListFile()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("UFODAW").getChildFile ("recent.txt");
}

juce::StringArray MainComponent::loadRecentProjects() const
{
    juce::StringArray all, existing;
    all.addLines (recentListFile().loadFileAsString());
    for (auto& path : all)
        if (path.isNotEmpty() && juce::File (path).existsAsFile())
            existing.add (path);
    return existing;
}

void MainComponent::addRecentProject (const juce::File& f)
{
    auto list = loadRecentProjects();
    list.removeString (f.getFullPathName());
    list.insert (0, f.getFullPathName());
    while (list.size() > 8)
        list.remove (list.size() - 1);
    recentListFile().getParentDirectory().createDirectory();
    recentListFile().replaceWithText (list.joinIntoString ("\n"));
}

void MainComponent::fileMenu()
{
    const auto recent = loadRecentProjects();
    juce::PopupMenu recentMenu;
    for (int i = 0; i < recent.size(); ++i)
        recentMenu.addItem (100 + i, juce::File (recent[i]).getFileNameWithoutExtension() + "   (" + juce::File (recent[i]).getParentDirectory().getFileName() + ")");

    juce::PopupMenu menu;
    menu.addItem (1, "New project   Ctrl+N");
    juce::PopupMenu templateMenu;   // projects made from the starter pack
    const auto templateList = ProjectTemplates::list();
    for (int i = 0; i < (int) templateList.size(); ++i)
        templateMenu.addItem (400 + i, templateList[(size_t) i].name + "   -   " + templateList[(size_t) i].description.substring (0, 70));
    menu.addSubMenu ("New from a template", templateMenu);
    menu.addItem (2, "Open...   Ctrl+O");
    menu.addSubMenu ("Open recent", recentMenu, ! recent.isEmpty());
    menu.addSeparator();
    menu.addItem (3, "Save   Ctrl+S");
    menu.addItem (4, "Save as...");
    menu.addItem (5, "Save a new version (adds v2, v3...)");
    menu.addItem (6, "Save with all audio in one folder...");
    menu.addItem (11, "Version history...", currentProject != juce::File() && backupFolder (currentProject).isDirectory());
    menu.addItem (7, "Show the backups folder", currentProject != juce::File() && backupFolder (currentProject).isDirectory());
    menu.addSeparator();
    menu.addItem (8, "Export audio (WAV, FLAC, MP3...)...");
    menu.addItem (9, "Export stems...");
    menu.addSeparator();
    menu.addItem (12, "Command palette...   Ctrl+K");
    menu.addItem (13, "Keyboard shortcuts...");
    menu.addItem (14, "MIDI controllers (MIDI learn)...");
    menu.addItem (15, "Sample browser (starter pack and your folders)...");
    menu.addSeparator();
    menu.addItem (16, updateVersion.isNotEmpty() ? "Update available: " + updateVersion + "..." : juce::String ("Check for updates..."));
    menu.addItem (17, "Check for updates once a day at start-up", true, UiSettings::autoUpdateCheck);
    menu.addItem (10, "Quick tour...");
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&railFile), [this, recent, templateList] (int result)
    {
        if (result == 12) openPalette();
        else if (result == 13) openShortcuts();
        else if (result == 14) openMidiLearn();
        else if (result == 15) openSampleBrowser();
        else if (result == 16) { if (updateVersion.isNotEmpty()) showUpdateInfo(); else checkForUpdates (true); }
        else if (result == 17) { UiSettings::autoUpdateCheck = ! UiSettings::autoUpdateCheck; saveUiSettings(); }
        if (result == 1) newProject();
        else if (result == 2) openProjectDialog();
        else if (result == 3) saveButton.triggerClick();
        else if (result == 4) saveProjectAs();
        else if (result == 5) saveNewVersion();
        else if (result == 6) collectProject();
        else if (result == 7) backupFolder (currentProject).revealToUser();
        else if (result == 11) openVersionHistory();
        else if (result == 8) exportDialog();
        else if (result == 9) exportStemsDialog();
        if (result == 10) startTour();
        else if (result >= 400 && result - 400 < (int) templateList.size())
        {
            StarterPack::ensureInstalled();
            ProjectTemplates::ensureWritten();
            const auto file = templateList[(size_t) result - 400].file;
            if (requestOpenTemplate)
                requestOpenTemplate (file);
            else
                openTemplateHere (file);
        }
        else if (result >= 100 && result - 100 < recent.size())
        {
            const juce::File chosen (recent[result - 100]);
            if (requestOpenInTab && ! isEmptyUntitled())
                requestOpenInTab (chosen);
            else
                loadProject (chosen);
        }
    });
}

void MainComponent::openTemplateHere (const juce::File& templateFile)
{
    if (! templateFile.existsAsFile())
        return;
    loadProject (templateFile);
    currentProject = juce::File();                           // (the template itself is never overwritten: saving asks for a name)
    untitledTitle = templateFile.getFileNameWithoutExtension();
    ++editCounter;                                           // unsaved: closing it asks
    if (onTitleChanged)
        onTitleChanged();
}

void MainComponent::newProject()
{
    if (requestNewTab)   // projects live in tabs: a new project is a new tab
    {
        requestNewTab();
        return;
    }
    if (! isDirty())
    {
        resetToEmptyProject();
        return;
    }

    const auto name = currentProject == juce::File() ? juce::String ("the untitled project") : currentProject.getFileNameWithoutExtension();
    juce::Component::SafePointer<MainComponent> safe (this);
    juce::AlertWindow::showYesNoCancelBox (juce::MessageBoxIconType::QuestionIcon, "Start a new project",
                                           "Save the changes to " + name + " first?", "Save", "Don't save", "Cancel", this,
                                           juce::ModalCallbackFunction::create ([safe] (int choice)
    {
        if (safe == nullptr || choice == 0)   // cancel
            return;
        if (choice == 1)                      // save
        {
            if (safe->currentProject == juce::File())
            {
                safe->saveProjectAs();        // (asks for a name; press New again afterwards)
                return;
            }
            safe->saveProject (safe->currentProject);
        }
        safe->resetToEmptyProject();
    }));
}

void MainComponent::resetToEmptyProject()
{
    untitledTitle = {};
    finishRecording();
    engine.silenceEverything();
    engine.setPositionSeconds (0.0);
    playStartSeconds = 0.0;
    clearTracksUI();
    undoStack.clear();
    redoStack.clear();
    updateUndoButtons();

    tempoMap.setSections ({ TempoSection() });
    engine.setTempoMap (tempoMap, false);
    engine.setSections ({});
    engine.getGroups().clear();
    engine.clearReference();
    midiMappings.clear();
    engine.metronome = false;
    metroButton.setToggleState (false, juce::dontSendNotification);
    bpmSlider.setValue (120.0, juce::dontSendNotification);
    engine.loopStart = engine.loopEnd = engine.punchIn = engine.punchOut = 0.0;
    engine.punchEnabled = false;
    engine.loopEnabled = false;
    punchButton.setToggleState (false, juce::dontSendNotification);
    loopButton.setToggleState (false, juce::dontSendNotification);
    engine.resetMaster();
    masterHeader->syncControls();

    currentProject = juce::File();
    savedEdit = editCounter;
    clearAutosave();
    if (onTitleChanged)
        onTitleChanged();
    clipsChanged();
    resized();
    repaint();
}

void MainComponent::openProjectDialog()
{
    chooser = std::make_unique<juce::FileChooser> ("Open project", currentProject, "*.mdaw");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this] (const juce::FileChooser& fc)
                          {
                              auto f = fc.getResult();
                              if (f.existsAsFile())
                              {
                                  if (requestOpenInTab && ! isEmptyUntitled())
                                      requestOpenInTab (f);
                                  else
                                      loadProject (f);
                              }
                          });
}

// `asTemplate`: the track(s) of the file are added to the open project (a template): the rest of the file (tempo, master...) is not used.
void MainComponent::loadProject (const juce::File& file, bool asTemplate)
{
    auto root = juce::JSON::parse (file);
    if (! root.isObject() || ! root["tracks"].isArray())
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                                                asTemplate ? "Cannot open template" : "Cannot open project", file.getFullPathName());
        return;
    }

    const int firstNew = asTemplate ? engine.getNumTracks() : 0;
    if (asTemplate)
    {
        beginEdit();
    }
    else
    {
        finishRecording();
        engine.stop();
        engine.setPositionSeconds (0.0);
        playStartSeconds = 0.0;
        clearTracksUI();
        undoStack.clear();   // a freshly opened project starts with a clean history
        redoStack.clear();
        updateUndoButtons();
    }

    if (! asTemplate)
    {
        // tempo sections; older projects only have one BPM (4/4 all the way)
        std::vector<TempoSection> sections;
        if (auto* list = root["tempoSections"].getArray())
            for (auto& sv : *list)
            {
                TempoSection s;
                s.bar = sv.getProperty ("bar", 0);
                s.bpm = sv.getProperty ("bpm", 120.0);
                s.numerator = sv.getProperty ("numerator", 4);
                s.denominator = sv.getProperty ("denominator", 4);
                s.name = sv.getProperty ("name", "").toString().toStdString();
                sections.push_back (s);
            }
        if (sections.empty())
        {
            TempoSection s;
            s.bpm = (double) root.getProperty ("bpm", 120.0);
            sections.push_back (s);
        }
        tempoMap.setSections (sections);
        engine.setTempoMap (tempoMap, false);   // loading: the MIDI is already where it belongs
    }
    if (! asTemplate)
    {
    {
        std::vector<ArrSection> arrangement;
        if (auto* list = root["arrangement"].getArray())
            for (auto& sv : *list)
            {
                ArrSection s;
                s.name = sv.getProperty ("name", "").toString().toStdString();
                s.startBar = juce::jmax (0, (int) sv.getProperty ("startBar", 0));
                s.bars = juce::jlimit (1, 999, (int) sv.getProperty ("bars", 4));
                s.colour = (juce::uint32) (juce::int64) sv.getProperty ("colour", (juce::int64) 0);
                arrangement.push_back (s);
            }
        engine.setSections (std::move (arrangement));

        engine.getGroups().clear();
        if (auto* list = root["groups"].getArray())
            for (auto& gv : *list)
            {
                TrackGroup g;
                g.id = (int) gv.getProperty ("id", 0);
                g.name = gv.getProperty ("name", "Group").toString();
                g.collapsed = (bool) gv.getProperty ("collapsed", false);
                if (g.id > 0)
                    engine.getGroups().push_back (g);
            }
    }
    engine.metronome = (bool) root.getProperty ("metronome", false);
    bpmSlider.setValue (tempoMap.getSections().front().bpm, juce::dontSendNotification);
    metroButton.setToggleState (engine.metronome, juce::dontSendNotification);

    engine.loopStart = (double) root.getProperty ("loopStart", 0.0);
    engine.loopEnd = (double) root.getProperty ("loopEnd", 0.0);
    engine.punchIn = (double) root.getProperty ("punchIn", 0.0);
    engine.punchOut = (double) root.getProperty ("punchOut", 0.0);
    engine.punchEnabled = (bool) root.getProperty ("punchEnabled", false);
    punchButton.setToggleState (engine.punchEnabled, juce::dontSendNotification);
    engine.loopEnabled = (bool) root.getProperty ("loopEnabled", false);
    loopButton.setToggleState (engine.loopEnabled, juce::dontSendNotification);
    }

    juce::StringArray missing, pluginProblems;
    const auto baseDir = file.getParentDirectory();

    if (! asTemplate)   // MIDI learn
    {
        midiMappings.clear();
        if (auto* list = root["midiMap"].getArray())
            for (auto& mv : *list)
            {
                MidiMapping mp;
                mp.cc = (int) mv.getProperty ("cc", -1);
                mp.channel = juce::jlimit (0, 16, (int) mv.getProperty ("channel", 0));
                mp.trackIndex = (int) mv.getProperty ("track", 0);
                mp.id = mv.getProperty ("id", "volume").toString();
                if (mp.cc >= 0 && mp.cc < 128)
                    midiMappings.push_back (mp);
            }
    }

    if (! asTemplate)   // the reference track
    {
        engine.clearReference();
        if (const auto ref = root["reference"]; ref.isObject())
        {
            const auto refFile = baseDir.getChildFile (ref["file"].toString());
            if (engine.loadReference (refFile))
            {
                engine.setReferenceGainDb ((float) (double) ref.getProperty ("gainDb", 0.0));
                engine.setReferenceOffset ((double) ref.getProperty ("offset", 0.0));
            }
            else
                missing.add (refFile.getFullPathName());
        }
    }

    // master channel (projects from before it had effects only stored a master volume)
    if (! asTemplate)
    {
        engine.resetMaster();
        if (const auto masterVar = root["master"]; masterVar.isObject())
        {
            engine.getMaster().gain = (float) (double) masterVar.getProperty ("gain", 0.9);
            readEffectsAndAutomation (engine.getMaster(), masterVar, pluginProblems);
        }
        else
        {
            engine.getMaster().gain = (float) (double) root.getProperty ("masterGain", 0.9);
        }
        masterHeader->syncControls();
    }

    for (auto& item : *root["tracks"].getArray())
    {
        const bool isMidi = item.getProperty ("type", "audio").toString() == "midi";
        const bool isDrums = isMidi && item.hasProperty ("drumPads");
        const auto trackName = item.getProperty ("name", "Track").toString();
        const bool isSampler = isMidi && item.hasProperty ("sampler");
        auto* t = isDrums ? engine.addDrumTrack (trackName)
                : isSampler ? engine.addSamplerTrack (trackName)
                            : engine.addTrack (trackName, isMidi ? Track::Type::Midi : Track::Type::Audio,
                                               ! isMidi && (bool) item.getProperty ("bus", false));
        t->colour = (juce::uint32) (juce::int64) item.getProperty ("colour", (juce::int64) 0);
        t->scaleRoot = juce::jlimit (0, 11, (int) item.getProperty ("scaleRoot", 0));
        t->scaleType = juce::jmax (0, (int) item.getProperty ("scaleType", 0));
        t->scaleChord = juce::jlimit (0, 2, (int) item.getProperty ("scaleChord", 0));
        t->scaleSnap = (bool) item.getProperty ("scaleSnap", false);

        if (isSampler)   // the sample and how it plays
        {
            const auto sv = item["sampler"];
            readSamplerParams (sv, t->sampler->params);   // (a fresh sampler's own values are the defaults of anything missing)
            if (sv.hasProperty ("file"))
            {
                const auto sampleFile = baseDir.getChildFile (sv["file"].toString());
                if (! engine.loadSamplerSample (*t, sampleFile))
                    missing.add (sampleFile.getFullPathName());
                else if (const double st = sv.getProperty ("stretch", 1.0); st != 1.0)
                    engine.setSamplerStretch (*t->sampler, st);
            }
        }

        if (isDrums)
            if (auto* padFiles = item["drumPads"].getArray())
                for (int pad = 0; pad < padFiles->size() && pad < DrumKit::numPads; ++pad)
                {
                    const auto rel = (*padFiles)[pad].toString();
                    if (rel.isEmpty())
                        continue;
                    const auto sample = baseDir.getChildFile (rel);
                    if (! engine.loadDrumSample (*t, pad, sample))
                        missing.add (sample.getFullPathName());
                }
        if (isDrums)   // (after the samples: loading a sample resets that pad's start / end)
            if (auto* rack = item["drumRack"].getArray())
                for (int pad = 0; pad < rack->size() && pad < DrumKit::numPads; ++pad)
                {
                    const auto& pv = (*rack)[pad];
                    auto& pd = t->kit->getPad (pad);
                    pd.gain = (float) (double) pv.getProperty ("gain", 1.0);
                    pd.pan = (float) (double) pv.getProperty ("pan", 0.0);
                    pd.mute = (bool) pv.getProperty ("mute", false);
                    pd.solo = (bool) pv.getProperty ("solo", false);
                    pd.chokeGroup = (int) pv.getProperty ("choke", 0);
                    readSamplerParams (pv, pd.sampler.params);
                    if (const double st = pv.getProperty ("stretch", 1.0); st != 1.0 && pd.sampler.getSample() != nullptr)
                        engine.setSamplerStretch (pd.sampler, st);
                    pd.velLo = (float) (double) pv.getProperty ("velLo", 0.0);
                    pd.velHi = (float) (double) pv.getProperty ("velHi", 1.0);
                    if (auto* layerList = pv["layers"].getArray())   // (after the pad's own settings: a layer starts as a copy of them)
                        for (auto& lv : *layerList)
                        {
                            const int slot = juce::jlimit (0, DrumKit::maxLayers - 1, (int) lv.getProperty ("slot", 0));
                            const auto sampleFile = baseDir.getChildFile (lv["file"].toString());
                            if (engine.loadDrumLayer (*t, pad, slot, sampleFile))
                            {
                                pd.layers[(size_t) slot].velLo = (float) (double) lv.getProperty ("velLo", 0.0);
                                pd.layers[(size_t) slot].velHi = (float) (double) lv.getProperty ("velHi", 1.0);
                            }
                            else
                                missing.add (sampleFile.getFullPathName());
                        }
                }
        t->gain = (float) (double) item.getProperty ("gain", 0.8);
        t->pan = (float) (double) item.getProperty ("pan", 0.0);
        t->mute = (bool) item.getProperty ("mute", false);
        t->solo = (bool) item.getProperty ("solo", false);


        auto loadClip = [&] (const juce::var& v)
        {
            const auto audioFile = baseDir.getChildFile (v["file"].toString());
            const double start = v.getProperty ("start", 0.0);
            const int id = engine.addClipFromFile (*t, audioFile, start);
            if (id < 0)
            {
                missing.add (audioFile.getFullPathName());
                return;
            }
            std::vector<WarpMarker> warps;
            if (auto* warpList = v["warps"].getArray())
                for (auto& wv : *warpList)
                    if (auto* pair = wv.getArray(); pair != nullptr && pair->size() >= 2)
                        warps.push_back ({ (double) (*pair)[0], (double) (*pair)[1] });
            if (v.hasProperty ("stretch"))   // the stretched / shifted audio is made again (or taken from the cache) before the region is set
                if (! engine.restoreProcessedClip (*t, id, v.getProperty ("stretch", 1.0), v.getProperty ("pitch", 0.0),
                                                   v.getProperty ("regionStart", 0.0), v.getProperty ("regionLength", 0.0), nullptr, warps))
                    pluginProblems.add ("Could not apply the time-stretch of a clip in " + t->name);
            Clip c;
            engine.getClip (*t, id, c);
            engine.setClipRegion (*t, id, start,
                                  v.getProperty ("offset", 0.0),
                                  v.getProperty ("length", c.sourceLength()));
            engine.setClipFades (*t, id, v.getProperty ("fadeIn", 0.0), v.getProperty ("fadeOut", 0.0));
            if (v.hasProperty ("gain"))
                engine.setClipGain (*t, id, (float) (double) v.getProperty ("gain", 1.0));
            if (auto* curve = v["envelope"].getArray())
            {
                std::vector<AutoPoint> points;
                for (auto& pv : *curve)
                    if (auto* pair = pv.getArray(); pair != nullptr && pair->size() >= 2)
                        points.push_back ({ (double) (*pair)[0], (float) (double) (*pair)[1] });
                engine.setClipEnvelope (*t, id, std::move (points));
            }
            engine.setClipReverse (*t, id, (bool) v.getProperty ("reverse", false));
            if ((int) v.getProperty ("takeGroup", 0) != 0)
                engine.setClipTake (*t, id, v["takeGroup"], v.getProperty ("takeIndex", 1), v.getProperty ("takeActive", true));
        };

        if (isMidi)
        {
            const auto s = item["synth"];
            t->synth.waveform = (int) s.getProperty ("waveform", (int) SynthParams::Saw);
            t->synth.attack = (float) (double) s.getProperty ("attack", 0.005);
            t->synth.decay = (float) (double) s.getProperty ("decay", 0.15);
            t->synth.sustain = (float) (double) s.getProperty ("sustain", 0.7);
            t->synth.release = (float) (double) s.getProperty ("release", 0.25);
            t->synth.cutoff = (float) (double) s.getProperty ("cutoff", 8000.0);

            if ((bool) item.getProperty ("subAuto", false))
                t->subAutoDone = true;
            if (auto* lanesArray = item["subLanes"].getArray())   // the sub-tracks
            {
                std::vector<SubLane> loaded;
                for (auto& lv : *lanesArray)
                {
                    SubLane sl;
                    sl.name = lv.getProperty ("name", "Sub").toString();
                    sl.pitchLo = juce::jlimit (0, 127, (int) lv.getProperty ("lo", 36));
                    sl.pitchHi = juce::jlimit (sl.pitchLo, 127, (int) lv.getProperty ("hi", sl.pitchLo));
                    sl.mute = (bool) lv.getProperty ("mute", false);
                    sl.solo = (bool) lv.getProperty ("solo", false);
                    sl.volume = juce::jlimit (0.0f, 1.5f, (float) (double) lv.getProperty ("vol", 1.0));
                    loaded.push_back (sl);
                }
                engine.setSubLanes (*t, std::move (loaded), (bool) item.getProperty ("subLanesOpen", true));
                t->subAutoDone = true;
            }

            if (auto* midiArray = item["midiClips"].getArray())
                for (auto& mv : *midiArray)
                {
                    const int id = engine.addMidiClip (*t, mv.getProperty ("start", 0.0), mv.getProperty ("length", 4.0));
                    std::vector<Note> notes;
                    if (auto* noteArray = mv["notes"].getArray())
                        for (auto& nv : *noteArray)
                        {
                            Note n;
                            n.pitch = (int) nv.getProperty ("pitch", 60);
                            n.start = nv.getProperty ("start", 0.0);
                            n.length = nv.getProperty ("length", 0.5);
                            n.velocity = (float) (double) nv.getProperty ("velocity", 0.8);
                            notes.push_back (n);
                        }
                    engine.setMidiNotes (*t, id, std::move (notes));

                    std::vector<CcEvent> ccs;
                    if (auto* ccArray = mv["ccs"].getArray())
                        for (auto& ev : *ccArray)
                            if (auto* e = ev.getArray(); e != nullptr && e->size() >= 3)
                                ccs.push_back ({ (double) (*e)[0], (int) (*e)[1], (float) (double) (*e)[2] });
                    if (! ccs.empty())
                        engine.setMidiCcs (*t, id, std::move (ccs));
                }

            // the MIDI input this track listens to; a device that was renamed or got a new identifier is found by its name
            t->midiInputChannel = juce::jlimit (0, 16, (int) item.getProperty ("midiChannel", 0));
            auto inputId = item.getProperty ("midiInput", "").toString();
            if (inputId.isNotEmpty())
            {
                const auto devices = juce::MidiInput::getAvailableDevices();
                const bool known = std::any_of (devices.begin(), devices.end(), [&] (auto& d) { return d.identifier == inputId; });
                if (! known)
                    for (auto& d : devices)
                        if (d.name == item.getProperty ("midiInputName", "").toString())
                            inputId = d.identifier;
            }
            t->midiInputId = inputId;
        }
        else if (auto* clipArray = item["clips"].getArray())
        {
            for (auto& cv : *clipArray)
                loadClip (cv);
        }
        else if (item.hasProperty ("file"))   // version 1: one clip per track
        {
            loadClip (item);
        }

        readEffectsAndAutomation (*t, item, pluginProblems);
        t->inputFirst = juce::jmax (0, (int) item.getProperty ("inputFirst", 0));
        t->inputCount = juce::jlimit (1, 2, (int) item.getProperty ("inputCount", 2));

        if (auto* hardware = item["externalOuts"].getArray())   // hardware MIDI outputs (opened here)
        {
            std::vector<ExternalRoute> routes;
            for (auto& hv : *hardware)
            {
                ExternalRoute r;
                r.deviceId = hv["device"].toString();
                r.deviceName = hv["name"].toString();
                r.channel = (int) hv["channel"];
                r.outChannel = hv.hasProperty ("outChannel") ? (int) hv["outChannel"] : 1;
                routes.push_back (r);
            }
            engine.setExternalOuts (*t, routes);
            for (auto& r : t->externalOuts)
                if (r.port < 0)
                    pluginProblems.add (t->name + ": MIDI output \"" + r.deviceName + "\" is not available");
        }

        addTrackUI (*t);
    }

    // MIDI routes refer to other tracks by index, so they are set once every track exists
    {
        int index = 0;
        for (auto& item : *root["tracks"].getArray())
        {
            std::vector<MidiRoute> routes;
            if (auto* outs = item["midiOuts"].getArray())
                for (auto& v : *outs)
                {
                    // newer projects: { track, channel }; earlier ones: just the track index
                    const bool isObject = v.isObject();
                    const int trackIndex = isObject ? (int) v["track"] : (int) v;
                    const int channel = isObject ? (int) v["channel"] : 0;
                    if (auto* to = engine.getTrack (firstNew + trackIndex))
                        routes.push_back ({ to, channel });
                }
            if (item.hasProperty ("midiOut") && (int) item["midiOut"] >= 0)   // projects saved with a single route
                if (auto* to = engine.getTrack (firstNew + (int) item["midiOut"]))
                    routes.push_back ({ to, 0 });

            if (auto* from = engine.getTrack (firstNew + index))
            {
                from->sequencer = (bool) item.getProperty ("sequencer", ! routes.empty());
                if (! routes.empty())
                    engine.setMidiOuts (*from, routes);

                // output bus and sends (a template has no other tracks to point at)
                if (! asTemplate && item.hasProperty ("output"))
                    if (auto* bus = engine.getTrack ((int) item["output"]))
                        engine.setTrackOutput (*from, bus);
                if (! asTemplate && item.hasProperty ("sidechain"))
                    if (auto* source = engine.getTrack ((int) item["sidechain"]))
                        engine.setSidechain (*from, source);
                if (! asTemplate)
                    from->groupId = (int) item.getProperty ("group", 0);
                if (! asTemplate && (bool) item.getProperty ("frozen", false))
                {
                    from->frozen = true;
                    if (item.hasProperty ("freezeTwin"))
                        if (auto* twin = engine.getTrack ((int) item["freezeTwin"]); twin != nullptr && twin != from)
                            from->freezeTwin = twin;
                }
                if (auto* sendArray = asTemplate ? nullptr : item["sends"].getArray())
                    for (auto& sv : *sendArray)
                        if (auto* bus = engine.getTrack ((int) sv["bus"]))
                            engine.setSend (*from, *bus, (float) (double) sv.getProperty ("level", 0.5), (bool) sv.getProperty ("pre", false));
            }
            ++index;
        }
    }

    if (! asTemplate)
    {
        currentProject = file;
        savedEdit = editCounter;   // freshly opened: nothing unsaved
        addRecentProject (file);
        if (onTitleChanged)
            onTitleChanged();
    }
    clipsChanged();

    if (missing.size() > 0)
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                                                "Some audio files were not found",
                                                missing.joinIntoString ("\n"));
    if (pluginProblems.size() > 0)
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                                                "Some plugins could not be loaded",
                                                pluginProblems.joinIntoString ("\n"));
}

// Renders one track (with its instrument, effects, plugins, volume and pan) to a WAV file behind a progress window, for Freeze.
class FreezeJob : public juce::ThreadWithProgressWindow
{
public:
    FreezeJob (Engine& e, Track* t, juce::File target, double lengthSeconds, juce::Component* centreAround, std::function<void (bool)> whenDone)
        : juce::ThreadWithProgressWindow ("Freezing " + t->name, true, true, 5000, "Cancel", centreAround),
          engine (e), track (t), file (std::move (target)), seconds (lengthSeconds), done (std::move (whenDone))
    {
    }

    void run() override
    {
        Engine::RenderOptions options;
        options.onlyTrack = track;
        options.masterProcessing = false;
        options.noDelayCompensation = true;
        options.bits = 32;   // floating point: no clipping in the bounce
        options.shouldCancel = [this] { return threadShouldExit(); };
        options.onProgress = [this] (double f) { setProgress (f); };
        ok = engine.renderToFile (file, seconds, options);
    }

    void threadComplete (bool userPressedCancel) override
    {
        const bool good = ok && ! userPressedCancel;
        if (! good)
            file.deleteFile();
        if (done)
            done (good);
    }

private:
    Engine& engine;
    Track* track;
    juce::File file;
    double seconds;
    std::function<void (bool)> done;
    bool ok = false;
};

void MainComponent::freezeTrack (Track& t)
{
    finishRecording();
    engine.stop();
    if (t.frozen.load() || t.bus || ! engine.trackHasOutput (t))
        return;

    const int latency = Engine::latencyOf (t);   // (the plugins' delay: trimmed off the bounce, the compensation puts the track back in time)
    const auto folder = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("UFODAW Recordings");
    folder.createDirectory();
    const auto file = folder.getChildFile ("Freeze " + juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H-%M-%S") + " - "
                                           + juce::File::createLegalFileName (t.name) + ".wav");
    const double seconds = engine.getTotalLengthSeconds() + engine.getRenderTailSeconds();

    juce::Component::SafePointer<MainComponent> safe (this);
    Track* ref = &t;
    auto* job = new FreezeJob (engine, ref, file, seconds, this, [safe, ref, file, latency] (bool ok)
    {
        if (safe == nullptr || ! ok || safe->engine.indexOfTrack (ref) < 0)
            return;
        auto& self = *safe;

        self.beginEdit();
        auto* twin = self.engine.addTrack (ref->name + " (frozen)");
        const int id = twin != nullptr ? self.engine.addClipFromFile (*twin, file, 0.0) : -1;
        if (twin == nullptr || id < 0)
        {
            if (twin != nullptr)
                self.engine.removeTrack (twin);
            self.cancelEdit();
            file.deleteFile();
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Freeze failed", "The bounce could not be loaded.");
            return;
        }

        Clip whole;
        if (latency > 0 && self.engine.getClip (*twin, id, whole))
        {
            const double trim = (double) latency / self.engine.getSampleRate();
            self.engine.setClipRegion (*twin, id, 0.0, trim, juce::jmax (0.01, whole.sourceLength() - trim));
        }

        twin->colour = ref->colour;
        self.engine.setTrackOutput (*twin, ref->output);
        for (auto& s : ref->sends)
            self.engine.setSend (*twin, *s.bus, s.level, s.preFader);

        ref->frozen = true;
        ref->freezeTwin = twin;
        for (int from = self.engine.indexOfTrack (twin), to = self.engine.indexOfTrack (ref) + 1; from > to; --from)   // right after the original
            self.engine.moveTrack (*twin, -1);

        self.rebuildTrackUI();
        self.clipsChanged();
    });
    job->launchThread();   // deletes itself when it is done
}

void MainComponent::unfreezeTrack (Track& t)
{
    if (! t.frozen.load())
        return;
    if (auto* twin = t.freezeTwin; twin != nullptr && engine.indexOfTrack (twin) >= 0)
        removeTrackUI (twin);   // (this takes the undo snapshot)
    else
        beginEdit();
    t.freezeTwin = nullptr;
    t.frozen = false;
    rebuildTrackUI();
    clipsChanged();
}

// Export stems: pick a folder, pick the format, then every track is rendered to its own WAV file in it.
void MainComponent::exportStemsDialog()
{
    finishRecording();
    engine.stop();

    // which tracks make a stem: muted and empty tracks don't
    std::vector<StemExportJob::Item> items;
    juce::StringArray skipped;
    for (int i = 0; i < engine.getNumTracks(); ++i)
    {
        auto* t = engine.getTrack (i);
        if (t->mute)                           { skipped.add (t->name + " (muted)"); continue; }
        if (! engine.trackHasOutput (*t))      { skipped.add (t->name + " (nothing to play)"); continue; }
        items.push_back ({ t, juce::File (juce::String::formatted ("%02d - ", i + 1) + juce::File::createLegalFileName (t->name) + ".wav") });
    }

    if (items.empty())
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "No stems to export",
                                                "There are no tracks with anything to play (muted tracks are skipped).");
        return;
    }

    auto startFolder = currentProject != juce::File() ? currentProject.getParentDirectory()
                                                       : juce::File::getSpecialLocation (juce::File::userMusicDirectory);
    chooser = std::make_unique<juce::FileChooser> ("Choose the folder for the stems", startFolder);
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                          [this, items, skipped] (const juce::FileChooser& fc)
                          {
                              const auto folder = fc.getResult();
                              if (folder == juce::File() || ! folder.createDirectory().wasOk())
                                  return;

                              juce::StringArray names;
                              for (auto& item : items)
                                  names.add (item.file.getFileNameWithoutExtension());

                              auto* options = new juce::AlertWindow ("Export stems",
                                                                     juce::String ((int) items.size()) + " stems will be written to\n"
                                                                         + folder.getFullPathName() + "\n\n"
                                                                         + "Each track is rendered on its own, after its effects, volume and pan. "
                                                                           "Existing files with the same name are replaced.",
                                                                     juce::MessageBoxIconType::QuestionIcon);
                              juce::StringArray formatNames;
                              for (auto& choice : AudioExport::choices())
                                  formatNames.add (choice.label);
                              options->addComboBox ("format", formatNames, "Format");
                              options->addComboBox ("master", { "Without master volume and limiter (stems add up to the mix)",
                                                                "With master volume and limiter on every stem" }, "Master");
                              options->getComboBoxComponent ("format")->setSelectedItemIndex (UiSettings::exportChoice);
                              options->getComboBoxComponent ("master")->setSelectedItemIndex (0);
                              options->addButton ("Export", 1, juce::KeyPress (juce::KeyPress::returnKey));
                              options->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

                              options->enterModalState (true,
                                  juce::ModalCallbackFunction::create ([this, options, items, skipped, folder] (int result)
                                  {
                                      if (result != 1)
                                          return;

                                      const int formatIndex = juce::jlimit (0, (int) AudioExport::choices().size() - 1,
                                                                            options->getComboBoxComponent ("format")->getSelectedItemIndex());
                                      UiSettings::exportChoice = formatIndex;
                                      saveUiSettings();
                                      const auto extension = AudioExport::extensionOf (AudioExport::choices()[(size_t) formatIndex].format);
                                      const bool withMaster = options->getComboBoxComponent ("master")->getSelectedItemIndex() == 1;

                                      auto fullItems = items;
                                      for (auto& item : fullItems)
                                          item.file = folder.getChildFile (item.file.getFileNameWithoutExtension() + "." + extension);

                                      auto* job = new StemExportJob (engine, std::move (fullItems),
                                                                     engine.getTotalLengthSeconds() + engine.getRenderTailSeconds(),
                                                                     formatIndex, withMaster, folder, skipped, this);
                                      job->launchThread();   // deletes itself when it is done
                                  }),
                                  true);
                          });
}

// The mix is rendered on a background thread behind a progress window (with a cancel button), in the format that was chosen.
class MixExportJob : public juce::ThreadWithProgressWindow
{
public:
    MixExportJob (Engine& e, juce::File target, double lengthSeconds, int formatChoiceIndex, juce::Component* centreAround)
        : juce::ThreadWithProgressWindow ("Exporting the mix", true, true, 5000, "Cancel", centreAround),
          engine (e), file (std::move (target)), seconds (lengthSeconds), formatChoice (formatChoiceIndex)
    {
    }

    void run() override
    {
        Engine::RenderOptions options;
        options.formatChoice = formatChoice;
        options.shouldCancel = [this] { return threadShouldExit(); };
        options.onProgress = [this] (double f) { setProgress (f); };
        ok = engine.renderToFile (file, seconds, options);
    }

    // Message thread, when it is done or cancelled (this object deletes itself here, like the stem export).
    void threadComplete (bool userPressedCancel) override
    {
        if (userPressedCancel)
        {
            file.deleteFile();   // (half a file helps nobody)
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Export cancelled", "Nothing was written.");
        }
        else
        {
            if (! ok)
                file.deleteFile();
            juce::AlertWindow::showMessageBoxAsync (ok ? juce::MessageBoxIconType::InfoIcon : juce::MessageBoxIconType::WarningIcon,
                                                    ok ? "Export finished" : "Export failed", file.getFullPathName());
        }
        delete this;
    }

private:
    Engine& engine;
    juce::File file;
    double seconds;
    int formatChoice;
    bool ok = false;
};

void MainComponent::exportDialog()
{
    if (engine.getTotalLengthSeconds() <= 0.0)
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon,
                                                "Nothing to export", "Add a track first.");
        return;
    }

    // first the format (WAV, FLAC, AIFF, MP3, Ogg Vorbis), then where to save it
    auto* options = new juce::AlertWindow ("Export the mix", "Choose the file format. WAV, FLAC and AIFF keep the sound exactly; MP3 and "
                                                             "Ogg Vorbis are smaller, with some loss.", juce::MessageBoxIconType::QuestionIcon);
    juce::StringArray formatNames;
    for (auto& choice : AudioExport::choices())
        formatNames.add (choice.label);
    options->addComboBox ("format", formatNames, "Format");
    options->getComboBoxComponent ("format")->setSelectedItemIndex (UiSettings::exportChoice);
    options->addButton ("Next...", 1, juce::KeyPress (juce::KeyPress::returnKey));
    options->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    options->enterModalState (true, juce::ModalCallbackFunction::create ([this, options] (int result)
    {
        if (result != 1)
            return;
        const int formatIndex = juce::jlimit (0, (int) AudioExport::choices().size() - 1,
                                              options->getComboBoxComponent ("format")->getSelectedItemIndex());
        UiSettings::exportChoice = formatIndex;
        saveUiSettings();
        const auto extension = AudioExport::extensionOf (AudioExport::choices()[(size_t) formatIndex].format);

        chooser = std::make_unique<juce::FileChooser> ("Export the mix", currentProject.withFileExtension (extension), "*." + extension);
        chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                                  | juce::FileBrowserComponent::warnAboutOverwriting,
                              [this, formatIndex, extension] (const juce::FileChooser& fc)
                              {
                                  auto f = fc.getResult();
                                  if (f == juce::File())
                                      return;

                                  finishRecording();
                                  engine.stop();
                                  f = f.withFileExtension (extension);
                                  auto* job = new MixExportJob (engine, f, engine.getTotalLengthSeconds() + engine.getRenderTailSeconds(), formatIndex, this);
                                  job->launchThread();   // deletes itself when it is done
                              });
    }), true);
}

//==============================================================================
// Project tabs: only the shown project has the audio device

void MainComponent::suspendAudio()
{
    mixerWindow.reset();
    finishRecording();
    engine.finishAutomationRecording();   // a ride in progress ends as a take
    engine.silenceEverything();
    stopTimer();
    if (isDirty())
        autosaveNow();   // (an inactive project's unsaved work is kept as well)
    samplerWindow.reset();
    padSamplerWindow.reset();
    drumRackWindow.reset();
    pluginHost.closeAllEditors();   // plugin windows belong to the tab that is shown

    deviceManager.removeChangeListener (this);
    deviceManager.removeMidiInputDeviceCallback ({}, &midiForwarder);
    deviceManager.removeAudioCallback (&player);
}

void MainComponent::resumeAudio()
{
    deviceManager.addChangeListener (this);
    deviceManager.addAudioCallback (&player);
    deviceManager.addMidiInputDeviceCallback ({}, &midiForwarder);
    enableAllMidiInputs();
    updateAudioInfo();
    startTimerHz (30);
    resized();
    grabKeyboardFocus();
}

bool MainComponent::saveForClose()
{
    if (! isDirty())
        return true;
    if (currentProject == juce::File())
    {
        saveProjectAs();
        return false;
    }
    saveProject (currentProject);
    return ! isDirty();
}

// While playing, the arrangement turns a page when the playhead reaches the right edge (and finds it again after a loop or a jump).
void MainComponent::followPlayhead (double seconds)
{
    if (! UiSettings::follow || ! engine.isPlaying() || juce::ModifierKeys::currentModifiers.isAnyMouseButtonDown())
        return;
    const int viewX = laneViewport.getViewPositionX();
    const int visibleW = laneViewport.getMaximumVisibleWidth();
    const int x = (int) (seconds * pixelsPerSecond);
    if (visibleW > 100 && (x > viewX + visibleW - 40 || x < viewX))
        laneViewport.setViewPosition (juce::jmax (0, x - visibleW / 10), laneViewport.getViewPositionY());
}

void MainComponent::initialiseDevices (juce::AudioDeviceManager& manager)
{
    const std::unique_ptr<juce::XmlElement> saved = juce::XmlDocument::parse (audioSettingsFile());
    const auto error = manager.initialise (2, 2, saved.get(), true);
    if (error.isNotEmpty())
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Audio device", error);
}

//==============================================================================
// The quick tour

void MainComponent::maybeStartTour()
{
    if (UiSettings::tourDone || tour != nullptr)
        return;
    // Wait for the window, and for a dialog that may be open first (a recovery question, an audio device error): look again every
    // second for a while.
    juce::Component::SafePointer<MainComponent> safe (this);
    auto attempt = std::make_shared<std::function<void (int)>>();
    *attempt = [safe, attempt] (int triesLeft)
    {
        if (safe == nullptr || UiSettings::tourDone || safe->tour != nullptr)
            return;
        if (safe->isShowing() && juce::ModalComponentManager::getInstance()->getNumModalComponents() == 0)
        {
            safe->startTour();
            return;
        }
        if (triesLeft > 0)
            juce::Timer::callAfterDelay (1000, [attempt, triesLeft] { (*attempt) (triesLeft - 1); });
    };
    juce::Timer::callAfterDelay (900, [attempt] { (*attempt) (60); });
}

void MainComponent::startTour()
{
    if (tour != nullptr)
        return;

    using Step = TourOverlay::Step;
    std::vector<Step> steps;
    auto mine = [this] (juce::Component& c) { return getLocalArea (c.getParentComponent(), c.getBounds()); };
    juce::ignoreUnused (mine);

    steps.push_back ({ "Welcome to UFODAW",
                       "A DAW for recording and writing music: audio, MIDI, drums, a sampler and VST plugins. This short tour shows where things are. "
                       "You can run it again from File > Quick tour.", nullptr });
    steps.push_back ({ "The rail",
                       "File (new, open, save, export), Add a track, Plugins, Tuner and Mixer are on the left. Audio settings (driver, interface, "
                       "buffer size) are at the bottom.",
                       [this] { return railFile.getBounds().getUnion (railMixer.getBounds()); } });
    steps.push_back ({ "Add a track",
                       "Audio tracks, MIDI tracks (built-in synth or VST), drums, a sampler or a bus. You can also drop audio files on the window "
                       "to make tracks from them.",
                       [this] { return railAdd.getBounds(); } });
    steps.push_back ({ "Time and tempo",
                       "The clock, the tempo (Tempo opens tempo and time-signature changes), the count-in for recording, and Follow, which scrolls "
                       "the arrangement along with the playhead.",
                       [this] { return timeLabel.getBounds().getUnion (followButton.getBounds()); } });
    steps.push_back ({ "Your tracks",
                       "The dot arms a track for recording. A automates volume, pan and effects (and can record your fader rides as takes), "
                       "VST holds plugins, FX the built-in effects, M and S mute and solo. A drum track opens the drum rack. Drag a track by its name to move it up or down.",
                       [this] { return headerViewport.getBounds(); } });
    steps.push_back ({ "The arrangement",
                       "Drag clips to move them, S splits at the playhead, right-click a clip for more (takes, comping, reverse, stretch). "
                       "Drag the right edge of a MIDI clip to repeat the pattern; with Shift it stretches.",
                       [this] { return laneViewport.getBounds(); } });
    steps.push_back ({ "Transport",
                       "Space plays and pauses, R records. Loop, Punch and Click (the metronome) sit next to the buttons.",
                       [this] { return transportPill; } });
    steps.push_back ({ "STOP ALL",
                       "One big button that stops playback and recording and cuts every sound, plugins and MIDI hardware included "
                       "(Ctrl+Shift+Space).",
                       [this] { return stopAllButton.getBounds(); } });
    steps.push_back ({ "That's it",
                       "Tabs along the top hold several projects (Ctrl+Tab switches). Ctrl+Z undoes almost everything, also inside the sampler "
                       "and drum rack. The README explains everything else. Have fun!", nullptr });

    juce::Component::SafePointer<MainComponent> safe (this);
    tour = std::make_unique<TourOverlay> (std::move (steps), [safe] (bool)
    {
        if (safe == nullptr)
            return;
        safe->tour.reset();
        UiSettings::tourDone = true;
        safe->saveUiSettings();
        safe->grabKeyboardFocus();
    });
    addAndMakeVisible (*tour);
    tour->setBounds (getLocalBounds());
    tour->toFront (true);
    tour->grabKeyboardFocus();
}

//==============================================================================
// Sub-tracks (note lanes) of MIDI tracks

namespace
{
    struct DrumLaneName { int pitch; const char* name; };
    const DrumLaneName gmDrumLanes[] = {
        { 36, "Kick" }, { 38, "Snare" }, { 37, "Side stick" }, { 39, "Clap" }, { 42, "Closed hat" }, { 46, "Open hat" }, { 44, "Pedal hat" },
        { 41, "Low tom" }, { 45, "Mid tom" }, { 48, "High tom" }, { 49, "Crash" }, { 51, "Ride" }, { 53, "Ride bell" }, { 56, "Cowbell" },
    };

    // The notes of a sub-track that is taken away go with it (the pitches another remaining sub-track still covers are kept).
    void removeLaneNotes (Engine& engine, Track& track, const SubLane& gone, const std::vector<SubLane>& remaining)
    {
        for (auto& clip : engine.getMidiClips (track))
        {
            auto notes = clip.notes;
            const auto end = std::remove_if (notes.begin(), notes.end(), [&] (const Note& n)
            {
                if (! gone.covers (n.pitch))
                    return false;
                for (auto& other : remaining)
                    if (other.covers (n.pitch))
                        return false;
                return true;
            });
            if (end != notes.end())
            {
                notes.erase (end, notes.end());
                engine.setMidiNotes (track, clip.id, std::move (notes));
            }
        }
    }

    juce::String laneNameForPitch (int pitch)
    {
        for (auto& d : gmDrumLanes)
            if (d.pitch == pitch)
                return d.name;
        if (pitch == 35) return "Kick 2";
        if (pitch == 40) return "Snare 2";
        if (pitch == 43) return "Floor tom";
        if (pitch == 47) return "Mid tom 2";
        if (pitch == 50) return "High tom 2";
        if (pitch == 52) return "China";
        if (pitch == 54) return "Tambourine";
        if (pitch == 55) return "Splash";
        if (pitch == 57) return "Crash 2";
        if (pitch == 59) return "Ride 2";
        return juce::MidiMessage::getMidiNoteName (pitch, true, true, 3);
    }
}

// ---- track groups

// A strip in the lane area next to a group header (the groups have nothing to show in time, it just keeps the rows together).
namespace
{
    struct GroupBand : juce::Component
    {
        GroupBand() { setInterceptsMouseClicks (false, false); }
        void paint (juce::Graphics& g) override
        {
            g.setColour (Theme::yellowDim.withAlpha (0.10f));
            g.fillRoundedRectangle (getLocalBounds().toFloat().reduced (0.0f, 2.0f), 6.0f);
        }
    };
}

// Builds the group header rows when the groups (their names, order, collapsed state) changed.
void MainComponent::syncGroupUI()
{
    juce::String signature;
    int previous = 0;
    for (int i = 0; i < engine.getNumTracks(); ++i)
    {
        const int gid = engine.getTrack (i)->groupId;
        if (gid != 0 && gid != previous)
            if (auto* g = engine.findGroup (gid))
                signature << gid << ":" << g->name << ";";
        previous = gid;
    }
    if (signature == groupSignature)
        return;
    groupSignature = signature;

    groupHeaders.clear();
    groupBands.clear();
    previous = 0;
    for (int i = 0; i < engine.getNumTracks(); ++i)
    {
        const int gid = engine.getTrack (i)->groupId;
        if (gid != 0 && gid != previous && engine.findGroup (gid) != nullptr)
        {
            headerHolder.addAndMakeVisible (groupHeaders.add (new GroupHeader (*this, gid)));
            laneHolder.addAndMakeVisible (groupBands.add (new GroupBand()));
        }
        previous = gid;
    }
}

void MainComponent::groupChanged()
{
    groupSignature = "*";   // (made again)
    rebuildTrackUI();
    ++editCounter;
}

void MainComponent::toggleGroupCollapsed (int groupId)
{
    if (auto* g = engine.findGroup (groupId))
    {
        g->collapsed = ! g->collapsed;
        layoutContent();
        for (auto* h : groupHeaders)
            h->repaint();
        ++editCounter;
    }
}

// All the tracks of the group to muted (or soloed), unless all of them already are: then all back.
void MainComponent::muteSoloGroup (int groupId, bool solo)
{
    const auto members = engine.groupMembers (groupId);
    if (members.empty())
        return;
    bool all = true;
    for (auto* t : members)
        all = all && (solo ? t->solo.load() : t->mute.load());
    beginEdit();
    for (auto* t : members)
    {
        if (solo) t->solo = ! all;
        else      t->mute = ! all;
    }
}

void MainComponent::renameGroup (int groupId)
{
    auto* group = engine.findGroup (groupId);
    if (group == nullptr)
        return;
    juce::Component::SafePointer<MainComponent> safe (this);
    auto* box = new juce::AlertWindow ("Rename group", "A name for this group of tracks:", juce::MessageBoxIconType::NoIcon);
    box->addTextEditor ("text", group->name);
    box->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
    box->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    box->enterModalState (true, juce::ModalCallbackFunction::create ([safe, box, groupId] (int ok)
    {
        if (ok != 1 || safe == nullptr)
            return;
        const auto text = box->getTextEditorContents ("text").trim();
        if (auto* g = safe->engine.findGroup (groupId); g != nullptr && text.isNotEmpty() && text != g->name)
        {
            safe->beginEdit();
            g->name = text;
            safe->groupSignature = "*";
            safe->layoutContent();
        }
    }), true);
}

void MainComponent::showGroupMenu (int groupId)
{
    auto* group = engine.findGroup (groupId);
    if (group == nullptr)
        return;
    juce::Component::SafePointer<MainComponent> safe (this);
    juce::PopupMenu menu;
    menu.addItem (1, group->collapsed ? "Expand" : "Collapse");
    menu.addItem (2, "Rename...");
    menu.addSeparator();
    menu.addItem (3, "Ungroup (the tracks stay)");
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea (juce::Rectangle<int> (1, 1).withCentre (juce::Desktop::getMousePosition())),
                        [safe, groupId] (int result)
    {
        if (safe == nullptr || result == 0)
            return;
        if (result == 1) safe->toggleGroupCollapsed (groupId);
        else if (result == 2) safe->renameGroup (groupId);
        else if (result == 3)
        {
            safe->beginEdit();
            for (auto* t : safe->engine.groupMembers (groupId))
                safe->engine.setTrackGroup (*t, 0);
            safe->groupChanged();
        }
    });
}

// The "Group" items of a track's right-click menu: a new group with it, join an existing one, leave its group.
void MainComponent::showTrackGroupMenuItems (juce::PopupMenu& menu, const Track& t)
{
    juce::PopupMenu sub;
    sub.addItem (70, "New group with this track");
    int k = 0;
    for (auto& g : engine.getGroups())
        sub.addItem (80 + k++, "Move into: " + g.name, true, g.id == t.groupId);
    if (t.groupId != 0)
    {
        sub.addSeparator();
        sub.addItem (71, "Take it out of its group");
    }
    menu.addSeparator();
    menu.addSubMenu ("Group (folder)", sub);
}

// Builds or removes the sub-track rows so that they match the tracks (the shown sub-tracks of every MIDI track, in order).
void MainComponent::syncSubLaneUI()
{
    juce::String signature;
    for (int i = 0; i < engine.getNumTracks(); ++i)
        if (auto* t = engine.getTrack (i); t->type == Track::Type::Midi && t->subLanesOpen)
            for (int s = 0; s < (int) t->subLanes.size(); ++s)
                signature << juce::String::toHexString ((juce::int64) (std::intptr_t) t) << ":" << s << ":" << t->subLanes[(size_t) s].name << ":"
                          << t->subLanes[(size_t) s].pitchLo << "-" << t->subLanes[(size_t) s].pitchHi << ";";
    if (signature == subSignature && lanes.size() > 0 && headers.size() == lanes.size())
        return;
    subSignature = signature;

    subRows.clear();
    subHeaders.clear();
    subFooters.clear();
    for (int i = 0; i < engine.getNumTracks(); ++i)
        if (auto* t = engine.getTrack (i); t->type == Track::Type::Midi && t->subLanesOpen)
            for (int s = 0; s < (int) t->subLanes.size(); ++s)
            {
                auto* row = subRows.add (new SubLaneRow (*this, *t, s));
                laneHolder.addAndMakeVisible (row);
                auto* header = subHeaders.add (new SubLaneHeader (*this, *t, s));
                headerHolder.addAndMakeVisible (header);
            }
    for (int i = 0; i < engine.getNumTracks(); ++i)
        if (auto* t = engine.getTrack (i); t->type == Track::Type::Midi && t->subLanesOpen && ! t->subLanes.empty())
            headerHolder.addAndMakeVisible (subFooters.add (new SubLaneFooter (*this, *t)));
}

// Right-click on a sub-track (or double-click its header): rename, the pitch it stands for, remove.
void MainComponent::showSubLaneMenu (Track& t, int sub)
{
    if (! juce::isPositiveAndBelow (sub, (int) t.subLanes.size()))
        return;
    juce::Component::SafePointer<MainComponent> safe (this);
    const TrackRef ref (&t);

    juce::PopupMenu menu;
    menu.addItem (1, "Rename...");
    menu.addItem (2, "Pitch...");
    menu.addItem (3, "Remove notes of this sub-track", true);
    menu.addItem (4, "Remove this sub-track (and its notes)");
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea (juce::Rectangle<int> (1, 1).withCentre (juce::Desktop::getMousePosition())),
                        [safe, ref, sub] (int result)
    {
        if (safe == nullptr || result == 0 || safe->engine.indexOfTrack (ref) < 0 || ! juce::isPositiveAndBelow (sub, (int) ref->subLanes.size()))
            return;
        auto& self = *safe;
        auto& track = *ref;
        const auto lane = track.subLanes[(size_t) sub];

        if (result == 1 || result == 2)
        {
            auto* box = new juce::AlertWindow (result == 1 ? "Rename sub-track" : "Pitch of the sub-track",
                                               result == 1 ? "A name for this sub-track:" : "The MIDI note this sub-track stands for (0-127). Give two numbers (36-38) for a range of pitches.",
                                               juce::MessageBoxIconType::NoIcon);
            box->addTextEditor ("text", result == 1 ? lane.name : (lane.pitchLo == lane.pitchHi ? juce::String (lane.pitchLo)
                                                                                                : juce::String (lane.pitchLo) + "-" + juce::String (lane.pitchHi)));
            box->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
            box->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
            box->enterModalState (true, juce::ModalCallbackFunction::create ([safe, ref, sub, box, result] (int ok)
            {
                if (ok != 1 || safe == nullptr || safe->engine.indexOfTrack (ref) < 0 || ! juce::isPositiveAndBelow (sub, (int) ref->subLanes.size()))
                    return;
                auto text = box->getTextEditorContents ("text").trim();
                auto lanes = ref->subLanes;
                if (result == 1)
                {
                    if (text.isEmpty())
                        return;
                    lanes[(size_t) sub].name = text;
                }
                else
                {
                    const auto parts = juce::StringArray::fromTokens (text, "-", "");
                    if (parts.isEmpty() || parts[0].trim().isEmpty())
                        return;
                    const int lo = juce::jlimit (0, 127, parts[0].trim().getIntValue());
                    const int hi = parts.size() > 1 ? juce::jlimit (lo, 127, parts[1].trim().getIntValue()) : lo;
                    lanes[(size_t) sub].pitchLo = lo;
                    lanes[(size_t) sub].pitchHi = hi;
                }
                safe->beginEdit();
                safe->engine.setSubLanes (*ref, std::move (lanes), true);
                safe->clipsChanged();
            }), true);
            return;
        }

        if (result == 3)   // every note of this pitch (range) in the track's clips
        {
            self.beginEdit();
            for (auto& clip : self.engine.getMidiClips (track))
            {
                auto notes = clip.notes;
                notes.erase (std::remove_if (notes.begin(), notes.end(), [&lane] (const Note& n) { return lane.covers (n.pitch); }), notes.end());
                self.engine.setMidiNotes (track, clip.id, std::move (notes));
            }
            self.clipsChanged();
        }
        else if (result == 4)   // the sub-track goes, and its notes with it
        {
            self.beginEdit();
            auto lanes = track.subLanes;
            lanes.erase (lanes.begin() + sub);
            removeLaneNotes (self.engine, track, lane, lanes);
            self.engine.setSubLanes (track, std::move (lanes), true);
            self.clipsChanged();
        }
    });
}

void MainComponent::applySubLaneChoice (Track& t, int choice)
{
    if (choice == 0)
        return;
    auto lanes = t.subLanes;
    bool open = true;

    if (choice == 1)
    {
        open = ! t.subLanesOpen;
    }
    else if (choice == 2)   // one sub-track per pitch that the clips use, lowest first
    {
        std::set<int> pitches;
        for (auto& clip : engine.getMidiClips (t))
            for (auto& n : clip.notes)
                pitches.insert (n.pitch);
        if (pitches.empty())
        {
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "No notes yet",
                                                    "This track has no notes: use the drum preset, or add sub-tracks by hand.");
            return;
        }
        // drum names (kick, snare...) when most of the pitches are in the General MIDI drum range, note names otherwise
        int inDrumRange = 0;
        for (int p : pitches)
            inDrumRange += (p >= 35 && p <= 59) ? 1 : 0;
        const bool drumNames = inDrumRange * 10 >= (int) pitches.size() * 7;
        lanes.clear();
        for (int p : pitches)
            lanes.push_back ({ drumNames ? laneNameForPitch (p) : juce::MidiMessage::getMidiNoteName (p, true, true, 3), p, p, false, false });
    }
    else if (choice == 3)
    {
        lanes.clear();
        for (auto& d : gmDrumLanes)
            lanes.push_back ({ d.name, d.pitch, d.pitch, false, false });
    }
    else if (choice == 4)
    {
        int pitch = lanes.empty() ? 36 : lanes.back().pitchHi + 1;
        pitch = juce::jlimit (0, 127, pitch);
        lanes.push_back ({ laneNameForPitch (pitch), pitch, pitch, false, false });
    }
    else if (choice == 5)
    {
        lanes.clear();
    }
    else if (choice == 6)   // take the last sub-track away, with its notes
    {
        if (lanes.empty())
            return;
        const auto gone = lanes.back();
        lanes.pop_back();
        beginEdit();
        removeLaneNotes (engine, t, gone, lanes);
        engine.setSubLanes (t, std::move (lanes), open);
        clipsChanged();
        return;
    }

    beginEdit();
    engine.setSubLanes (t, std::move (lanes), open);
    clipsChanged();
}


// The + / - button of a track header: show or hide its sub-tracks.
void MainComponent::toggleSubLanes (Track& t)
{
    if (engine.indexOfTrack (&t) < 0)
        return;
    if (t.subLanes.empty())   // nothing to show yet: the + makes the first sub-track
    {
        changeSubLaneCount (t, 1);
        return;
    }
    beginEdit();
    engine.setSubLanes (t, t.subLanes, ! t.subLanesOpen);
    clipsChanged();
}

void MainComponent::changeSubLaneCount (Track& t, int delta)
{
    // Deferred: this is called from a button of the row that is rebuilt by the change.
    const TrackRef ref (&t);
    juce::Component::SafePointer<MainComponent> safe (this);
    juce::MessageManager::callAsync ([safe, ref, delta]
    {
        if (safe == nullptr || safe->engine.indexOfTrack (ref) < 0)
            return;
        if (delta > 0)
            safe->applySubLaneChoice (*ref, 4);
        else if (! ref->subLanes.empty())
            safe->applySubLaneChoice (*ref, 6);
    });
}

// Renamed from the header of the sub-track (done after the label's own callback has finished: the header is rebuilt).
void MainComponent::renameSubLane (Track& t, int sub, const juce::String& name)
{
    const TrackRef ref (&t);
    juce::Component::SafePointer<MainComponent> safe (this);
    juce::MessageManager::callAsync ([safe, ref, sub, name]
    {
        if (safe == nullptr || safe->engine.indexOfTrack (ref) < 0 || ! juce::isPositiveAndBelow (sub, (int) ref->subLanes.size()))
            return;
        auto lanes = ref->subLanes;
        lanes[(size_t) sub].name = name;
        safe->beginEdit();
        safe->engine.setSubLanes (*ref, std::move (lanes), true);
        safe->clipsChanged();
    });
}

// A MIDI track whose instrument is a drum plugin (EZdrummer, Superior Drummer, Addictive Drums, BFD...) gets the drum sub-tracks by itself,
// once: kick, snare, hats... shown under the track, a lane per sound. (Each track is looked at once; remove them and they stay away.)
void MainComponent::autoSubLanes()
{
    for (int i = 0; i < engine.getNumTracks(); ++i)
    {
        auto* t = engine.getTrack (i);
        if (t == nullptr || t->type != Track::Type::Midi || t->subAutoDone || t->instrument == nullptr)
            continue;
        t->subAutoDone = true;
        if (! t->subLanes.empty())
            continue;

        const auto text = (t->instrument->description.name + " " + t->instrument->description.manufacturerName).toLowerCase();
        const bool isDrumPlugin = text.contains ("drummer") || text.contains ("drums") || text.contains ("drum kit") || text.contains ("drumkit")
                                  || text.contains ("bfd") || text.contains ("drum");
        if (! isDrumPlugin)
            continue;

        std::vector<SubLane> lanes;
        for (auto& d : gmDrumLanes)
            lanes.push_back ({ d.name, d.pitch, d.pitch, false, false });
        beginEdit();   // (one undo step takes them away again)
        engine.setSubLanes (*t, std::move (lanes), true);
        clipsChanged();
    }
}

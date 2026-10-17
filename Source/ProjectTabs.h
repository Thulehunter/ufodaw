#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "MainComponent.h"
#include "Theme.h"

// Several projects open at once: a tab strip along the top of the window, one MainComponent (with its own engine) per project.
// Only the shown tab has the audio device (and the MIDI inputs); the others are suspended until they are clicked.
class ProjectTabs : public juce::Component, private juce::Timer
{
public:
    static constexpr int barHeight = 34;
    static constexpr int maxTabs = 8;   // (also the number of recovery slots)

    ProjectTabs()
    {
        newTab();
        if (auto* first = getActive())
            setSize (juce::jmax (800, preferredW), juce::jmax (500, preferredH) + barHeight);
        startTimerHz (4);   // dirty dots, titles
    }

    ~ProjectTabs() override
    {
        stopTimer();
        tabs.clear();
    }

    MainComponent* getActive() const { return tabs.empty() ? nullptr : tabs[(size_t) active].get(); }

    // At start-up: restore work from sessions that did not end properly (one recovery file per tab slot), or open a project file
    void start (const juce::File& projectToOpen)
    {
        if (projectToOpen.existsAsFile())
        {
            openFile (projectToOpen);
            return;
        }
        getActive()->offerRecovery();
        for (int slot = 1; slot < maxTabs; ++slot)
        {
            const auto recovery = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("UFODAW")
                                      .getChildFile ("autosave").getChildFile ("recovery-" + juce::String (slot) + ".mdaw");
            if (recovery.existsAsFile())
                if (auto* tab = newTab (slot))
                    tab->offerRecovery();
        }
    }

    // An empty untitled project takes the file; otherwise it opens in a tab of its own
    void openFile (const juce::File& file)
    {
        if (auto* current = getActive(); current != nullptr && current->isEmptyUntitled())
            current->openFileHere (file);
        else if (auto* tab = newTab())
            tab->openFileHere (file);
        refreshTitle();
        repaint();
    }

    MainComponent* newTab (int forcedSlot = -1)
    {
        if ((int) tabs.size() >= maxTabs)
        {
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Too many projects",
                                                    "Up to " + juce::String (maxTabs) + " projects can be open at once. Close one first.");
            return nullptr;
        }

        int slot = forcedSlot;
        if (slot < 0)   // the lowest free recovery slot
            for (slot = 0; slot < maxTabs; ++slot)
            {
                bool used = false;
                for (auto& t : tabs)
                    used = used || t->getTabSlot() == slot;
                if (! used)
                    break;
            }

        if (auto* current = getActive())   // the device is let go before the new project opens it
        {
            current->suspendAudio();
            current->setVisible (false);
        }

        auto tab = std::make_unique<MainComponent> (slot);
        auto* raw = tab.get();
        raw->requestNewTab = [safe = juce::Component::SafePointer<ProjectTabs> (this)]
        {
            juce::MessageManager::callAsync ([safe] { if (safe != nullptr) safe->newTab(); });
        };
        raw->requestOpenInTab = [safe = juce::Component::SafePointer<ProjectTabs> (this)] (const juce::File& f)
        {
            juce::MessageManager::callAsync ([safe, f] { if (safe != nullptr) safe->openFile (f); });
        };
        raw->requestCloseTab = [safe = juce::Component::SafePointer<ProjectTabs> (this), raw]
        {
            juce::MessageManager::callAsync ([safe, raw] { if (safe != nullptr) safe->closeTab (safe->indexOf (raw)); });
        };
        raw->onTitleChanged = [this] { refreshTitle(); repaint(); };

        if (tabs.empty())
        {
            preferredW = raw->getWidth();
            preferredH = raw->getHeight();
        }
        tabs.push_back (std::move (tab));
        active = (int) tabs.size() - 1;
        addAndMakeVisible (*raw);
        resized();
        refreshTitle();
        raw->grabKeyboardFocus();
        repaint();
        return raw;
    }

    void closeTab (int index)
    {
        if (! juce::isPositiveAndBelow (index, (int) tabs.size()))
            return;

        auto* tab = tabs[(size_t) index].get();
        if (tab->hasUnsavedChanges())
        {
            setActive (index);   // (so it is clear which one is meant)
            juce::Component::SafePointer<ProjectTabs> safe (this);
            juce::AlertWindow::showYesNoCancelBox (juce::MessageBoxIconType::QuestionIcon, "Close project",
                                                   "Save the changes to " + tab->getTitle() + " first?", "Save", "Don't save", "Cancel", this,
                                                   juce::ModalCallbackFunction::create ([safe, tab] (int choice)
            {
                if (safe == nullptr || choice == 0)
                    return;
                const int i = safe->indexOf (tab);
                if (i < 0)
                    return;
                if (choice == 1 && ! tab->saveForClose())
                    return;   // (an untitled project is being saved with a dialog: close it again afterwards)
                safe->removeTab (i);
            }));
            return;
        }
        removeTab (index);
    }

    void setActive (int index)
    {
        if (! juce::isPositiveAndBelow (index, (int) tabs.size()) || index == active)
            return;
        tabs[(size_t) active]->suspendAudio();
        tabs[(size_t) active]->setVisible (false);
        active = index;
        tabs[(size_t) active]->setVisible (true);
        tabs[(size_t) active]->resumeAudio();
        resized();
        refreshTitle();
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Theme::background);
        g.setColour (Theme::rowDark);
        g.fillRect (0, 0, getWidth(), barHeight);
        g.setColour (Theme::border);
        g.fillRect (0, barHeight - 1, getWidth(), 1);

        for (int i = 0; i < (int) tabs.size(); ++i)
        {
            const auto r = tabRect (i);
            const bool on = i == active;
            g.setColour (on ? Theme::panel : Theme::rowLight.withAlpha (0.7f));
            g.fillRoundedRectangle (r.toFloat().withTrimmedBottom (-6.0f), 8.0f);
            if (on)
            {
                g.setColour (Theme::orange);
                g.fillRoundedRectangle ((float) r.getX() + 8.0f, (float) r.getY(), (float) r.getWidth() - 16.0f, 2.5f, 1.2f);
            }

            auto text = r.reduced (12, 0);
            auto closeArea = text.removeFromRight (18);
            if (tabs[(size_t) i]->hasUnsavedChanges())
            {
                g.setColour (Theme::yellow);
                g.fillEllipse ((float) text.getRight() - 14.0f, (float) r.getCentreY() - 3.5f, 7.0f, 7.0f);
                text.removeFromRight (16);
            }
            g.setColour (on ? Theme::text : Theme::textDim);
            g.setFont (juce::FontOptions (13.0f, on ? juce::Font::bold : juce::Font::plain));
            g.drawText (tabs[(size_t) i]->getTitle(), text, juce::Justification::centredLeft, true);

            g.setColour (hoverClose == i ? Theme::redBright : Theme::textFaint);
            const auto c = closeArea.getCentre().toFloat();
            g.drawLine (c.x - 4.0f, c.y - 4.0f, c.x + 4.0f, c.y + 4.0f, 1.6f);
            g.drawLine (c.x - 4.0f, c.y + 4.0f, c.x + 4.0f, c.y - 4.0f, 1.6f);
        }

        g.setColour (hoverPlus ? Theme::yellow : Theme::orange);
        g.setFont (juce::FontOptions (20.0f));
        g.drawText ("+", plusRect(), juce::Justification::centred);
    }

    void resized() override
    {
        if (auto* a = getActive())
            a->setBounds (getLocalBounds().withTrimmedTop (barHeight));
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.y >= barHeight)
            return;
        if (plusRect().contains (e.getPosition()))
        {
            newTab();
            return;
        }
        for (int i = 0; i < (int) tabs.size(); ++i)
            if (tabRect (i).contains (e.getPosition()))
            {
                if (e.mods.isMiddleButtonDown() || closeRect (i).contains (e.getPosition()))
                    closeTab (i);
                else
                    setActive (i);
                return;
            }
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        int c = -1;
        for (int i = 0; i < (int) tabs.size(); ++i)
            if (closeRect (i).contains (e.getPosition()))
                c = i;
        const bool p = plusRect().contains (e.getPosition());
        if (c != hoverClose || p != hoverPlus)
        {
            hoverClose = c;
            hoverPlus = p;
            repaint (0, 0, getWidth(), barHeight);
        }
    }

    void mouseExit (const juce::MouseEvent&) override { hoverClose = -1; hoverPlus = false; repaint (0, 0, getWidth(), barHeight); }

private:
    int indexOf (MainComponent* tab) const
    {
        for (int i = 0; i < (int) tabs.size(); ++i)
            if (tabs[(size_t) i].get() == tab)
                return i;
        return -1;
    }

    void removeTab (int index)
    {
        if (tabs.size() <= 1)   // the last project: it stays, as a new empty one
        {
            auto old = std::move (tabs[0]);
            tabs.clear();
            old->suspendAudio();
            old->markClean();
            removeChildComponent (old.get());
            old.reset();
            active = 0;
            newTab();
            return;
        }

        const bool wasActive = index == active;
        auto old = std::move (tabs[(size_t) index]);
        tabs.erase (tabs.begin() + index);
        if (wasActive)
            old->suspendAudio();
            old->markClean();
        removeChildComponent (old.get());
        old->markClean();   // (a closed project leaves no recovery file: its destructor clears it)
        old.reset();

        if (index < active)
            --active;
        active = juce::jlimit (0, (int) tabs.size() - 1, active);
        if (wasActive)
        {
            for (auto& t : tabs)
                t->setVisible (false);
            tabs[(size_t) active]->setVisible (true);
            tabs[(size_t) active]->resumeAudio();
        }
        resized();
        refreshTitle();
        repaint();
    }

    int tabWidth() const
    {
        const int available = getWidth() - 60;
        return juce::jlimit (90, 210, tabs.empty() ? 210 : available / (int) tabs.size());
    }

    juce::Rectangle<int> tabRect (int i) const { return { 8 + i * (tabWidth() + 3), 5, tabWidth(), barHeight - 5 }; }
    juce::Rectangle<int> closeRect (int i) const { return tabRect (i).removeFromRight (30); }
    juce::Rectangle<int> plusRect() const { return { 8 + (int) tabs.size() * (tabWidth() + 3) + 4, 3, 30, barHeight - 6 }; }

    void refreshTitle()
    {
        if (auto* w = getTopLevelComponent(); w != nullptr && getActive() != nullptr)
            w->setName ("UFODAW - " + getActive()->getTitle());
    }

    void timerCallback() override { refreshTitle(); repaint (0, 0, getWidth(), barHeight); }

    std::vector<std::unique_ptr<MainComponent>> tabs;
    int active = 0;
    int preferredW = 1400, preferredH = 760;
    int hoverClose = -1;
    bool hoverPlus = false;
};

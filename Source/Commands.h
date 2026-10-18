#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <map>
#include "Theme.h"

// Everything the program can do, as a list of commands: a name, a place in the menus of the mind (category), an optional shortcut and
// what it does. The command palette (Ctrl+K) searches this list; the shortcuts of the keyboard come from it, and can be changed.
struct Command
{
    juce::String id, name, category;
    juce::KeyPress defaultKey;                 // invalid = no shortcut
    std::function<void()> run;
    std::function<bool()> enabled;             // empty = always
    bool hidden = false;                       // not shown in the palette (a second key for something)
    bool isEnabled() const { return ! enabled || enabled(); }
};

// The shortcuts the user changed, kept in the program's settings folder (one list for all projects).
class Shortcuts
{
public:
    static juce::File file()
    {
        auto dir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("UFODAW");
        dir.createDirectory();
        return dir.getChildFile ("shortcuts.xml");
    }

    static void load()
    {
        if (loaded())
            return;
        loaded() = true;
        if (auto xml = juce::XmlDocument::parse (file()))
            for (auto* e : xml->getChildWithTagNameIterator ("KEY"))
                user()[e->getStringAttribute ("id")] = e->getStringAttribute ("key").isEmpty() ? juce::KeyPress()
                                                                                                : juce::KeyPress::createFromDescription (e->getStringAttribute ("key"));
    }

    static void save()
    {
        juce::XmlElement xml ("SHORTCUTS");
        for (auto& [id, key] : user())
        {
            auto* e = xml.createNewChildElement ("KEY");
            e->setAttribute ("id", id);
            e->setAttribute ("key", key.isValid() ? key.getTextDescription() : juce::String());
        }
        xml.writeTo (file());
    }

    // The key a command answers to now: the one the user chose (or removed), else its default.
    static juce::KeyPress keyFor (const Command& c)
    {
        load();
        if (auto it = user().find (c.id); it != user().end())
            return it->second;
        return c.defaultKey;
    }

    static void set (const juce::String& id, const juce::KeyPress& key) { load(); user()[id] = key; save(); }
    static void resetOne (const juce::String& id) { load(); user().erase (id); save(); }
    static void resetAll() { load(); user().clear(); save(); }
    static bool isCustom (const juce::String& id) { load(); return user().count (id) > 0; }

    // Is `pressed` the key `wanted`? Letters match in either case; the modifiers must be the same.
    static bool matches (const juce::KeyPress& pressed, const juce::KeyPress& wanted)
    {
        if (! wanted.isValid())
            return false;
        auto normal = [] (const juce::ModifierKeys& m)   // (Ctrl on Windows is Command on a Mac: the same key for a shortcut)
        {
            return (m.isShiftDown() ? 1 : 0) | (m.isCommandDown() ? 2 : 0) | (m.isAltDown() ? 4 : 0);
        };
        if (normal (pressed.getModifiers()) != normal (wanted.getModifiers()))
            return false;
        int a = pressed.getKeyCode();
        const int b = wanted.getKeyCode();
        if (a >= 1 && a <= 26 && a != 8 && a != 9 && a != 13 && pressed.getModifiers().isCommandDown())
            a = 'A' + a - 1;   // (Ctrl+K can arrive as the control character 11)
        if (a == b)
            return true;
        return a < 256 && b < 256 && juce::CharacterFunctions::toLowerCase ((juce::juce_wchar) a) == juce::CharacterFunctions::toLowerCase ((juce::juce_wchar) b);
    }

    static juce::String describe (const juce::KeyPress& k) { return k.isValid() ? k.getTextDescription().replace ("ctrl + ", "Ctrl+").replace ("shift + ", "Shift+").replace ("alt + ", "Alt+") : juce::String(); }

private:
    static std::map<juce::String, juce::KeyPress>& user() { static std::map<juce::String, juce::KeyPress> m; return m; }
    static bool& loaded() { static bool l = false; return l; }
};

//==============================================================================
// The command palette: type a few letters of what you want to do, Enter does it. Up / Down choose, Esc closes.
class CommandPalette : public juce::Component, private juce::ListBoxModel
{
public:
    struct Entry
    {
        juce::String name, category, shortcut;
        std::function<void()> run;
        bool enabled = true;
    };

    CommandPalette (std::vector<Entry> all, std::function<void()> onDone) : entries (std::move (all)), done (std::move (onDone))
    {
        input.onKey = [this] (const juce::KeyPress& k)
        {
            if (k == juce::KeyPress::downKey)   { list.selectRow (juce::jmin (shown.size() - 1, list.getSelectedRow() + 1)); return true; }
            if (k == juce::KeyPress::upKey)     { list.selectRow (juce::jmax (0, list.getSelectedRow() - 1)); return true; }
            if (k == juce::KeyPress::pageDownKey) { list.selectRow (juce::jmin (shown.size() - 1, list.getSelectedRow() + 8)); return true; }
            if (k == juce::KeyPress::pageUpKey)   { list.selectRow (juce::jmax (0, list.getSelectedRow() - 8)); return true; }
            if (k == juce::KeyPress::escapeKey) { finish (-1); return true; }
            if (k == juce::KeyPress::returnKey) { finish (list.getSelectedRow()); return true; }
            return false;
        };
        input.setTextToShowWhenEmpty ("What do you want to do?  (type: warp, colour, export...)", Theme::textFaint);
        input.setFont (juce::FontOptions (18.0f));
        input.setColour (juce::TextEditor::backgroundColourId, Theme::laneBg);
        input.setColour (juce::TextEditor::textColourId, Theme::text);
        input.setColour (juce::TextEditor::outlineColourId, Theme::border);
        input.setColour (juce::TextEditor::focusedOutlineColourId, Theme::orange);
        input.onTextChange = [this] { refilter(); };
        addAndMakeVisible (input);

        list.setModel (this);
        list.setRowHeight (30);
        list.setColour (juce::ListBox::backgroundColourId, Theme::panel);
        list.setMouseMoveSelectsRows (false);
        addAndMakeVisible (list);

        refilter();
        setWantsKeyboardFocus (false);
    }

    void focusInput() { input.grabKeyboardFocus(); }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colours::black.withAlpha (0.45f));
        g.setColour (Theme::panel);
        g.fillRoundedRectangle (box.toFloat(), 10.0f);
        g.setColour (Theme::border);
        g.drawRoundedRectangle (box.toFloat(), 10.0f, 1.0f);
    }

    void resized() override
    {
        const int w = juce::jmin (600, getWidth() - 40);
        const int rows = juce::jlimit (1, 10, shown.size());
        const int h = 14 + 40 + 8 + rows * 30 + 14;
        box = juce::Rectangle<int> ((getWidth() - w) / 2, juce::jmax (30, getHeight() / 6), w, h);
        auto r = box.reduced (14);
        input.setBounds (r.removeFromTop (40));
        r.removeFromTop (8);
        list.setBounds (r);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (! box.contains (e.getPosition()))
            finish (-1);
    }

private:
    struct Input : public juce::TextEditor
    {
        std::function<bool (const juce::KeyPress&)> onKey;
        bool keyPressed (const juce::KeyPress& k) override { return (onKey && onKey (k)) || juce::TextEditor::keyPressed (k); }
    };

    void refilter()
    {
        shown.clear();
        const auto words = juce::StringArray::fromTokens (input.getText().trim().toLowerCase(), " ", "");
        std::vector<std::pair<int, int>> scored;   // (score, index): lower = better
        for (int i = 0; i < (int) entries.size(); ++i)
        {
            const auto text = (entries[(size_t) i].name + " " + entries[(size_t) i].category).toLowerCase();
            const auto name = entries[(size_t) i].name.toLowerCase();
            bool all = true;
            int score = 0;
            for (auto& w : words)
            {
                const int at = text.indexOf (w);
                if (at < 0) { all = false; break; }
                score += name.startsWith (w) ? 0 : name.contains (w) ? 5 + name.indexOf (w) : 50;
            }
            if (all)
                scored.push_back ({ score + (entries[(size_t) i].enabled ? 0 : 1000), i });
        }
        std::stable_sort (scored.begin(), scored.end(), [] (auto& a, auto& b) { return a.first < b.first; });
        for (auto& s : scored)
            shown.add (s.second);
        list.updateContent();
        if (shown.size() > 0)
            list.selectRow (0);
        resized();
        repaint();
    }

    void finish (int row)
    {
        std::function<void()> action;
        if (juce::isPositiveAndBelow (row, shown.size()) && entries[(size_t) shown[row]].enabled)
            action = entries[(size_t) shown[row]].run;
        auto closeIt = done;
        juce::MessageManager::callAsync ([closeIt, action]
        {
            if (closeIt)
                closeIt();    // (this palette is deleted here)
            if (action)
                action();     // (after it is gone: the action may open windows or dialogs)
        });
    }

    // ListBoxModel
    int getNumRows() override { return shown.size(); }

    void paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected) override
    {
        if (! juce::isPositiveAndBelow (row, shown.size()))
            return;
        const auto& e = entries[(size_t) shown[row]];
        if (selected)
        {
            g.setColour (Theme::orangeDim);
            g.fillRoundedRectangle (juce::Rectangle<float> (0.0f, 1.0f, (float) width, (float) height - 2.0f), 5.0f);
        }
        g.setColour (e.enabled ? Theme::text : Theme::textFaint);
        g.setFont (juce::FontOptions (15.0f));
        g.drawText (e.name, 10, 0, width - 200, height, juce::Justification::centredLeft, true);
        g.setColour (Theme::textDim);
        g.setFont (juce::FontOptions (12.0f));
        g.drawText (e.shortcut.isNotEmpty() ? e.shortcut : e.category, width - 190, 0, 180, height, juce::Justification::centredRight, true);
    }

    void listBoxItemClicked (int row, const juce::MouseEvent&) override { finish (row); }

    std::vector<Entry> entries;
    juce::Array<int> shown;
    std::function<void()> done;
    Input input;
    juce::ListBox list;
    juce::Rectangle<int> box;
};

//==============================================================================
// The keyboard shortcuts: every command with its key. Double-click (or Change) and press the new key.
class ShortcutsPanel : public juce::Component, private juce::ListBoxModel
{
public:
    ShortcutsPanel (std::vector<Command>& commandList, std::function<void()> onChanged, std::function<void()> onClose)
        : commands (commandList), changed (std::move (onChanged)), close (std::move (onClose))
    {
        for (int i = 0; i < (int) commands.size(); ++i)
            if (! commands[(size_t) i].hidden)
                order.add (i);
        std::stable_sort (order.begin(), order.end(), [this] (int a, int b)
        {
            return commands[(size_t) a].category != commands[(size_t) b].category ? commands[(size_t) a].category < commands[(size_t) b].category : false;
        });

        list.setModel (this);
        list.setRowHeight (28);
        list.setColour (juce::ListBox::backgroundColourId, Theme::laneBg);
        addAndMakeVisible (list);

        for (auto* b : { &changeButton, &clearButton, &defaultButton, &resetAllButton, &closeButton })
            addAndMakeVisible (*b);
        changeButton.onClick = [this] { startCapture(); };
        clearButton.onClick = [this] { assign (juce::KeyPress()); };
        defaultButton.onClick = [this]
        {
            if (auto* c = selectedCommand())
            {
                Shortcuts::resetOne (c->id);
                refresh();
            }
        };
        resetAllButton.onClick = [this] { Shortcuts::resetAll(); refresh(); };
        closeButton.onClick = [this] { if (close) close(); };

        info.setText ("Select a command and press Change, then the new key. Esc cancels. A key that another command used is taken from it.",
                      juce::dontSendNotification);
        info.setFont (juce::FontOptions (12.0f));
        info.setColour (juce::Label::textColourId, Theme::textFaint);
        addAndMakeVisible (info);
        setWantsKeyboardFocus (true);
        setSize (620, 520);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Theme::panel);
        if (capturing)
        {
            auto r = getLocalBounds().withSizeKeepingCentre (380, 110);
            g.setColour (Theme::background);
            g.fillRoundedRectangle (r.toFloat(), 10.0f);
            g.setColour (Theme::orange);
            g.drawRoundedRectangle (r.toFloat(), 10.0f, 2.0f);
            g.setColour (Theme::text);
            g.setFont (juce::FontOptions (16.0f, juce::Font::bold));
            if (auto* c = selectedCommand())
                g.drawText (c->name, r.removeFromTop (46).reduced (10, 6), juce::Justification::centred, true);
            g.setColour (Theme::textDim);
            g.setFont (juce::FontOptions (14.0f));
            g.drawText ("Press the new shortcut...   (Esc cancels)", r.reduced (10, 4), juce::Justification::centred);
        }
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (12, 10);
        info.setBounds (r.removeFromTop (20));
        r.removeFromTop (4);
        auto buttons = r.removeFromBottom (30);
        r.removeFromBottom (8);
        list.setBounds (r);
        closeButton.setBounds (buttons.removeFromRight (80));
        buttons.removeFromRight (8);
        resetAllButton.setBounds (buttons.removeFromRight (120));
        buttons.removeFromRight (8);
        defaultButton.setBounds (buttons.removeFromRight (90));
        buttons.removeFromRight (8);
        clearButton.setBounds (buttons.removeFromRight (80));
        buttons.removeFromRight (8);
        changeButton.setBounds (buttons.removeFromRight (90));
    }

    bool keyPressed (const juce::KeyPress& k) override
    {
        if (! capturing)
            return false;
        if (k == juce::KeyPress::escapeKey)
        {
            capturing = false;
            repaint();
            return true;
        }
        // a modifier alone is not a shortcut yet
        const int code = k.getKeyCode();
        assign (k);
        capturing = false;
        repaint();
        return true;
    }

    void refresh() { list.updateContent(); list.repaint(); if (changed) changed(); }

private:
    Command* selectedCommand()
    {
        const int row = list.getSelectedRow();
        return juce::isPositiveAndBelow (row, order.size()) ? &commands[(size_t) order[row]] : nullptr;
    }

    void startCapture()
    {
        if (selectedCommand() == nullptr)
            return;
        capturing = true;
        grabKeyboardFocus();
        repaint();
    }

    void assign (const juce::KeyPress& k)
    {
        auto* c = selectedCommand();
        if (c == nullptr)
            return;
        if (k.isValid())
            for (auto& other : commands)   // the key moves here
                if (&other != c && Shortcuts::matches (k, Shortcuts::keyFor (other)))
                    Shortcuts::set (other.id, juce::KeyPress());
        Shortcuts::set (c->id, k);
        refresh();
    }

    int getNumRows() override { return order.size(); }

    void paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected) override
    {
        if (! juce::isPositiveAndBelow (row, order.size()))
            return;
        const auto& c = commands[(size_t) order[row]];
        if (selected)
        {
            g.setColour (Theme::orangeDim);
            g.fillRect (0, 0, width, height);
        }
        g.setColour (Theme::text);
        g.setFont (juce::FontOptions (14.0f));
        g.drawText (c.name, 10, 0, width - 330, height, juce::Justification::centredLeft, true);
        g.setColour (Theme::textFaint);
        g.setFont (juce::FontOptions (12.0f));
        g.drawText (c.category, width - 320, 0, 130, height, juce::Justification::centredLeft, true);
        const auto key = Shortcuts::keyFor (c);
        g.setColour (Shortcuts::isCustom (c.id) ? Theme::yellow : Theme::textDim);
        g.setFont (juce::FontOptions (13.0f, Shortcuts::isCustom (c.id) ? juce::Font::bold : juce::Font::plain));
        g.drawText (key.isValid() ? Shortcuts::describe (key) : juce::String ("-"), width - 180, 0, 170, height, juce::Justification::centredRight, true);
    }

    void listBoxItemDoubleClicked (int, const juce::MouseEvent&) override { startCapture(); }

    std::vector<Command>& commands;
    juce::Array<int> order;
    std::function<void()> changed, close;
    juce::ListBox list;
    juce::Label info;
    juce::TextButton changeButton { "Change" }, clearButton { "Remove" }, defaultButton { "Default" }, resetAllButton { "Reset all" }, closeButton { "Close" };
    bool capturing = false;
};

class ShortcutsWindow : public juce::DocumentWindow
{
public:
    ShortcutsWindow (ShortcutsPanel* panel, std::function<void()> onClosed)
        : juce::DocumentWindow ("Keyboard shortcuts", Theme::background, juce::DocumentWindow::closeButton), closed (std::move (onClosed))
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

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include "Theme.h"

// A list of plugins with a search box: type part of a name, a maker or a category ("reverb", "serum", "fab") and the list narrows.
// Up / Down move through it, Enter (or a double-click) picks, Esc closes. Shown in a callout box.
// A text box that lets its owner handle Up / Down / Page keys first (a single-line editor would use them for the caret).
class PluginSearchBox : public juce::TextEditor
{
public:
    std::function<bool (const juce::KeyPress&)> onKey;
    bool keyPressed (const juce::KeyPress& k) override { return (onKey && onKey (k)) || juce::TextEditor::keyPressed (k); }
};

class PluginSearchPanel : public juce::Component, private juce::ListBoxModel
{
public:
    // `candidates` are the plugins to choose from; `builtInLabel` (if not empty) adds a first entry that stands for the
    // built-in sound. `onPick` gets the index into `candidates`, or -1 for the built-in entry.
    PluginSearchPanel (juce::Array<juce::PluginDescription> candidates, juce::String builtInLabel, std::function<void (int)> onPick)
        : plugins (std::move (candidates)), builtIn (std::move (builtInLabel)), pick (std::move (onPick))
    {
        // alphabetical, but the indices handed to `onPick` stay those of the caller's array
        for (int i = 0; i < plugins.size(); ++i)
            order.push_back (i);
        std::sort (order.begin(), order.end(), [this] (int a, int b)
        {
            return plugins.getReference (a).name.compareIgnoreCase (plugins.getReference (b).name) < 0;
        });

        search.setTextToShowWhenEmpty ("Type to search plugins...", Theme::textFaint);
        search.setFont (juce::FontOptions (15.0f));
        search.setColour (juce::TextEditor::backgroundColourId, Theme::laneBg);
        search.setColour (juce::TextEditor::textColourId, Theme::text);
        search.setColour (juce::TextEditor::outlineColourId, Theme::gridLineStrong);
        search.setColour (juce::TextEditor::focusedOutlineColourId, Theme::orange);
        search.onKey = [this] (const juce::KeyPress& k) { return handleArrows (k); };
        search.onTextChange = [this] { refilter(); };
        search.onReturnKey = [this] { choose (list.getSelectedRow()); };
        search.onEscapeKey = [this] { if (auto* box = findParentComponentOfClass<juce::CallOutBox>()) box->dismiss(); };
        addAndMakeVisible (search);

        list.setModel (this);
        list.setRowHeight (34);
        list.setColour (juce::ListBox::backgroundColourId, Theme::laneBg);
        list.setColour (juce::ListBox::outlineColourId, Theme::gridLineStrong);
        list.setOutlineThickness (1);
        addAndMakeVisible (list);

        count.setColour (juce::Label::textColourId, Theme::textFaint);
        count.setFont (juce::FontOptions (12.0f));
        addAndMakeVisible (count);

        refilter();
        setSize (440, 420);
        setWantsKeyboardFocus (false);
    }

    void visibilityChanged() override { if (isShowing()) search.grabKeyboardFocus(); }
    void parentHierarchyChanged() override { juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<PluginSearchPanel> (this)]
                                                                              { if (safe != nullptr) safe->search.grabKeyboardFocus(); }); }

    void paint (juce::Graphics& g) override { g.fillAll (Theme::panel); }

    void resized() override
    {
        auto r = getLocalBounds().reduced (10);
        search.setBounds (r.removeFromTop (30));
        r.removeFromTop (6);
        count.setBounds (r.removeFromBottom (18));
        r.removeFromBottom (4);
        list.setBounds (r);
    }

    // Up / Down in the search box move through the list
    bool keyPressed (const juce::KeyPress& k) override { return handleArrows (k); }

private:
    struct Entry
    {
        int index;            // into `plugins`, or -1 for the built-in entry
        juce::String title, detail;
    };

    bool handleArrows (const juce::KeyPress& k)
    {
        const int rows = (int) shown.size();
        if (rows == 0)
            return false;
        if (k == juce::KeyPress::downKey)  { list.selectRow (juce::jmin (rows - 1, list.getSelectedRow() + 1)); return true; }
        if (k == juce::KeyPress::upKey)    { list.selectRow (juce::jmax (0, list.getSelectedRow() - 1)); return true; }
        if (k == juce::KeyPress::pageDownKey) { list.selectRow (juce::jmin (rows - 1, list.getSelectedRow() + 8)); return true; }
        if (k == juce::KeyPress::pageUpKey)   { list.selectRow (juce::jmax (0, list.getSelectedRow() - 8)); return true; }
        return false;
    }

    // every word typed has to occur somewhere in the plugin's name, maker, category or format
    void refilter()
    {
        const auto words = juce::StringArray::fromTokens (search.getText().toLowerCase(), " ", "");
        shown.clear();

        if (builtIn.isNotEmpty() && matches (builtIn.toLowerCase(), words))
            shown.push_back ({ -1, builtIn, "the sound that comes with the program" });

        for (int i : order)
        {
            const auto& d = plugins.getReference (i);
            const auto haystack = (d.name + " " + d.manufacturerName + " " + d.category + " " + d.pluginFormatName
                                   + (d.isInstrument ? " instrument" : " effect")).toLowerCase();
            if (matches (haystack, words))
                shown.push_back ({ i, d.name, d.manufacturerName + (d.category.isNotEmpty() ? "  -  " + d.category.replace ("|", " / ") : juce::String())
                                                + (d.isInstrument && ! d.category.containsIgnoreCase ("instrument") ? "  -  instrument" : "") });
        }

        list.updateContent();
        list.selectRow (shown.empty() ? -1 : 0);
        list.repaint();
        count.setText (juce::String ((int) shown.size()) + " of " + juce::String (plugins.size() + (builtIn.isNotEmpty() ? 1 : 0)),
                       juce::dontSendNotification);
    }

    static bool matches (const juce::String& haystack, const juce::StringArray& words)
    {
        for (auto& w : words)
            if (w.isNotEmpty() && ! haystack.contains (w))
                return false;
        return true;
    }

    void choose (int row)
    {
        if (row < 0 || row >= (int) shown.size())
            return;
        const int index = shown[(size_t) row].index;
        auto callback = pick;
        // close first, then act: the callback may open another window
        if (auto* box = findParentComponentOfClass<juce::CallOutBox>())
            box->dismiss();
        if (callback)
            juce::MessageManager::callAsync ([callback, index] { callback (index); });
    }

    // ListBoxModel
    int getNumRows() override { return (int) shown.size(); }

    void paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool selected) override
    {
        if (row < 0 || row >= (int) shown.size())
            return;
        if (selected)
        {
            g.setColour (Theme::orangeDim.withAlpha (0.55f));
            g.fillRect (0, 0, w, h);
        }
        const auto& e = shown[(size_t) row];
        g.setColour (e.index < 0 ? Theme::yellow : Theme::text);
        g.setFont (juce::FontOptions (14.5f, juce::Font::bold));
        g.drawText (e.title, 10, 2, w - 20, 17, juce::Justification::centredLeft);
        g.setColour (Theme::textDim);
        g.setFont (juce::FontOptions (11.5f));
        g.drawText (e.detail, 10, 18, w - 20, 14, juce::Justification::centredLeft);
    }

    void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override { choose (row); }
    void returnKeyPressed (int row) override { choose (row); }
    void listBoxItemClicked (int row, const juce::MouseEvent& e) override
    {
        if (e.getNumberOfClicks() == 1)
            list.selectRow (row);
    }

    juce::Array<juce::PluginDescription> plugins;
    juce::String builtIn;
    std::function<void (int)> pick;
    std::vector<int> order;
    std::vector<Entry> shown;

    PluginSearchBox search;
    juce::ListBox list;
    juce::Label count;
};

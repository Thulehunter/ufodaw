#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "Theme.h"

// The versions of a project: the time-stamped copies that every save (and every ten minutes of unsaved work) leaves in the project's backups
// folder, newest first. Open one next to the current project in a tab, or go back to it (the project as it is now is kept as a copy first).
class VersionHistoryPanel : public juce::Component, private juce::ListBoxModel
{
public:
    VersionHistoryPanel (const juce::File& backupsFolder, std::function<void (const juce::File&)> onOpenInTab,
                         std::function<void (const juce::File&)> onRestore, std::function<void()> onClose)
        : folder (backupsFolder), openInTab (std::move (onOpenInTab)), restore (std::move (onRestore)), close (std::move (onClose))
    {
        files = folder.findChildFiles (juce::File::findFiles, false, "*.mdaw");
        std::sort (files.begin(), files.end(), [] (const juce::File& a, const juce::File& b) { return a.getFileName() > b.getFileName(); });

        list.setModel (this);
        list.setRowHeight (30);
        list.setColour (juce::ListBox::backgroundColourId, Theme::laneBg);
        addAndMakeVisible (list);
        if (! files.isEmpty())
            list.selectRow (0);

        for (auto* b : { &openButton, &restoreButton, &folderButton, &closeButton })
            addAndMakeVisible (*b);
        openButton.setTooltip ("Opens this version in a new tab; the project you are working on stays as it is");
        restoreButton.setTooltip ("Goes back to this version (the project as it is now is kept as a copy first, so this can be undone by opening that copy)");
        openButton.onClick = [this] { if (auto f = selectedFile(); f.existsAsFile() && openInTab) openInTab (f); };
        restoreButton.onClick = [this] { if (auto f = selectedFile(); f.existsAsFile() && restore) restore (f); };
        folderButton.onClick = [this] { folder.revealToUser(); };
        closeButton.onClick = [this] { if (close) close(); };

        info.setText (files.isEmpty() ? "No versions yet: they appear when you save the project." : "Every save keeps a copy; the newest forty are kept.",
                      juce::dontSendNotification);
        info.setColour (juce::Label::textColourId, Theme::textFaint);
        info.setFont (juce::FontOptions (12.0f));
        addAndMakeVisible (info);
        updateButtons();
        setSize (520, 400);
    }

    void paint (juce::Graphics& g) override { g.fillAll (Theme::panel); }

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
        folderButton.setBounds (buttons.removeFromRight (110));
        buttons.removeFromRight (8);
        restoreButton.setBounds (buttons.removeFromRight (130));
        buttons.removeFromRight (8);
        openButton.setBounds (buttons.removeFromRight (140));
    }

private:
    juce::File selectedFile() const
    {
        const int row = list.getSelectedRow();
        return juce::isPositiveAndBelow (row, files.size()) ? files[row] : juce::File();
    }

    void updateButtons()
    {
        const bool any = selectedFile().existsAsFile();
        openButton.setEnabled (any);
        restoreButton.setEnabled (any);
    }

    // ListBoxModel
    int getNumRows() override { return files.size(); }

    void paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected) override
    {
        if (! juce::isPositiveAndBelow (row, files.size()))
            return;
        if (selected)
        {
            g.setColour (Theme::orangeDim);
            g.fillRect (0, 0, width, height);
        }
        const auto& f = files[row];
        const auto when = f.getLastModificationTime();
        const auto age = juce::Time::getCurrentTime() - when;
        juce::String ago;
        if (age.inMinutes() < 1.0)       ago = "just now";
        else if (age.inMinutes() < 60.0) ago = juce::String ((int) age.inMinutes()) + " min ago";
        else if (age.inHours() < 48.0)   ago = juce::String ((int) age.inHours()) + " h ago";
        else                             ago = juce::String ((int) age.inDays()) + " days ago";

        g.setColour (Theme::text);
        g.setFont (juce::FontOptions (14.0f, juce::Font::bold));
        g.drawText (when.formatted ("%d %b %Y  %H:%M:%S"), 10, 0, 230, height, juce::Justification::centredLeft);
        g.setColour (Theme::textDim);
        g.setFont (juce::FontOptions (13.0f));
        g.drawText (ago, 240, 0, 120, height, juce::Justification::centredLeft);
        g.drawText (juce::File::descriptionOfSizeInBytes (f.getSize()), width - 100, 0, 90, height, juce::Justification::centredRight);
    }

    void selectedRowsChanged (int) override { updateButtons(); }
    void listBoxItemDoubleClicked (int, const juce::MouseEvent&) override { openButton.onClick(); }

    juce::File folder;
    juce::Array<juce::File> files;
    std::function<void (const juce::File&)> openInTab, restore;
    std::function<void()> close;
    juce::ListBox list;
    juce::Label info;
    juce::TextButton openButton { "Open in a new tab" }, restoreButton { "Go back to this" }, folderButton { "Show the folder" }, closeButton { "Close" };
};

class VersionHistoryWindow : public juce::DocumentWindow
{
public:
    VersionHistoryWindow (const juce::String& title, VersionHistoryPanel* panel, std::function<void()> onClosed)
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

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include "Engine.h"
#include "StarterPack.h"
#include "Theme.h"

// The sample browser: the sounds of the starter pack (by category) and folders of your own, with a search box. Click a sound to hear it (through the
// master output, apart from the project), double-click or press Add to put it on a new audio track, or drag it onto a track, a drum pad or the
// sampler. Loops of the starter pack show their tempo.
class SampleBrowserPanel : public juce::Component, private juce::Timer
{
public:
    struct Hooks
    {
        std::function<void (const juce::File&)> addToProject;   // a new audio track with this sound
    };

    SampleBrowserPanel (Engine& e, Hooks h) : engine (e), hooks (std::move (h))
    {
        formats.registerBasicFormats();

        search.setTextToShowWhenEmpty ("Search all sounds...", Theme::textFaint);
        search.onTextChange = [this] { rebuildFiles(); };
        addAndMakeVisible (search);

        addFolderButton.setTooltip ("Add a folder of your own samples to the browser");
        addFolderButton.onClick = [this]
        {
            chooser = std::make_unique<juce::FileChooser> ("Choose a folder of samples", juce::File::getSpecialLocation (juce::File::userMusicDirectory));
            chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories, [this] (const juce::FileChooser& fc)
            {
                if (fc.getResult().isDirectory())
                {
                    auto list = userFolders();
                    list.addIfNotAlreadyThere (fc.getResult().getFullPathName());
                    saveUserFolders (list);
                    rebuildCategories();
                }
            });
        };
        addAndMakeVisible (addFolderButton);

        removeFolderButton.setTooltip ("Take the selected folder of your own out of the browser (the files stay where they are)");
        removeFolderButton.onClick = [this]
        {
            const int row = categories.getSelectedRow();
            if (row >= 0 && row < (int) entries.size() && entries[(size_t) row].user)
            {
                auto list = userFolders();
                list.removeString (entries[(size_t) row].folder.getFullPathName());
                saveUserFolders (list);
                rebuildCategories();
            }
        };
        addAndMakeVisible (removeFolderButton);

        stopButton.onClick = [this] { engine.stopPreview(); };
        addAndMakeVisible (stopButton);
        addButton.setTooltip ("Puts the selected sound on a new audio track (or double-click it)");
        addButton.onClick = [this] { addSelected(); };
        addAndMakeVisible (addButton);

        autoPreview.setToggleState (true, juce::dontSendNotification);
        addAndMakeVisible (autoPreview);

        info.setColour (juce::Label::textColourId, Theme::textFaint);
        info.setFont (juce::FontOptions (12.0f));
        addAndMakeVisible (info);

        categories.setModel (&categoryModel);
        files.setModel (&fileModel);
        for (auto* l : { &categories, &files })
        {
            l->setRowHeight (26);
            l->setColour (juce::ListBox::backgroundColourId, Theme::laneBg);
            l->setColour (juce::ListBox::outlineColourId, Theme::border);
            l->setOutlineThickness (1);
            addAndMakeVisible (*l);
        }
        categories.setMultipleSelectionEnabled (false);
        files.setMultipleSelectionEnabled (false);

        rebuildCategories();
        setSize (760, 540);
        startTimerHz (10);
    }

    ~SampleBrowserPanel() override { engine.stopPreview(); }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Theme::panel);
        if (getWidth() < 520)   // (docked at the side of the main window: a line on its right edge)
        {
            g.setColour (Theme::border);
            g.fillRect (getWidth() - 1, 0, 1, getHeight());
        }
    }

    void resized() override
    {
        if (getWidth() < 520)   // narrow (docked beside the tracks): everything under each other
        {
            auto r = getLocalBounds().reduced (10, 10);
            search.setBounds (r.removeFromTop (28));
            r.removeFromTop (6);
            auto folderButtons = r.removeFromTop (26);
            addFolderButton.setBounds (folderButtons.removeFromLeft (folderButtons.getWidth() / 2 - 3));
            folderButtons.removeFromLeft (6);
            removeFolderButton.setBounds (folderButtons);
            r.removeFromTop (6);
            auto bottom1 = r.removeFromBottom (28);
            info.setBounds (r.removeFromBottom (22));
            r.removeFromBottom (4);
            autoPreview.setBounds (r.removeFromBottom (24));
            r.removeFromBottom (4);
            stopButton.setBounds (bottom1.removeFromLeft (70));
            bottom1.removeFromLeft (6);
            addButton.setBounds (bottom1);
            const int folderHeight = juce::jlimit (90, 220, r.getHeight() / 3);
            categories.setBounds (r.removeFromTop (folderHeight));
            r.removeFromTop (8);
            files.setBounds (r);
            return;
        }
        auto r = getLocalBounds().reduced (12);
        auto top = r.removeFromTop (28);
        search.setBounds (top.removeFromLeft (260));
        top.removeFromLeft (10);
        addFolderButton.setBounds (top.removeFromLeft (120));
        top.removeFromLeft (6);
        removeFolderButton.setBounds (top.removeFromLeft (150));
        r.removeFromTop (8);
        auto bottom = r.removeFromBottom (30);
        stopButton.setBounds (bottom.removeFromLeft (70));
        bottom.removeFromLeft (8);
        addButton.setBounds (bottom.removeFromLeft (150));
        bottom.removeFromLeft (14);
        autoPreview.setBounds (bottom.removeFromLeft (150));
        info.setBounds (bottom);
        r.removeFromBottom (8);
        categories.setBounds (r.removeFromLeft (190));
        r.removeFromLeft (10);
        files.setBounds (r);
    }

private:
    struct Entry { juce::String label; juce::File folder; bool user = false; bool all = false; };

    static juce::File foldersFile() { return StarterPack::folder().getParentDirectory().getChildFile ("sample-folders.txt"); }
    static juce::StringArray userFolders() { return juce::StringArray::fromLines (foldersFile().loadFileAsString()); }
    static void saveUserFolders (const juce::StringArray& list) { foldersFile().replaceWithText (list.joinIntoString ("\n")); }

    void rebuildCategories()
    {
        StarterPack::ensureInstalled();
        entries.clear();
        entries.push_back ({ "All starter sounds", StarterPack::folder(), false, true });
        for (auto& dir : StarterPack::folder().findChildFiles (juce::File::findDirectories, false))
            if (dir.getFileName() != "Projects")
                entries.push_back ({ dir.getFileName(), dir, false, false });
        for (auto& path : userFolders())
            if (path.isNotEmpty())
                entries.push_back ({ "Mine: " + juce::File (path).getFileName(), juce::File (path), true, false });
        categories.updateContent();
        if (categories.getSelectedRow() < 0)
            categories.selectRow (0);
        rebuildFiles();
    }

    void rebuildFiles()
    {
        shown.clear();
        const auto query = search.getText().trim().toLowerCase();
        auto collect = [this, &query] (const juce::File& folder)
        {
            for (auto& f : folder.findChildFiles (juce::File::findFiles, true, "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg"))
                if (query.isEmpty() || f.getFileNameWithoutExtension().toLowerCase().contains (query))
                    shown.push_back (f);
        };
        if (query.isNotEmpty())   // a search looks in everything
        {
            collect (StarterPack::folder());
            for (auto& path : userFolders())
                if (path.isNotEmpty())
                    collect (juce::File (path));
        }
        else
        {
            const int row = categories.getSelectedRow();
            if (row >= 0 && row < (int) entries.size())
                collect (entries[(size_t) row].folder);
        }
        std::sort (shown.begin(), shown.end(), [] (const juce::File& a, const juce::File& b) { return a.getFileName().compareNatural (b.getFileName()) < 0; });
        if (shown.size() > 800)
            shown.resize (800);
        files.updateContent();
        files.repaint();
        info.setText (juce::String ((int) shown.size()) + " sounds", juce::dontSendNotification);
    }

    void timerCallback() override
    {
        const bool playing = engine.isPreviewing();
        if (playing != wasPlaying)
        {
            wasPlaying = playing;
            files.repaint();
        }
    }

    void addSelected()
    {
        const int row = files.getSelectedRow();
        if (row >= 0 && row < (int) shown.size() && hooks.addToProject)
            hooks.addToProject (shown[(size_t) row]);
    }

    void preview (const juce::File& file)
    {
        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
        if (reader == nullptr || reader->lengthInSamples <= 0)
            return;
        const int length = (int) juce::jmin<juce::int64> (reader->lengthInSamples, (juce::int64) (reader->sampleRate * 60.0));
        auto buffer = std::make_shared<juce::AudioBuffer<float>> ((int) juce::jmin<juce::uint32> (reader->numChannels, 2), length);
        reader->read (buffer.get(), 0, length, 0, true, true);
        engine.startPreview (buffer, reader->sampleRate);
        info.setText (file.getFileNameWithoutExtension() + "  -  " + juce::String ((double) length / reader->sampleRate, 1) + " s"
                          + (StarterPack::loopTempo (file.getFileNameWithoutExtension()) > 0 ? "  -  a loop at " + juce::String (StarterPack::loopTempo (file.getFileNameWithoutExtension())) + " BPM" : juce::String()),
                      juce::dontSendNotification);
    }

    // the models of the two lists
    struct CategoryModel : juce::ListBoxModel
    {
        explicit CategoryModel (SampleBrowserPanel& o) : owner (o) {}
        int getNumRows() override { return (int) owner.entries.size(); }
        void paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool selected) override
        {
            if (row < 0 || row >= (int) owner.entries.size())
                return;
            if (selected) { g.setColour (Theme::orange.withAlpha (0.35f)); g.fillRect (0, 0, w, h); }
            g.setColour (owner.entries[(size_t) row].user ? Theme::yellow : Theme::text);
            g.setFont (juce::FontOptions (13.5f));
            g.drawText (owner.entries[(size_t) row].label, 10, 0, w - 14, h, juce::Justification::centredLeft, true);
        }
        void selectedRowsChanged (int) override { owner.search.setText ({}, juce::dontSendNotification); owner.rebuildFiles(); }
        SampleBrowserPanel& owner;
    };

    struct FileModel : juce::ListBoxModel
    {
        explicit FileModel (SampleBrowserPanel& o) : owner (o) {}
        int getNumRows() override { return (int) owner.shown.size(); }
        void paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool selected) override
        {
            if (row < 0 || row >= (int) owner.shown.size())
                return;
            const auto& f = owner.shown[(size_t) row];
            if (selected) { g.setColour (Theme::orange.withAlpha (0.35f)); g.fillRect (0, 0, w, h); }
            g.setColour (Theme::text);
            g.setFont (juce::FontOptions (13.5f));
            g.drawText (f.getFileNameWithoutExtension(), 10, 0, w - 140, h, juce::Justification::centredLeft, true);
            const int bpm = StarterPack::loopTempo (f.getFileNameWithoutExtension());
            g.setColour (Theme::textFaint);
            g.setFont (juce::FontOptions (12.0f));
            g.drawText (f.getParentDirectory().getFileName() + (bpm > 0 ? "  " + juce::String (bpm) + " BPM" : juce::String()), w - 200, 0, 190, h, juce::Justification::centredRight, true);
        }
        void selectedRowsChanged (int row) override
        {
            if (row >= 0 && row < (int) owner.shown.size() && owner.autoPreview.getToggleState())
                owner.preview (owner.shown[(size_t) row]);
        }
        void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override
        {
            owner.files.selectRow (row);
            owner.addSelected();
        }
        juce::var getDragSourceDescription (const juce::SparseSet<int>& rows) override
        {
            // dragging a sound: it is handed to whatever is under the mouse as a file (a track lane, a drum pad, the sampler)
            if (rows.size() > 0 && rows[0] < (int) owner.shown.size())
            {
                juce::StringArray paths;
                paths.add (owner.shown[(size_t) rows[0]].getFullPathName());
                juce::DragAndDropContainer::performExternalDragDropOfFiles (paths, false);
            }
            return {};
        }
        SampleBrowserPanel& owner;
    };

    Engine& engine;
    Hooks hooks;
    juce::AudioFormatManager formats;
    std::vector<Entry> entries;
    std::vector<juce::File> shown;
    bool wasPlaying = false;
    std::unique_ptr<juce::FileChooser> chooser;

    CategoryModel categoryModel { *this };
    FileModel fileModel { *this };
    juce::ListBox categories, files;
    juce::TextEditor search;
    juce::TextButton addFolderButton { "Add a folder..." }, removeFolderButton { "Remove this folder" }, stopButton { "Stop" }, addButton { "Add to project" };
    juce::ToggleButton autoPreview { "Play on click" };
    juce::Label info;
};

class SampleBrowserWindow : public juce::DocumentWindow
{
public:
    SampleBrowserWindow (SampleBrowserPanel* panel, std::function<void()> onClosed)
        : juce::DocumentWindow ("Sample browser", Theme::background, juce::DocumentWindow::closeButton), closed (std::move (onClosed))
    {
        setUsingNativeTitleBar (true);
        setContentOwned (panel, true);
        setResizable (true, false);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
    }

    void closeButtonPressed() override { if (closed) closed(); }

private:
    std::function<void()> closed;
};

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "PluginCrash.h"

// Problem reports. Nothing is ever sent by the program itself: a report is plain text that is shown first, saved in
// %APPDATA%\UFODAW\reports, and the user chooses to open a prefilled GitHub issue (which they still have to submit) or to copy the text.
// It holds the version, the Windows version, the processor, and (after a crash) the file that crashed, with the user's folder name removed.
// It holds no project data, audio, or file names of the user's projects.
namespace CrashReport
{
    inline juce::String hidePersonalPaths (juce::String text)
    {
        const auto home = juce::File::getSpecialLocation (juce::File::userHomeDirectory).getFullPathName();
        text = text.replace (home, "%USERPROFILE%", true);
        return text.replace (juce::SystemStats::getLogonName(), "<user>", true);
    }

    inline juce::String build (const juce::String& what, const juce::String& crashedModule, const juce::String& pluginName)
    {
        juce::String r;
        r << "UFODAW " << JUCE_APPLICATION_VERSION_STRING << "\n"
          << "Windows: " << juce::SystemStats::getOperatingSystemName() << "\n"
          << "CPU: " << juce::SystemStats::getCpuModel() << ", " << juce::SystemStats::getNumCpus() << " logical cores, "
          << juce::SystemStats::getMemorySizeInMegabytes() << " MB RAM\n"
          << "Time: " << juce::Time::getCurrentTime().toISO8601 (true) << "\n";
        if (crashedModule.isNotEmpty())
            r << "Crashed in: " << hidePersonalPaths (crashedModule) << "\n";
        if (pluginName.isNotEmpty())
            r << "Plugin: " << pluginName << " (now blocked)\n";
        if (what.trim().isNotEmpty())
            r << "\nWhat happened:\n" << what.trim() << "\n";
        return r;
    }

    inline juce::File saveCopy (const juce::String& report)
    {
        auto dir = PluginCrash::folder().getChildFile ("reports");
        dir.createDirectory();
        auto file = dir.getChildFile (juce::Time::getCurrentTime().formatted ("report-%Y%m%d-%H%M%S.txt"));
        file.replaceWithText (report);
        return file;
    }

    inline void openGitHubIssue (const juce::String& title, const juce::String& report)
    {
        auto body = report;
        if (body.length() > 3500)
            body = body.substring (0, 3500) + "\n...";
        juce::URL ("https://github.com/Thulehunter/ufodaw/issues/new")
            .withParameter ("title", title).withParameter ("body", body).launchInDefaultBrowser();
    }

    // The report is shown in the dialog; "Report on GitHub" opens a prefilled issue, "Copy" puts the text on the clipboard.
    inline void show (const juce::String& dialogTitle, const juce::String& intro, const juce::String& issueTitle, const juce::String& report)
    {
        saveCopy (report);
        auto options = juce::MessageBoxOptions().withIconType (juce::MessageBoxIconType::WarningIcon).withTitle (dialogTitle)
                           .withMessage (intro + "\n\nThis is the report. Nothing is sent by the program; you decide:\n\n" + report)
                           .withButton ("Report on GitHub").withButton ("Copy report").withButton ("Close");
        juce::AlertWindow::showAsync (options, [issueTitle, report] (int result)
        {
            if (result == 1)
                openGitHubIssue (issueTitle, report);
            else if (result == 2)
                juce::SystemClipboard::copyTextToClipboard (report);
        });
    }

    // File menu > Report a problem...: the user describes it, then the same choices.
    inline void showProblemDialog()
    {
        auto* w = new juce::AlertWindow ("Report a problem",
                                         "Describe what went wrong (what you did, what you expected). The next step shows the full report before anything leaves "
                                         "your computer.", juce::MessageBoxIconType::QuestionIcon);
        w->addTextEditor ("what", "", "What happened?");
        w->addButton ("Continue", 1, juce::KeyPress (juce::KeyPress::returnKey));
        w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        w->enterModalState (true, juce::ModalCallbackFunction::create ([w] (int result)
        {
            const auto text = w->getTextEditorContents ("what");
            delete w;
            if (result == 1)
                show ("Problem report", "Thank you.", "Problem: " + text.upToFirstOccurrenceOf ("\n", false, false).substring (0, 70),
                      build (text, {}, {}));
        }), false);
    }
}

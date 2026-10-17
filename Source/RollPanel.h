#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "Engine.h"

// The editor panel below the arrangement. The piano roll and the drum step sequencer both are one,
// so the main window can treat whichever is showing the same way.
class RollPanel : public juce::Component
{
public:
    virtual void showClip (Track*, int clipId) = 0;
    virtual Track* getTrack() const = 0;
    virtual int getClipId() const = 0;
    virtual bool stillValid() const = 0;       // false once the clip or its track is gone
    virtual bool deleteSelectedNote() = 0;     // true if something was deleted
    virtual void repaintGrid() = 0;

    std::function<void()> onClose;
};

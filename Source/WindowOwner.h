#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>
#endif

// Tool windows (the drum rack, the samplers, the synth, the effects) belong to the main window: Windows keeps an owned window above its owner, so a
// click in the main window (to drag a sound from the sample browser onto a pad, for example) does not hide the tool windows behind it. They are not
// on top of other programs, and they go away with the main window when it is minimised.
namespace WindowOwner
{
    inline void keepAbove (juce::Component& tool, juce::Component& owner)
    {
       #if JUCE_WINDOWS
        auto* toolPeer = tool.getPeer();
        auto* ownerPeer = owner.getPeer();
        if (toolPeer != nullptr && ownerPeer != nullptr)
            SetWindowLongPtrW ((HWND) toolPeer->getNativeHandle(), GWLP_HWNDPARENT, (LONG_PTR) ownerPeer->getNativeHandle());
       #else
        juce::ignoreUnused (tool, owner);
       #endif
    }
}

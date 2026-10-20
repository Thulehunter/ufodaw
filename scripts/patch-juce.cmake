# Makes JUCE's Windows windows start with the software renderer (engine 0) instead of Direct2D (engine 1): Direct2D leaves windows white on some PCs.
# Run by FetchContent after JUCE is downloaded (it can be run again safely).
set(file "${JUCE_DIR}/modules/juce_gui_basics/native/juce_Windowing_windows.cpp")
file(READ "${file}" text)
string(REPLACE "(HWND) parentHWND, false, 1 };" "(HWND) parentHWND, false, 0 };" text "${text}")
file(WRITE "${file}" "${text}")

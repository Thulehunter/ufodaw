# Makes JUCE's Windows windows start with the software renderer (engine 0) instead of Direct2D (engine 1): Direct2D leaves windows white on some PCs.
# Run by FetchContent after JUCE is downloaded (it can be run again safely).
set(file "${JUCE_DIR}/modules/juce_gui_basics/native/juce_Windowing_windows.cpp")
file(READ "${file}" text)
string(REPLACE "(HWND) parentHWND, false, 1 };" "(HWND) parentHWND, false, 0 };" text "${text}")
file(WRITE "${file}" "${text}")

# When Windows cannot make the window's bitmap (CreateDIBSection fails: out of GDI resources), JUCE would write to a null pointer and the program
# crashed. A plain memory buffer takes its place then (the window is still drawn, with SetDIBitsToDevice).
string(REPLACE "if (format == Image::ARGB && clearImage)" "if (hBitmap == nullptr) bitmapData = (uint8*) calloc ((size_t) std::abs (h * lineStride) + 64, 1);\n        if (format == Image::ARGB && clearImage)" text "${text}")
file(WRITE "${file}" "${text}")
string(REPLACE "hBitmap = CreateDIBSection (hdc," "bitmapData = nullptr;\n        hBitmap = CreateDIBSection (hdc," text "${text}")
file(WRITE "${file}" "${text}")

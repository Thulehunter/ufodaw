#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

// MiniDAW's look: a warm dark brown base with red, orange and yellow accents.
// Change a colour here and it changes everywhere.
namespace Theme
{
    // surfaces, darkest to lightest
    inline const juce::Colour border      { 0xff100a08 };
    inline const juce::Colour laneBg      { 0xff17100d };
    inline const juce::Colour rowDark     { 0xff171110 };
    inline const juce::Colour gridBg      { 0xff1b1411 };
    inline const juce::Colour background  { 0xff1c1512 };
    inline const juce::Colour rowLight    { 0xff221915 };
    inline const juce::Colour keyBlack    { 0xff2a1e19 };
    inline const juce::Colour panel       { 0xff2b201c };
    inline const juce::Colour gridLine    { 0xff33251f };
    inline const juce::Colour panelRaised { 0xff3a2b25 };
    inline const juce::Colour gridLineStrong { 0xff4a362c };

    // buttons
    inline const juce::Colour buttonOff   { 0xff3b2c26 };
    inline const juce::Colour buttonHover { 0xff4b362d };
    inline const juce::Colour buttonDown  { 0xff5e4337 };
    inline const juce::Colour buttonBase  { 0xff3a2a24 };

    // text
    inline const juce::Colour text        { 0xfff3e6dc };
    inline const juce::Colour textDim     { 0xffb5a095 };
    inline const juce::Colour textFaint   { 0xff8c7468 };
    inline const juce::Colour iconIdle    { 0xffe6d3c4 };
    inline const juce::Colour keyWhite    { 0xfff3e6dc };

    // accents
    inline const juce::Colour red         { 0xffc9302c };
    inline const juce::Colour redBright   { 0xffff5a47 };
    inline const juce::Colour redDim      { 0xff9c3b34 };
    inline const juce::Colour orange      { 0xfff28c28 };
    inline const juce::Colour orangeDim   { 0xffb8661c };
    inline const juce::Colour yellow      { 0xffffc933 };
    inline const juce::Colour yellowDim   { 0xffc99a1c };
    inline const juce::Colour playhead    { 0xffffd23f };

    // clips and notes
    inline const juce::Colour audioClip    { 0xffa8501c };
    inline const juce::Colour audioClipSel { 0xffd9702a };
    inline const juce::Colour audioWave    { 0xffffd9b8 };
    inline const juce::Colour midiClip     { 0xff9c2f35 };
    inline const juce::Colour midiClipSel  { 0xffd94452 };
    inline const juce::Colour drumClip     { 0xff9c7410 };
    inline const juce::Colour drumClipSel  { 0xffd9a21f };
    inline const juce::Colour midiNote     { 0xffffd1c8 };
    inline const juce::Colour noteSel      { 0xffffe08a };
    inline const juce::Colour noteOutline  { 0xff2a0f0c };

    // the colour that tells track types apart (header stripe, level of detail in lists)
    inline juce::Colour trackColour (bool isMidi, bool isDrums)
    {
        return isDrums ? yellow : isMidi ? redBright : orange;
    }
}

class WarmLookAndFeel : public juce::LookAndFeel_V4
{
public:
    WarmLookAndFeel();

    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour& backgroundColour,
                               bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;
    void drawToggleButton (juce::Graphics&, juce::ToggleButton&, bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;
    void drawLinearSlider (juce::Graphics&, int x, int y, int width, int height, float sliderPos, float minSliderPos,
                           float maxSliderPos, juce::Slider::SliderStyle, juce::Slider&) override;
    void drawComboBox (juce::Graphics&, int width, int height, bool isButtonDown, int buttonX, int buttonY,
                       int buttonW, int buttonH, juce::ComboBox&) override;
    void drawPopupMenuBackground (juce::Graphics&, int width, int height) override;
};

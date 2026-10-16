// Shared, themeable parts for plugin windows (v0.9.0): colours, sizes, the font and drawing helpers. Nothing here
// knows the plugin: any window may use it.
//
// Sizes are in dp. A window lays itself out in dp and is scaled as a whole (a component transform), so every size here
// follows the window's zoom. Text sizes are em sizes, as CSS px.
#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

namespace ui {

namespace colours {
inline const juce::Colour window{0xff1b1b1f};
inline const juce::Colour row{0xff26262b}, rowEdge{0xff303036};
inline const juce::Colour button{0xff34343a}, buttonEdge{0xff44444c};
inline const juce::Colour line{0xff34343a};
inline const juce::Colour text{0xffe8e8ea}, muted{0xff8a8a92}, heading{0xff9a9aa3};
inline const juce::Colour selection{0xff2f3a4f}, selectionEdge{0xff4d6fa8};
inline const juce::Colour info{0xff9db7e8};
inline const juce::Colour led{0xffff3b2f};
inline const juce::Colour dim{0x73000000};           // black at 45 %: over what the window covers
inline const juce::Colour shadow{0xa6000000};        // black at 65 %: the window's shadow
}  // namespace colours

namespace dp {
constexpr float inset = 14, radius = 12;             // the margin around a window (dp of what it covers); its corners
constexpr float shadowBlur = 28, shadowX = 5, shadowY = 12;   // the window's shadow (dp of what it covers)
constexpr float header = 64, footer = 84;
constexpr float padY = 18, padX = 20;                // the body
constexpr float tabText = 14, tabRadius = 8;
constexpr float fileRow = 42, fileRadius = 10, fileText = 13;
constexpr float cell = 29, cellText = 13;            // a preset in a list
constexpr float bankRow = 72;
constexpr float button = 34, buttonRadius = 8, buttonText = 14;
constexpr float headingText = 12;                    // column headings
constexpr float gap = 16;                            // between columns
}  // namespace dp

struct Theme {
    juce::Typeface::Ptr typeface;                    // the skin's UI font; null: JUCE's default sans
    juce::Colour accent{0xff8a3a9c}, accentEdge{0xffa64dbb};
    // The font at an em size of `px` dp. Bold only with the default sans (a skin's font is used as it is).
    juce::Font font(float px, bool bold = false) const;
    // A typeface from a font file's data (TrueType / OpenType); null when the file is missing or is not a font.
    static juce::Typeface::Ptr loadTypeface(const juce::File& file);
};

// One line of text: what does not fit ends in "…"; never wraps.
juce::String ellipsize(const juce::String& text, const juce::Font& font, float width);
float textWidth(const juce::Font& font, const juce::String& text);
void drawText(juce::Graphics& g, const juce::String& text, const juce::Font& font, juce::Rectangle<float> area, juce::Colour colour,
              juce::Justification just = juce::Justification::centredLeft);

// Explanations that must be read whole: as many lines as `width` needs (names and labels use drawText's "…").
float wrappedHeight(const juce::String& text, const juce::Font& font, float width);
void drawWrapped(juce::Graphics& g, const juce::String& text, const juce::Font& font, juce::Rectangle<float> area, juce::Colour colour);

// Symbols drawn as paths, never as font glyphs: the arrow (▸), the cross (✕), the tick (✓), a dot, an LED.
juce::Path arrowPath(juce::Rectangle<float> box);    // a right-pointing triangle centred in box
void drawArrow(juce::Graphics& g, juce::Rectangle<float> box, juce::Colour colour);
void drawCross(juce::Graphics& g, juce::Rectangle<float> box, juce::Colour colour, float thickness);
void drawTick(juce::Graphics& g, juce::Rectangle<float> box, juce::Colour colour, float thickness);
void drawDot(juce::Graphics& g, juce::Point<float> centre, float radius, juce::Colour colour);
void drawLed(juce::Graphics& g, juce::Point<float> centre, float radius, bool lit);   // lit: with its glow

}  // namespace ui

// The custom skin elements: <knob cc>, <pbutton raw led>, <led code|source>, <vfdigit pos>, and for the editor unit
// <vfdrow row> and <legend action value>.
#pragma once
#include <RmlUi/Core/Element.h>
#include <array>
#include <string>
#include <vector>

namespace skin {

class SkinView;

void registerElements();          // once per RmlUi initialisation

// Shows sprite <spriteprefix><NNN> (three digits) through the element's decorator.
class SpriteElement : public Rml::Element {
public:
    explicit SpriteElement(const Rml::String& tag) : Rml::Element(tag) {}
    const std::string& shownSprite() const { return shown_; }   // "image(<sprite>)" or empty
protected:
    void showSprite(int index);
    std::string prefix();
private:
    std::string shown_;
};

// Value = raw 0-1023; frame = round(raw x (frames-1) / 1023). Sends Bx cc vv only when the user (or Lua) moves it.
class KnobElement : public SpriteElement {
public:
    explicit KnobElement(const Rml::String& tag);
    ~KnobElement() override;
    int cc() const { return cc_; }
    int raw() const { return raw_; }
    // Moves the knob to raw: stores its position and sends the value (unless unchanged).
    void move(int raw);
    // Shows raw and stores it, without sending (the value was sent already, e.g. by panel.setKnob).
    void show(int raw);
    void wheel(float notches, bool fine);
protected:
    void OnUpdate() override;
    void OnAttributeChange(const Rml::ElementAttributes& changed) override;
private:
    std::string key() const;
    void redraw();
    SkinView* view_;
    int cc_ = -1;
    int raw_ = 0;
    bool known_ = false;
};

// Left button: 81 raw while down, 80 raw on release (class pressed). Alt-click: latching hold (class held).
class PButtonElement : public Rml::Element {
public:
    explicit PButtonElement(const Rml::String& tag);
    ~PButtonElement() override;
    int raw() const { return raw_; }
    int ledCode() const { return led_; }
    void mouseDown(bool alt);
    void mouseRelease();             // the left button went up after mouseDown
protected:
    void OnAttributeChange(const Rml::ElementAttributes& changed) override;
private:
    SkinView* view_;
    int raw_ = -1, led_ = -1;
    bool down_ = false;
};

// Class lit per the LED logic; source="audioclip" is the Input Clip LED.
class LedElement : public Rml::Element {
public:
    explicit LedElement(const Rml::String& tag);
    ~LedElement() override;
    int code() const { return code_; }
    bool audioClip() const { return audioClip_; }
protected:
    void OnAttributeChange(const Rml::ElementAttributes& changed) override;
private:
    SkinView* view_;
    int code_ = -1;
    bool audioClip_ = false;
};

// One display digit: pos 0 = leftmost (panel message 96) ... 3 = rightmost (93). Sprite = segment byte & 0x7F.
class VfDigitElement : public SpriteElement {
public:
    explicit VfDigitElement(const Rml::String& tag);
    ~VfDigitElement() override;
    int pos() const { return pos_; }
    void setByte(uint8_t b) { byte_ = b & 0x7F; showSprite(byte_); }
protected:
    void OnUpdate() override { showSprite(byte_); }
    void OnAttributeChange(const Rml::ElementAttributes& changed) override;
private:
    SkinView* view_;
    int pos_ = 0;
    int byte_ = 0;
};

// The editor unit's glass: one row of 50 character cells (row 0-3). Each cell is a <span class="cell"> holding
// <span class="chr"> (sprite <spriteprefix><NNN>, NNN = the character's ASCII code; no sprite for a space),
// <span class="ul"> (the underline) and <span class="dp"> (the decimal point), both with class "lit" when on. A cell
// showing a space has class "ghost"; the focused value's cells have class "focus", and "blink" during the off half
// of the 2.5 Hz blink. Positions and art come from the skin's RCSS.
class VfdRowElement : public Rml::Element {
public:
    explicit VfdRowElement(const Rml::String& tag);
    ~VfdRowElement() override;
    int row() const { return row_; }
    // Shows one row: 50 characters, underlines, decimal points, the focused cells [focusCol, focusCol + focusLen).
    void setRow(const std::string& text, const std::array<bool, 50>& underline, const std::array<bool, 50>& dot,
                int focusCol, int focusLen, bool blinkOff);
    char shownChar(int col) const { return col >= 0 && col < 50 ? shown_[size_t(col)] : 0; }
    int cellAt(Rml::Element* e) const;               // the cell (0-49) holding `e`, -1 if none
protected:
    void OnAttributeChange(const Rml::ElementAttributes& changed) override;
private:
    void ensureCells();
    SkinView* view_;
    int row_ = 0;
    std::vector<Rml::Element*> cells_, chars_, uls_, dps_;
    std::array<char, 50> shown_{};
};

// A legend on the editor glass: action (area, section, target, page, page_prev, page_next, toggle_follow, indicator,
// label, library) and value (its text; library: LIBRARY, SAVE, IMPORT, EXPORT, FOLDER). The library legends may also be
// in the main document (empty legends: click areas over the art's own lettering); lit and avail apply there too. Class "lit" when active, "avail" when clicking it leads somewhere; hover is the
// skin's :hover. A click sends the action to the editor.
class LegendElement : public Rml::Element {
public:
    explicit LegendElement(const Rml::String& tag);
    ~LegendElement() override;
    const std::string& action() const { return action_; }
    const std::string& value() const { return value_; }
    bool clickable() const { return action_ != "indicator" && action_ != "label" && !action_.empty(); }
protected:
    void OnAttributeChange(const Rml::ElementAttributes& changed) override;
private:
    SkinView* view_;
    std::string action_, value_;
};

int parseInt(const std::string& s, int fallback);   // decimal or 0x hex

}  // namespace skin

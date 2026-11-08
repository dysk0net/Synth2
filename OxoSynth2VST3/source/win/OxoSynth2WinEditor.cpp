#include "OxoSynth2WinEditor.h"

#include "../OxoSynth2Controller.h"
#include "EditorCore/Storage.hpp"
#include "EngineCore/InitPatch.hpp"
#include "EngineCore/ParamFormat.hpp"
#include "EngineCore/Params.hpp"
#include "EngineCore/PatchLibrary.hpp"

#include "vstgui/lib/cdrawcontext.h"
#include "vstgui/lib/cframe.h"
#include "vstgui/lib/cfont.h"
#include "vstgui/lib/cgradient.h"
#include "vstgui/lib/cgraphicspath.h"
#include "vstgui/lib/coffscreencontext.h"
#include "vstgui/lib/controls/coptionmenu.h"
#include "vstgui/lib/cview.h"
#include "vstgui/lib/events.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <string>
#include <vector>

using namespace VSTGUI;

namespace oxo {

namespace {

using namespace s2;

// The Mac panel's parameter numbering (Shared/SynthPanelView.swift) is the
// engine's: the layout below uses those numbers directly.
static_assert(kOsc1Shape == 0 && kOsc2Pitch == 2 && kFilterType == 14 && kPlayMode == 38 && kLfo1Dest == 41 &&
                  kEffectType == 78 && kCtrlSrc1 == 86 && kCtrlAssign2 == 89 && kPan == 90 && kOscPhase == 91 &&
                  kPolyphony == 94 && kSubGain == 95 && kSubShape == 96 && kSubOctave == 97 && kDelayTone == 98,
              "parameter numbering changed: re-check the Windows panel layout");


// ---- user settings (colours, size), remembered ---------------------------------
// One setting for every instance, in %APPDATA%\Oxo Music\Oxo Synth2:
//   background.txt  "RRGGBB"  panel background  (BG Color)
//   accent.txt      "RRGGBB"  knob arc / dots / pointer  (Color)
//   scale.txt       "1.25"    window zoom
// The section panels are derived from the background (a little lighter or
// darker), so any background keeps the strips readable.

constexpr uint32_t kDefaultBackground = 0x22252A, kDefaultAccent = 0xFF4040;
constexpr double kMinScale = 0.7, kMaxScale = 2.5;
uint32_t gBackground = kDefaultBackground;
uint32_t gAccent = kDefaultAccent;
double gScale = 1.0;

std::filesystem::path SettingsFile(const char *name) { return storage::Root() / name; }

bool ReadText(const char *name, std::string &out) {
    std::ifstream in(SettingsFile(name));
    return bool(in >> out);
}
void WriteText(const char *name, const std::string &text) {
    std::error_code ec;
    std::filesystem::create_directories(SettingsFile(name).parent_path(), ec);
    std::ofstream(SettingsFile(name), std::ios::trunc) << text << "\n";
}

void LoadHex(const char *name, uint32_t &value) {
    std::string hex;
    if (!ReadText(name, hex) || hex.size() != 6) return;
    char *end = nullptr;
    const unsigned long v = std::strtoul(hex.c_str(), &end, 16);
    if (end && *end == '\0') value = uint32_t(v);
}
void SaveHex(const char *name, uint32_t value) {
    char b[16];
    std::snprintf(b, sizeof b, "%06X", unsigned(value & 0xFFFFFF));
    WriteText(name, b);
}

void LoadSettings() {
    LoadHex("background.txt", gBackground);
    LoadHex("accent.txt", gAccent);
    std::string text;
    if (ReadText("scale.txt", text)) {
        const double v = std::atof(text.c_str());
        if (v >= kMinScale && v <= kMaxScale) gScale = v;
    }
}
void SaveBackground() { SaveHex("background.txt", gBackground); }
void SaveAccent() { SaveHex("accent.txt", gAccent); }
void SaveScale() {
    char b[32];
    std::snprintf(b, sizeof b, "%.3f", gScale);
    WriteText("scale.txt", b);
}

CColor AccentColor() { return CColor(uint8_t(gAccent >> 16), uint8_t(gAccent >> 8), uint8_t(gAccent), 255); }


// ---- look ---------------------------------------------------------------------
// Fixed colours are Synth1's (Shared/SynthTheme.swift "Synth2" theme); the
// panel colour (BG Color) and the knob dial / ring colour (Color) are the
// user's. Section edges / headers / label ink derive from the panel colour.

const CColor kKnobBody{23, 23, 23, 255};
const CColor kKnobHi{44, 44, 44, 255};
const CColor kButton{54, 54, 48, 255};
const CColor kButtonText{237, 237, 237, 255};
const CColor kLedOn{92, 242, 102, 255};
const CColor kLedOff{28, 73, 31, 255};
const CColor kRadioOn{255, 51, 26, 255};
const CColor kRadioOff{107, 21, 11, 255};
const CColor kDisplayText{115, 242, 255, 255};
const CColor kLcdTop{18, 61, 64, 255};
const CColor kLcdBottom{5, 23, 26, 255};
const CColor kLcdText{224, 255, 255, 255};
const CColor kRed{173, 15, 13, 255};
const CColor kBlack{0, 0, 0, 255};
const CColor kHeaderText{230, 224, 184, 255};

CColor Shade(uint32_t rgb, int delta) {
    const auto ch = [&](int shift) { return uint8_t(std::clamp(int((rgb >> shift) & 0xFF) + delta, 0, 255)); };
    return CColor(ch(16), ch(8), ch(0), 255);
}
bool BackgroundIsLight() {
    const int r = int((gBackground >> 16) & 0xFF), g = int((gBackground >> 8) & 0xFF), b = int(gBackground & 0xFF);
    return (r * 299 + g * 587 + b * 114) / 1000 > 140;
}
CColor BgColor() { return Shade(gBackground, 0); }
CColor Ink() { return BackgroundIsLight() ? CColor(26, 26, 15, 255) : CColor(228, 231, 236, 255); }
CColor HeaderFill() { return BackgroundIsLight() ? CColor(59, 59, 56, 255) : Shade(gBackground, 30); }
CColor EdgeColor() { return BackgroundIsLight() ? Shade(gBackground, -100) : Shade(gBackground, 48); }
CColor Faint(const CColor &c, double a) { return CColor(c.red, c.green, c.blue, uint8_t(255 * a)); }

// Fonts (never freed: they outlive the DLL's VSTGUI shutdown order).
CFontRef Font(const char *face, double size, int style) {
    static std::map<std::string, CFontDesc *> cache;
    const std::string key = std::string(face) + "/" + std::to_string(size) + "/" + std::to_string(style);
    CFontDesc *&f = cache[key];
    if (!f) f = new CFontDesc(face, size, style);
    return f;
}
CFontRef LabelFont() { return Font("Segoe UI", 10, 0); }
CFontRef ButtonFont() { return Font("Segoe UI", 10, kBoldFace); }
CFontRef MonoFont(double size, bool bold = true) { return Font("Consolas", size, bold ? kBoldFace : 0); }

// Text widths for layout (valid while an editor is open).
SharedPointer<COffscreenContext> gMeasure;
CCoord TextWidth(const std::string &s, CFontRef font) {
    if (!gMeasure) gMeasure = COffscreenContext::create(CPoint(8, 8), 1.0);
    if (gMeasure) {
        gMeasure->setFont(font);
        return gMeasure->getStringWidth(s.c_str());
    }
    return double(s.size()) * 5.5;
}

int DefaultValue(int id) {
    return id == kVolumeParamId ? kVolumeParam.def : kInitPatchValues[size_t(std::clamp(id, 0, int(kParamCount) - 1))];
}

CRect Around(double cx, double cy, double w, double h) { return CRect(cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2); }

// A rounded rectangle with a vertical gradient and a border.
void Face(CDrawContext *dc, const CRect &r, double radius, const CColor &top, const CColor &bottom, const CColor &border,
          double borderWidth = 1.0) {
    dc->setDrawMode(kAntiAliasing);
    if (auto path = owned(dc->createRoundRectGraphicsPath(r, radius))) {
        if (auto gradient = owned(CGradient::create(0.0, 1.0, top, bottom)))
            dc->fillLinearGradient(path, *gradient, CPoint(r.left, r.top), CPoint(r.left, r.bottom));
        dc->setFrameColor(border);
        dc->setLineWidth(borderWidth);
        dc->drawGraphicsPath(path, CDrawContext::kPathStroked);
    }
}
CColor Scaled(const CColor &c, double f) { return CColor(uint8_t(c.red * f), uint8_t(c.green * f), uint8_t(c.blue * f), 255); }
void ButtonFace(CDrawContext *dc, const CRect &r, const CColor &fill = kButton) {
    Face(dc, r, 3, Scaled(fill, 1.12), Scaled(fill, 0.9), Faint(kBlack, 0.75));
}
void Text(CDrawContext *dc, const std::string &s, const CRect &r, CFontRef f, const CColor &c, CHoriTxtAlign a = kCenterText) {
    dc->setFont(f);
    dc->setFontColor(c);
    dc->drawString(s.c_str(), r, a, true);
}

// ---- controls -----------------------------------------------------------------

struct MenuGroup {
    std::string title; // "" = items at the menu's top level
    std::vector<std::pair<int, std::string>> items;
};

// Everything a control needs from the panel.
struct Env {
    OxoSynth2Controller *c = nullptr;
    std::function<void(int)> readout; // show "name: value" on the LCD
    // Pop up a menu under `where` (panel coordinates); picking calls back with the item's value.
    std::function<void(const CPoint &, std::vector<MenuGroup>, std::function<void(int)>)> popup;
};

class ParamView : public CView {
public:
    ParamView(const CRect &r, Env &env, int id) : CView(r), env_(env), id_(id) {
        if (id >= 0) {
            const ParamInfo &p = Info(id);
            min_ = p.min;
            max_ = p.max;
            value_ = env.c->NativeValue(id);
        }
    }
    // Pulled from the controller (host automation, patch loads).
    void Sync() {
        if (id_ < 0) return;
        const int32_t v = env_.c->NativeValue(id_);
        if (v != value_) {
            value_ = v;
            invalid();
        }
    }

protected:
    void Set(int v) {
        v = std::clamp(v, min_, max_);
        if (v == value_) return;
        value_ = v;
        env_.c->PerformEdit(id_, v);
        env_.readout(id_);
        invalid();
    }
    void Edit(const std::function<void()> &fn) {
        env_.c->BeginEdit(id_);
        fn();
        env_.c->EndEdit(id_);
    }
    // Step through a choice parameter, wrapping.
    void Cycle(int dir) {
        int v = value_ + dir;
        if (v > max_) v = min_;
        if (v < min_) v = max_;
        Edit([&] { Set(v); });
    }
    std::string ValueText() const {
        const int sync = TempoSyncSwitch(id_);
        return FormatParam(id_, value_, sync >= 0 && env_.c->NativeValue(sync), env_.c->NativeValue(kEffectType));
    }
    CPoint Local(const CPoint &parentPos) const { return CPoint(parentPos.x - getViewSize().left, parentPos.y - getViewSize().top); }

    Env &env_;
    int id_;
    int min_ = 0, max_ = 1, value_ = 0;
};

// ---- knob ---------------------------------------------------------------------
// Synth1's: a dark body, a dial line (the user's Color) and an 11-dot ring over
// the -140..+140 degree travel that lights up to the value (the same colour).

class Knob : public ParamView {
public:
    Knob(double cx, double cy, Env &env, int id, std::string label, double size = 24)
        : ParamView(CRect(cx - 22, cy - std::ceil(size * 0.65) - 2, cx + 22, cy + size / 2 + 14), env, id), label_(std::move(label)), size_(size) {
        cy_ = std::ceil(size * 0.65) + 2;
    }

    void draw(CDrawContext *dc) override {
        const CRect v = getViewSize();
        const double cx = v.left + v.getWidth() / 2, cy = v.top + cy_, r = size_ / 2;
        const double frac = max_ > min_ ? double(value_ - min_) / double(max_ - min_) : 0.0;
        dc->setDrawMode(kAntiAliasing);
        const CColor accent = AccentColor();
        for (int i = 0; i < 11; ++i) {
            const double a = (-140.0 + 28.0 * i) * M_PI / 180.0, rr = size_ * 0.65 - 1.5;
            const bool lit = value_ != min_ && double(i) / 10.0 <= frac + 1e-6;
            dc->setFillColor(lit ? accent : Faint(Ink(), 0.3));
            const double px = cx + std::sin(a) * rr, py = cy - std::cos(a) * rr;
            dc->drawEllipse(CRect(px - 1.0, py - 1.0, px + 1.0, py + 1.0), kDrawFilled);
        }
        dc->setFillColor(Faint(kBlack, 0.3));
        dc->drawEllipse(CRect(cx - r + 1, cy - r + 1, cx + r + 1, cy + r + 1), kDrawFilled); // soft shadow
        dc->setFillColor(kKnobBody);
        dc->setFrameColor(Faint(EdgeColor(), 0.6));
        dc->setLineWidth(1.4);
        dc->drawEllipse(CRect(cx - r, cy - r, cx + r, cy + r), kDrawFilledAndStroked);
        dc->setFillColor(kKnobHi);
        dc->drawEllipse(CRect(cx - r * 0.7, cy - r * 0.75, cx - r * 0.05, cy - r * 0.2), kDrawFilled);
        const double a = (-140.0 + 280.0 * frac) * M_PI / 180.0;
        dc->setFrameColor(accent);
        dc->setLineWidth(2.0);
        dc->drawLine(CPoint(cx + std::sin(a) * r * 0.12, cy - std::cos(a) * r * 0.12),
                     CPoint(cx + std::sin(a) * r * 0.74, cy - std::cos(a) * r * 0.74));
        if (!label_.empty()) Text(dc, label_, CRect(v.left, cy + r + 1, v.right, cy + r + 13), LabelFont(), Ink());
        setDirty(false);
    }

    void onMouseDownEvent(MouseDownEvent &e) override {
        if (!e.buttonState.isLeft()) return;
        if (e.clickCount >= 2) {
            Edit([&] { Set(DefaultValue(id_)); });
            e.consumed = true;
            return;
        }
        dragging_ = true;
        startY_ = e.mousePosition.y;
        startValue_ = value_;
        env_.c->BeginEdit(id_);
        env_.readout(id_);
        e.consumed = true;
    }
    void onMouseMoveEvent(MouseMoveEvent &e) override {
        if (!dragging_) return;
        const double px = e.modifiers.has(ModifierKey::Shift) ? 900.0 : 180.0; // pixels for the full range
        Set(startValue_ + int(std::lround((startY_ - e.mousePosition.y) * double(max_ - min_) / px)));
        e.consumed = true;
    }
    void onMouseUpEvent(MouseUpEvent &e) override {
        Finish();
        e.consumed = true;
    }
    void onMouseCancelEvent(MouseCancelEvent &) override { Finish(); }
    void onMouseWheelEvent(MouseWheelEvent &e) override {
        if (e.deltaY == 0) return;
        const int step = e.modifiers.has(ModifierKey::Shift) ? 1 : std::max(1, (max_ - min_) / 40);
        Edit([&] { Set(value_ + (e.deltaY > 0 ? step : -step)); });
        e.consumed = true;
    }
    void onMouseEnterEvent(MouseEnterEvent &) override { env_.readout(id_); }

private:
    void Finish() {
        if (!dragging_) return;
        dragging_ = false;
        env_.c->EndEdit(id_);
    }
    std::string label_;
    double size_, cy_ = 0, startY_ = 0;
    int startValue_ = 0;
    bool dragging_ = false;
};

// ---- buttons ------------------------------------------------------------------

// Dark push button with a LED: a parameter switch.
class Toggle : public ParamView {
public:
    Toggle(double cx, double cy, double w, Env &env, int id, std::string title)
        : ParamView(Around(cx, cy, w, 17), env, id), title_(std::move(title)), textW_(TextWidth(title_, ButtonFont())) {}
    void draw(CDrawContext *dc) override {
        const CRect v = getViewSize();
        ButtonFace(dc, v);
        const double total = 5 + 3 + textW_, x = v.left + (v.getWidth() - total) / 2;
        const bool on = value_ != 0;
        if (on) {
            dc->setFillColor(Faint(AccentColor(), 0.35));
            dc->drawEllipse(CRect(x - 2, v.top + 6.5 - 2, x + 7, v.top + 11.5 + 2), kDrawFilled);
        }
        dc->setFillColor(on ? AccentColor() : Scaled(AccentColor(), 0.25));
        dc->drawRect(CRect(x, v.top + 6, x + 5, v.top + 11), kDrawFilled);
        Text(dc, title_, CRect(x + 8, v.top, v.right, v.bottom), ButtonFont(), kButtonText, kLeftText);
        setDirty(false);
    }
    void onMouseDownEvent(MouseDownEvent &e) override {
        if (!e.buttonState.isLeft()) return;
        Edit([&] { Set(value_ ? 0 : 1); });
        e.consumed = true;
    }
    void onMouseEnterEvent(MouseEnterEvent &) override { env_.readout(id_); }

private:
    std::string title_;
    double textW_;
};

// Dark push button stepping a choice parameter ("type", "dst", "range");
// Shift-click steps back. LEDs / a display next to it show the value.
class CycleButton : public ParamView {
public:
    CycleButton(double cx, double cy, double w, Env &env, int id, std::string title)
        : ParamView(Around(cx, cy, w, 17), env, id), title_(std::move(title)) {}
    void draw(CDrawContext *dc) override {
        ButtonFace(dc, getViewSize());
        Text(dc, title_, getViewSize(), ButtonFont(), kButtonText);
        setDirty(false);
    }
    void onMouseDownEvent(MouseDownEvent &e) override {
        if (!e.buttonState.isLeft()) return;
        Cycle(e.modifiers.has(ModifierKey::Shift) ? -1 : 1);
        e.consumed = true;
    }
    void onMouseWheelEvent(MouseWheelEvent &e) override {
        if (e.deltaY == 0) return;
        Cycle(e.deltaY > 0 ? 1 : -1);
        e.consumed = true;
    }
    void onMouseEnterEvent(MouseEnterEvent &) override { env_.readout(id_); }

private:
    std::string title_;
};

// Plain (non-parameter) push button.
class PushButton : public CView {
public:
    PushButton(const CRect &r, std::string text, std::function<void(PushButton &)> onClick, const CColor &fill = kButton, double fontSize = 11)
        : CView(r), text_(std::move(text)), onClick_(std::move(onClick)), fill_(fill), fontSize_(fontSize) {}
    void draw(CDrawContext *dc) override {
        ButtonFace(dc, getViewSize(), fill_);
        Text(dc, text_, getViewSize(), Font("Segoe UI", fontSize_, kBoldFace), kButtonText);
        setDirty(false);
    }
    void onMouseDownEvent(MouseDownEvent &e) override {
        if (!e.buttonState.isLeft()) return;
        onClick_(*this);
        e.consumed = true;
    }

private:
    std::string text_;
    std::function<void(PushButton &)> onClick_;
    CColor fill_;
    double fontSize_;
};

// Synth1's black number badge ("1", "2"); with a parameter it is the LFO's on / off switch.
class Badge : public ParamView {
public:
    Badge(double cx, double cy, Env &env, int id, std::string text) : ParamView(Around(cx, cy, 16, 18), env, id), text_(std::move(text)) {}
    void draw(CDrawContext *dc) override {
        const bool on = id_ < 0 || value_ != 0;
        Face(dc, getViewSize(), 2, on ? kBlack : CColor(56, 56, 56, 255), on ? kBlack : CColor(56, 56, 56, 255), CColor(128, 128, 128, 255));
        const CColor c = on ? (id_ < 0 ? CColor(255, 255, 255, 255) : AccentColor()) : CColor(115, 115, 115, 255);
        Text(dc, text_, getViewSize(), Font("Segoe UI", 12, kBoldFace), c);
        setDirty(false);
    }
    void onMouseDownEvent(MouseDownEvent &e) override {
        if (id_ < 0 || !e.buttonState.isLeft()) return;
        Edit([&] { Set(value_ ? 0 : 1); });
        e.consumed = true;
    }

private:
    std::string text_;
};

// ---- red LED radio lists --------------------------------------------------------

class LedRadio : public ParamView {
public:
    struct Option {
        int value;
        std::string label;
    };
    LedRadio(double cx, double cy, Env &env, int id, std::vector<Option> options, bool horizontal = false, bool ledTrailing = false,
             bool showLabels = true, double rowHeight = 13)
        : ParamView(CRect(0, 0, 1, 1), env, id), opts_(std::move(options)), horizontal_(horizontal), trailing_(ledTrailing), showLabels_(showLabels) {
        std::vector<double> w;
        for (const Option &o : opts_) w.push_back(7 + (showLabels_ ? 3 + TextWidth(o.label, LabelFont()) : 0));
        double W = 0, H = 0;
        if (horizontal_) {
            for (size_t i = 0; i < w.size(); ++i) W += w[i] + (i ? 8 : 0);
            H = rowHeight;
        } else {
            for (double x : w) W = std::max(W, x);
            H = rowHeight * double(opts_.size());
        }
        double x = 0;
        for (size_t i = 0; i < opts_.size(); ++i) {
            if (horizontal_) {
                rows_.push_back(CRect(x, 0, x + w[i], H));
                x += w[i] + 8;
            } else {
                rows_.push_back(CRect(0, double(i) * rowHeight, W, double(i + 1) * rowHeight));
            }
        }
        setViewSize(Around(cx, cy, W, H));
        setMouseableArea(getViewSize());
    }
    void draw(CDrawContext *dc) override {
        const CRect v = getViewSize();
        dc->setDrawMode(kAntiAliasing);
        for (size_t i = 0; i < opts_.size(); ++i) {
            const CRect r(rows_[i].left + v.left, rows_[i].top + v.top, rows_[i].right + v.left, rows_[i].bottom + v.top);
            const bool on = value_ == opts_[i].value;
            const double ledX = trailing_ ? r.right - 7 : r.left, midY = r.top + r.getHeight() / 2;
            if (on) {
                dc->setFillColor(Faint(kRadioOn, 0.35));
                dc->drawEllipse(CRect(ledX - 2.5, midY - 6, ledX + 9.5, midY + 6), kDrawFilled);
            }
            dc->setFillColor(on ? kRadioOn : kRadioOff);
            dc->setFrameColor(Faint(kBlack, 0.5));
            dc->setLineWidth(0.5);
            dc->drawEllipse(CRect(ledX, midY - 3.5, ledX + 7, midY + 3.5), kDrawFilledAndStroked);
            if (showLabels_)
                Text(dc, opts_[i].label, trailing_ ? CRect(r.left, r.top, r.right - 10, r.bottom) : CRect(r.left + 10, r.top, r.right, r.bottom),
                     LabelFont(), Ink(), trailing_ ? kRightText : kLeftText);
        }
        setDirty(false);
    }
    void onMouseDownEvent(MouseDownEvent &e) override {
        if (!e.buttonState.isLeft()) return;
        const CPoint p = Local(e.mousePosition);
        for (size_t i = 0; i < opts_.size(); ++i)
            if (rows_[i].pointInside(p)) {
                Edit([&] { Set(opts_[i].value); });
                break;
            }
        e.consumed = true;
    }
    void onMouseWheelEvent(MouseWheelEvent &e) override {
        if (e.deltaY == 0) return;
        Cycle(e.deltaY > 0 ? 1 : -1);
        e.consumed = true;
    }

private:
    std::vector<Option> opts_;
    std::vector<CRect> rows_; // local to the view
    bool horizontal_, trailing_, showLabels_;
};

// ---- waveform buttons -----------------------------------------------------------

enum class Wave { Sine, Saw, Pulse, Square, Tri, Noise, SampleHold, Smooth };

void DrawWave(CDrawContext *dc, Wave w, const CRect &box, const CColor &colour) {
    const CRect r(box.left + 4, box.top + 3.5, box.right - 4, box.bottom - 3.5);
    const double x0 = r.left, x1 = r.right, y0 = r.top, y1 = r.bottom, W = r.getWidth(), H = r.getHeight();
    std::vector<CPoint> pts;
    switch (w) {
        case Wave::Sine:
            for (int i = 0; i <= 24; ++i) pts.emplace_back(x0 + W * i / 24.0, r.top + H / 2 - H / 2 * std::sin(2 * M_PI * i / 24.0));
            break;
        case Wave::Saw: pts = {{x0, y1}, {x0, y0}, {x1, y1}}; break;
        case Wave::Pulse:
        case Wave::Square: {
            const double duty = w == Wave::Pulse ? 0.3 : 0.5;
            pts = {{x0, y1}, {x0, y0}, {x0 + W * duty, y0}, {x0 + W * duty, y1}, {x1, y1}, {x1, y0}};
            break;
        }
        case Wave::Tri: pts = {{x0, y1}, {r.left + W / 2, y0}, {x1, y1}}; break;
        case Wave::Noise: {
            static const double ys[] = {0.5, 0.1, 0.8, 0.3, 0.95, 0.2, 0.6, 0.0, 0.7, 0.4, 0.9, 0.25, 0.55};
            for (int i = 0; i < 13; ++i) pts.emplace_back(x0 + W * i / 12.0, y0 + H * ys[i]);
            break;
        }
        case Wave::SampleHold:
        case Wave::Smooth: {
            static const double ys[] = {0.6, 0.1, 0.8, 0.35, 0.95};
            const double step = W / 5.0, soft = w == Wave::Smooth ? step * 0.35 : 0.0;
            pts.emplace_back(x0, y0 + H * ys[0]);
            for (int i = 0; i < 5; ++i) {
                const double edge = x0 + step * i, level = y0 + H * ys[i];
                if (i > 0) {
                    const double prev = y0 + H * ys[i - 1];
                    if (soft > 0) {
                        for (int k = 1; k <= 4; ++k) { // eased edge
                            const double t = k / 4.0, s = t * t * (3 - 2 * t);
                            pts.emplace_back(edge + soft * t, prev + (level - prev) * s);
                        }
                    } else {
                        pts.emplace_back(edge, prev);
                        pts.emplace_back(edge, level);
                    }
                }
                pts.emplace_back(x0 + step * (i + 1) - (i < 4 ? soft : 0), level);
            }
            break;
        }
    }
    CDrawContext::LineList lines;
    for (size_t i = 1; i < pts.size(); ++i) lines.push_back({pts[i - 1], pts[i]});
    dc->setDrawMode(kAntiAliasing);
    dc->setFrameColor(colour);
    dc->setLineWidth(1.3);
    dc->drawLines(lines);
}

void WaveCell(CDrawContext *dc, const CRect &r, Wave w, bool on) {
    if (on) {
        dc->setFillColor(Faint(AccentColor(), 0.3));
        dc->drawRect(CRect(r.left - 2, r.top - 2, r.right + 2, r.bottom + 2), kDrawFilled);
    }
    Face(dc, r, 2, on ? AccentColor() : Scaled(kButton, 1.1), on ? AccentColor() : Scaled(kButton, 0.9), Faint(kBlack, 0.7));
    DrawWave(dc, w, r, on ? kBlack : CColor(217, 217, 217, 255));
}

// Oscillator / sub shape grid (2 columns, row-major in value order).
class WaveRadio : public ParamView {
public:
    struct Shape {
        int value;
        Wave wave;
    };
    WaveRadio(double cx, double cy, Env &env, int id, std::vector<Shape> shapes)
        : ParamView(Around(cx, cy, 2 * 22 + 2, double((shapes.size() + 1) / 2) * 14 + double((shapes.size() + 1) / 2 - 1) * 2), env, id), shapes_(std::move(shapes)) {}
    void draw(CDrawContext *dc) override {
        const CRect v = getViewSize();
        for (size_t i = 0; i < shapes_.size(); ++i)
            WaveCell(dc, Cell(i, v), shapes_[i].wave, value_ == shapes_[i].value);
        setDirty(false);
    }
    void onMouseDownEvent(MouseDownEvent &e) override {
        if (!e.buttonState.isLeft()) return;
        for (size_t i = 0; i < shapes_.size(); ++i)
            if (Cell(i, getViewSize()).pointInside(e.mousePosition)) {
                Edit([&] { Set(shapes_[i].value); });
                break;
            }
        e.consumed = true;
    }
    void onMouseWheelEvent(MouseWheelEvent &e) override {
        if (e.deltaY == 0) return;
        Cycle(e.deltaY > 0 ? 1 : -1);
        e.consumed = true;
    }

private:
    static CRect Cell(size_t i, const CRect &v) {
        const double x = v.left + double(i % 2) * 24, y = v.top + double(i / 2) * 16;
        return CRect(x, y, x + 22, y + 14);
    }
    std::vector<Shape> shapes_;
};

// One button showing the current wave; a click steps to the next (LFO waves).
class WaveCycle : public ParamView {
public:
    WaveCycle(double cx, double cy, Env &env, int id, std::vector<Wave> waves) : ParamView(Around(cx, cy, 26, 17), env, id), waves_(std::move(waves)) {}
    void draw(CDrawContext *dc) override {
        const size_t i = size_t(std::clamp(value_ - min_, 0, int(waves_.size()) - 1));
        WaveCell(dc, getViewSize(), waves_[i], true);
        setDirty(false);
    }
    void onMouseDownEvent(MouseDownEvent &e) override {
        if (!e.buttonState.isLeft()) return;
        Cycle(e.modifiers.has(ModifierKey::Shift) ? -1 : 1);
        e.consumed = true;
    }
    void onMouseWheelEvent(MouseWheelEvent &e) override {
        if (e.deltaY == 0) return;
        Cycle(e.deltaY > 0 ? 1 : -1);
        e.consumed = true;
    }

private:
    std::vector<Wave> waves_;
};

// ---- displays ---------------------------------------------------------------------

// Small cyan-on-dark number / type display ("00" key shift, "16" poly, "ST"
// delay type): drag vertically or wheel to change, double-click = default;
// `tapCycles` = a click steps to the next value (type displays); `onTap` = a
// click calls it instead (effect type menu).
class ValueDisplay : public ParamView {
public:
    ValueDisplay(double cx, double cy, double w, Env &env, int id, bool tapCycles = false, bool twoDigits = false,
                 std::function<void(ValueDisplay &)> onTap = nullptr)
        : ParamView(Around(cx, cy, w, 17), env, id), tapCycles_(tapCycles), twoDigits_(twoDigits), onTap_(std::move(onTap)) {}
    void draw(CDrawContext *dc) override {
        const CRect v = getViewSize();
        Face(dc, v, 2, kLcdTop, kLcdBottom, Faint(kBlack, 0.7));
        char b[16];
        std::snprintf(b, sizeof b, "%02d", value_);
        Text(dc, twoDigits_ ? std::string(b) : ValueText(), v, MonoFont(11), kDisplayText);
        setDirty(false);
    }
    void onMouseDownEvent(MouseDownEvent &e) override {
        if (!e.buttonState.isLeft()) return;
        if (e.clickCount >= 2 && !tapCycles_ && !onTap_) {
            Edit([&] { Set(DefaultValue(id_)); });
            e.consumed = true;
            return;
        }
        dragging_ = true;
        moved_ = false;
        startY_ = e.mousePosition.y;
        startValue_ = value_;
        env_.c->BeginEdit(id_);
        env_.readout(id_);
        e.consumed = true;
    }
    void onMouseMoveEvent(MouseMoveEvent &e) override {
        if (!dragging_) return;
        const int steps = int(std::lround((startY_ - e.mousePosition.y) / 8.0));
        if (steps != 0) moved_ = true;
        if (moved_ && !onTap_) Set(startValue_ + steps);
        e.consumed = true;
    }
    void onMouseUpEvent(MouseUpEvent &e) override {
        Finish();
        e.consumed = true;
    }
    void onMouseCancelEvent(MouseCancelEvent &) override { Finish(); }
    void onMouseWheelEvent(MouseWheelEvent &e) override {
        if (e.deltaY == 0) return;
        Edit([&] { Set(value_ + (e.deltaY > 0 ? 1 : -1)); });
        e.consumed = true;
    }
    void onMouseEnterEvent(MouseEnterEvent &) override { env_.readout(id_); }
    int Value() const { return value_; }
    void Pick(int v) { Edit([&] { Set(v); }); }

private:
    void Finish() {
        if (!dragging_) return;
        dragging_ = false;
        env_.c->EndEdit(id_);
        if (!moved_) {
            if (onTap_) onTap_(*this);
            else if (tapCycles_) Cycle(1);
        }
    }
    bool tapCycles_, twoDigits_, dragging_ = false, moved_ = false;
    double startY_ = 0;
    int startValue_ = 0;
    std::function<void(ValueDisplay &)> onTap_;
};

// MIDI controller source / assign: a display-styled pop-up menu.
class MenuDisplay : public ParamView {
public:
    MenuDisplay(double cx, double cy, double w, Env &env, int id, std::function<std::string(int)> text,
                std::function<std::vector<MenuGroup>()> groups)
        : ParamView(Around(cx, cy, w, 16), env, id), text_(std::move(text)), groups_(std::move(groups)) {}
    void draw(CDrawContext *dc) override {
        const CRect v = getViewSize();
        Face(dc, v, 2, kLcdTop, kLcdBottom, Faint(kBlack, 0.7));
        Text(dc, text_(value_), CRect(v.left + 4, v.top, v.right - 2, v.bottom), MonoFont(10), kDisplayText, kLeftText);
        setDirty(false);
    }
    void onMouseDownEvent(MouseDownEvent &e) override {
        if (!e.buttonState.isLeft()) return;
        env_.popup(CPoint(getViewSize().left, getViewSize().bottom), groups_(), [this](int v) { Edit([&] { Set(v); }); });
        e.consumed = true;
    }

private:
    std::function<std::string(int)> text_;
    std::function<std::vector<MenuGroup>()> groups_;
};

// ---- static pieces --------------------------------------------------------------------

class Label : public CView {
public:
    Label(double cx, double cy, std::string text, CFontRef font = nullptr, bool dim = false)
        : CView(CRect(0, 0, 1, 1)), text_(std::move(text)), font_(font ? font : LabelFont()), dim_(dim) {
        setViewSize(Around(cx, cy, TextWidth(text_, font_) + 6, 14));
        setMouseEnabled(false);
    }
    void draw(CDrawContext *dc) override {
        Text(dc, text_, getViewSize(), font_, dim_ ? Faint(Ink(), 0.7) : Ink());
        setDirty(false);
    }

private:
    std::string text_;
    CFontRef font_;
    bool dim_;
};

// Dim separator line (section-header colour, mostly transparent).
class Divider : public CView {
public:
    Divider(double x1, double y1, double x2, double y2) : CView(CRect(x1, y1, std::max(x2, x1 + 1), std::max(y2, y1 + 1))) { setMouseEnabled(false); }
    void draw(CDrawContext *dc) override {
        dc->setFillColor(Faint(HeaderFill(), 0.28));
        dc->drawRect(getViewSize(), kDrawFilled);
        setDirty(false);
    }
};

// Framed section with Synth1's title strip (top 16 units).
class SectionBox : public CView {
public:
    SectionBox(const CRect &r, std::string title) : CView(r), title_(std::move(title)) { setMouseEnabled(false); }
    void draw(CDrawContext *dc) override {
        const CRect v = getViewSize();
        dc->setDrawMode(kAntiAliasing);
        dc->setFillColor(BgColor());
        dc->drawRect(v, kDrawFilled);
        dc->setFillColor(HeaderFill());
        dc->drawRect(CRect(v.left, v.top, v.right, v.top + 16), kDrawFilled);
        Text(dc, title_, CRect(v.left + 5, v.top, v.right, v.top + 16), Font("Segoe UI", 12, kBoldFace), kHeaderText, kLeftText);
        dc->setFrameColor(EdgeColor());
        dc->setLineWidth(1);
        dc->drawRect(CRect(v.left + 0.5, v.top + 0.5, v.right - 0.5, v.bottom - 0.5), kDrawStroked);
        setDirty(false);
    }

private:
    std::string title_;
};

class TitleBand : public CView {
public:
    explicit TitleBand(const CRect &r) : CView(r) { setMouseEnabled(false); }
    void draw(CDrawContext *dc) override {
        const CRect v = getViewSize();
        Text(dc, "Synth2", CRect(v.left + 8, v.top, v.left + 90, v.bottom), Font("Segoe UI", 20, kBoldFace), Ink(), kLeftText);
        Text(dc, "Virtual Analog", CRect(v.left + 92, v.top + 5, v.left + 220, v.bottom), Font("Segoe UI", 12, kItalicFace), Faint(Ink(), 0.7), kLeftText);
        const std::string badge = "a Synth1 recreation";
        const double w = TextWidth(badge, Font("Segoe UI", 12, kBoldFace)) + 20;
        const CRect box(v.right - 8 - w, v.top + 3, v.right - 8, v.top + 23);
        Text(dc, badge, box, Font("Segoe UI", 12, kBoldFace), AccentColor());
        setDirty(false);
    }
};

// The patch display (bottom bar): click = browse banks.
class Lcd : public CView {
public:
    Lcd(const CRect &r, std::function<void(Lcd &)> onClick) : CView(r), onClick_(std::move(onClick)) {}
    void Set(const std::string &l1, const std::string &l2, bool readout) {
        if (l1 == line1_ && l2 == line2_ && readout == readout_) return;
        line1_ = l1;
        line2_ = l2;
        readout_ = readout;
        invalid();
    }
    void draw(CDrawContext *dc) override {
        const CRect v = getViewSize();
        Face(dc, v, 4, kLcdTop, kLcdBottom, kBlack, 1.5);
        Text(dc, line1_, CRect(v.left + 10, v.top + 6, v.left + 338, v.top + 32), MonoFont(17), kLcdText, kLeftText);
        Text(dc, line2_, CRect(v.left + 10, v.top + 38, v.right - 10, v.top + 62), MonoFont(13, false),
             readout_ ? kDisplayText : Faint(kLcdText, 0.85), kLeftText);
        setDirty(false);
    }
    void onMouseDownEvent(MouseDownEvent &e) override {
        if (!e.buttonState.isLeft()) return;
        onClick_(*this);
        e.consumed = true;
    }

private:
    std::string line1_, line2_;
    bool readout_ = false;
    std::function<void(Lcd &)> onClick_;
};

// ---- MIDI ctrl routing (Synth1's src -> assign words) -------------------------------

const std::map<int, std::string> kCcNames = {
    {1, "mod wheel"}, {2, "breath"}, {4, "foot"}, {5, "porta time"}, {7, "volume"}, {8, "balance"}, {10, "pan"},
    {11, "expression"}, {64, "sustain"}, {65, "portamento"}, {66, "sostenuto"}, {67, "soft"}, {71, "resonance"},
    {72, "release"}, {73, "attack"}, {74, "cutoff"}, {91, "reverb"}, {93, "chorus"},
};

std::string CtrlSrcLabel(int v) {
    char b[48];
    switch (v >> 8) {
        case 0xB0: {
            const auto it = kCcNames.find(v & 0xFF);
            std::snprintf(b, sizeof b, "#%d:%s", v & 0xFF, it != kCcNames.end() ? it->second.c_str() : "cc");
            return b;
        }
        case 0xE0: return "pitch bend";
        case 0xD0: return "ch pressure";
        default:
            if (v <= 0) return "none";
            std::snprintf(b, sizeof b, "0x%04X", v);
            return b;
    }
}

std::vector<MenuGroup> CtrlSrcGroups() {
    std::vector<MenuGroup> g;
    g.push_back({"", {{0xE000, "pitch bend"}, {0xD000, "ch pressure"}}});
    for (int base = 0; base < 128; base += 32) {
        MenuGroup m;
        m.title = "CC " + std::to_string(base) + "-" + std::to_string(base + 31);
        for (int cc = base; cc < base + 32; ++cc) m.items.emplace_back(0xB000 | cc, CtrlSrcLabel(0xB000 | cc));
        g.push_back(std::move(m));
    }
    return g;
}

// Synth1's assign list, in its order (v1.12 manual): -1 = none.
const int kAssignTargets[] = {76, 45, 95, 2,  12, 13, 11, 5,  8,  91, 15, 16, 17, 18, 19, 20, 21, 23, 22, 25, 26, 27, 28, 29, 79, 80, 81,
                              61, 62, 63, 60, 90, 35, 36, 37, 83, 98, 52, 53, 54, 55, 56, 39, 75, 92, 84, 85, 34, 43, 44, 48, 49};

std::string AssignName(int v, int effectType) {
    if (v < 0) return "none";
    if (v < int(kParamCount)) return DisplayName(v, effectType);
    return std::to_string(v);
}

// Bottom-right resize grip. Dragging asks the editor for a new zoom; the host
// resizes its window and calls back onSize. Mouse positions arrive in the
// panel's own (unzoomed) coordinates, which shift as the window follows the
// mouse, so the target zoom is computed from the live zoom each move.
class Grip : public CView {
public:
    Grip(const CRect &r, CCoord baseW, CCoord baseH, std::function<double()> zoom, std::function<void(double)> request,
         std::function<void()> done)
        : CView(r), baseW_(baseW), baseH_(baseH), zoom_(std::move(zoom)), request_(std::move(request)), done_(std::move(done)) {}

    void draw(CDrawContext *dc) override {
        const CRect v = getViewSize();
        dc->setDrawMode(kAntiAliasing);
        dc->setFrameColor(Ink());
        dc->setLineWidth(1.5);
        for (int i = 1; i <= 3; ++i) {
            const CCoord d = i * 4.0;
            dc->drawLine(CPoint(v.right - 2, v.bottom - 2 - d), CPoint(v.right - 2 - d, v.bottom - 2));
        }
        setDirty(false);
    }
    void onMouseEnterEvent(MouseEnterEvent &) override {
        if (auto *f = getFrame()) f->setCursor(kCursorNWSESize);
    }
    void onMouseExitEvent(MouseExitEvent &) override {
        if (auto *f = getFrame(); f && !dragging_) f->setCursor(kCursorDefault);
    }
    void onMouseDownEvent(MouseDownEvent &e) override {
        if (!e.buttonState.isLeft()) return;
        dragging_ = true;
        grabX_ = baseW_ - e.mousePosition.x; // how far the click was from the corner
        grabY_ = baseH_ - e.mousePosition.y;
        e.consumed = true;
    }
    void onMouseMoveEvent(MouseMoveEvent &e) override {
        if (!dragging_) return;
        const double z = zoom_();
        const double zx = (e.mousePosition.x + grabX_) * z / baseW_, zy = (e.mousePosition.y + grabY_) * z / baseH_;
        request_((zx + zy) / 2.0);
        e.consumed = true;
    }
    void onMouseUpEvent(MouseUpEvent &e) override { Finish(); e.consumed = true; }
    void onMouseCancelEvent(MouseCancelEvent &) override { Finish(); }

private:
    void Finish() {
        if (!dragging_) return;
        dragging_ = false;
        if (auto *f = getFrame()) f->setCursor(kCursorDefault);
        done_();
    }
    CCoord baseW_, baseH_, grabX_ = 0, grabY_ = 0;
    bool dragging_ = false;
    std::function<double()> zoom_;
    std::function<void(double)> request_;
    std::function<void()> done_;
};

// ---- native file dialogs ------------------------------------------------------

std::string Utf8(const std::wstring &w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

bool PickFile(HWND owner, bool save, const std::string &suggested, std::filesystem::path &out) {
    wchar_t file[MAX_PATH * 2] = {};
    if (save) {
        const std::wstring name = std::filesystem::u8path(suggested).wstring();
        wcsncpy(file, name.c_str(), MAX_PATH - 1);
    }
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = L"Synth1 patch (*.sy1)\0*.sy1\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = DWORD(sizeof file / sizeof file[0]);
    ofn.lpstrDefExt = L"sy1";
    ofn.Flags = OFN_NOCHANGEDIR | OFN_EXPLORER | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    if (!(save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn))) return false;
    out = file;
    return true;
}

// Standard Windows colour picker (with the "Define Custom Colors" pane open).
bool PickColor(HWND owner, uint32_t &rgb) {
    static COLORREF custom[16] = {};
    CHOOSECOLORW cc{};
    cc.lStructSize = sizeof cc;
    cc.hwndOwner = owner;
    cc.rgbResult = RGB((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
    cc.lpCustColors = custom;
    cc.Flags = CC_RGBINIT | CC_FULLOPEN | CC_ANYCOLOR;
    if (!ChooseColorW(&cc)) return false;
    rgb = (uint32_t(GetRValue(cc.rgbResult)) << 16) | (uint32_t(GetGValue(cc.rgbResult)) << 8) | GetBValue(cc.rgbResult);
    return true;
}


} // namespace

// ---- the panel ----------------------------------------------------------------

struct OxoSynth2WinEditor::Panel {
    Env env;
    CFrame *frame = nullptr;
    HWND hwnd = nullptr;
    std::vector<ParamView *> params;
    Lcd *lcd = nullptr;
    int readoutId = -1;
    std::chrono::steady_clock::time_point readoutAt{};
    std::string notice;
    std::chrono::steady_clock::time_point noticeAt{};
    SharedPointer<COptionMenu> menu;
    std::vector<int> pickValues; // menu tag -> value of the pop-up in progress

    void Flash(const std::string &text) {
        notice = text;
        noticeAt = std::chrono::steady_clock::now();
    }

    static std::string Pad3(int n) {
        char b[16];
        std::snprintf(b, sizeof b, "%03d", n);
        return b;
    }

    // Value pick-list under `where` (frame coordinates).
    void Popup(const CPoint &where, const std::vector<MenuGroup> &groups, std::function<void(int)> pick) {
        menu = makeOwned<COptionMenu>();
        pickValues.clear();
        const auto add = [&](COptionMenu *m, const std::pair<int, std::string> &item) {
            if (CMenuItem *it = m->addEntry(UTF8String(item.second), -1)) it->setTag(int32_t(pickValues.size()));
            pickValues.push_back(item.first);
        };
        for (const MenuGroup &g : groups) {
            if (g.title.empty()) {
                for (const auto &item : g.items) add(menu, item);
                continue;
            }
            auto *sub = new COptionMenu();
            for (const auto &item : g.items) add(sub, item);
            menu->addEntry(sub, UTF8String(g.title));
            sub->forget();
        }
        menu->popup(frame, where, [this, pick](COptionMenu *m) {
            int32_t idx = -1;
            COptionMenu *sub = m->getLastItemMenu(idx);
            if (!sub || idx < 0) return;
            if (CMenuItem *it = sub->getEntry(idx)) {
                const int32_t tag = it->getTag();
                if (tag >= 0 && size_t(tag) < pickValues.size()) pick(pickValues[size_t(tag)]);
            }
        });
    }

    // Flattened list of non-empty library slots (bank, slot), in browser order.
    std::vector<std::pair<int, int>> Programs() {
        std::vector<std::pair<int, int>> list;
        env.c->Library().EnsureScanned();
        const auto &banks = env.c->Library().Library().Banks();
        for (size_t b = 0; b < banks.size(); ++b)
            for (size_t s = 0; s < banks[b].slots.size(); ++s)
                if (!banks[b].slots[s].Empty()) list.emplace_back(int(b), int(s));
        return list;
    }

    int CurrentBank() {
        const s2::LibraryProgram &p = env.c->Program();
        if (p.slot < 0) return -1;
        env.c->Library().EnsureScanned();
        return s2::FindProgramBank(env.c->Library().Library(), env.c->Library().Root(), p.bank);
    }

    void StepProgram(int dir) {
        const auto list = Programs();
        if (list.empty()) return;
        const int bank = CurrentBank();
        const int slot = bank >= 0 ? env.c->Program().slot : -1;
        int at = -1;
        for (size_t i = 0; i < list.size(); ++i)
            if (list[i].first == bank && list[i].second == slot) at = int(i);
        const int n = int(list.size());
        const int next = at < 0 ? (dir > 0 ? 0 : n - 1) : (at + dir + n) % n;
        env.c->LoadLibraryPatch(list[size_t(next)].first, list[size_t(next)].second);
    }

    void Browse(const CPoint &where) {
        env.c->Library().EnsureScanned();
        const auto &banks = env.c->Library().Library().Banks();
        menu = makeOwned<COptionMenu>();
        bool any = false;
        for (size_t b = 0; b < banks.size(); ++b) {
            auto *sub = new COptionMenu();
            int count = 0;
            for (size_t s = 0; s < banks[b].slots.size(); ++s) {
                if (banks[b].slots[s].Empty()) continue;
                const std::string title = Pad3(int(s) + 1) + "  " + banks[b].slots[s].name;
                if (CMenuItem *it = sub->addEntry(UTF8String(title), -1)) it->setTag(int32_t(b * 128 + s));
                ++count;
            }
            if (count == 0) {
                sub->forget();
                continue;
            }
            menu->addEntry(sub, UTF8String(banks[b].name));
            sub->forget();
            any = true;
        }
        if (!any) {
            Flash("Library is empty");
            return;
        }
        menu->popup(frame, where, [this](COptionMenu *m) {
            int32_t idx = -1;
            COptionMenu *sub = m->getLastItemMenu(idx);
            if (!sub || idx < 0) return;
            if (CMenuItem *it = sub->getEntry(idx)) {
                const int32_t tag = it->getTag();
                if (tag >= 0) env.c->LoadLibraryPatch(tag / 128, tag % 128);
            }
        });
    }

    void SavePatch() {
        std::filesystem::path p;
        if (PickFile(hwnd, true, env.c->PatchName() + ".sy1", p))
            Flash(env.c->SavePatchFile(p.string()) ? "Saved " + Utf8(p.filename().wstring()) : "Could not save patch");
    }

    void MainMenu(const CPoint &where) {
        menu = makeOwned<COptionMenu>();
        menu->addEntry("Initialize patch", 0);
        menu->addEntry("Load patch file...", 1);
        menu->addEntry("Save patch file...", 2);
        menu->addEntry("All notes off", 3);
        menu->addSeparator();
        menu->addEntry("BG Color...", 4);
        menu->addEntry("Color...", 5);
        menu->addEntry("Reset colors", 6);
        menu->popup(frame, where, [this](COptionMenu *m) {
            switch (m->getLastResult()) {
                case 0: env.c->ApplyPatch(s2::InitPatch()); break;
                case 1: {
                    std::filesystem::path p;
                    if (PickFile(hwnd, false, {}, p))
                        Flash(env.c->LoadPatchFile(p.string()) ? "Loaded " + Utf8(p.filename().wstring()) : "Could not load patch");
                    break;
                }
                case 2: SavePatch(); break;
                case 3: env.c->AllNotesOff(); break;
                case 4: ChooseBackground(); break;
                case 5: ChooseAccent(); break;
                case 6: ResetColors(); break;
                default: break;
            }
        });
    }

    void ApplyBackground() {
        frame->setBackgroundColor(BgColor());
        frame->invalid();
    }
    void ChooseBackground() {
        uint32_t rgb = gBackground;
        if (!PickColor(hwnd, rgb)) return;
        gBackground = rgb;
        SaveBackground();
        ApplyBackground();
    }
    void ChooseAccent() {
        uint32_t rgb = gAccent;
        if (!PickColor(hwnd, rgb)) return;
        gAccent = rgb;
        SaveAccent();
        frame->invalid(); // knobs redraw with the new colour
    }
    void ResetColors() {
        gBackground = kDefaultBackground;
        gAccent = kDefaultAccent;
        SaveBackground();
        SaveAccent();
        ApplyBackground();
    }

    void ShowReadout(int id) {
        readoutId = id;
        readoutAt = std::chrono::steady_clock::now();
    }

    void Refresh() {
        env.c->Library().PollRescan();
        for (ParamView *p : params) p->Sync();
        const auto now = std::chrono::steady_clock::now();
        const s2::LibraryProgram &prog = env.c->Program();
        const std::string l1 = (prog.slot >= 0 ? Pad3(prog.slot + 1) : std::string("---")) + ":" + env.c->PatchName();
        std::string l2;
        bool readout = false;
        if (!notice.empty() && now - noticeAt < std::chrono::seconds(3)) {
            l2 = notice;
            readout = true;
        } else if (readoutId >= 0 && now - readoutAt < std::chrono::milliseconds(1500)) {
            const int sync = s2::TempoSyncSwitch(readoutId);
            const int v = env.c->NativeValue(readoutId);
            l2 = std::string(s2::DisplayName(readoutId, env.c->NativeValue(s2::kEffectType))) + ": " +
                 s2::FormatParam(readoutId, v, sync >= 0 && env.c->NativeValue(sync), env.c->NativeValue(s2::kEffectType));
            readout = true;
        } else if (prog.slot >= 0) {
            l2 = prog.bank == s2::kFactoryProgramBank ? s2::kFactoryBankName : prog.bank;
        } else {
            l2 = "click here to browse banks";
        }
        const std::string note = env.c->Library().TakeNotice();
        if (!note.empty()) Flash(note);
        lcd->Set(l1, l2, readout);
    }
};

// ---- editor ---------------------------------------------------------------------

OxoSynth2WinEditor::OxoSynth2WinEditor(OxoSynth2Controller *controller)
    : VSTGUIEditor(static_cast<Steinberg::Vst::EditController *>(controller)), controller_(controller) {
    baseW_ = 770; // Synth1's proportions (the Mac panel's design canvas)
    baseH_ = 560;
    setRect(Steinberg::ViewRect(0, 0, baseW_, baseH_));
}

OxoSynth2WinEditor::~OxoSynth2WinEditor() = default;

#if VSTGUI_VERSION_MAJOR >= 4 && VSTGUI_VERSION_MINOR >= 1
bool PLUGIN_API OxoSynth2WinEditor::open(void *parent, const PlatformType &platformType)
#else
bool PLUGIN_API OxoSynth2WinEditor::open(void *parent)
#endif
{
    if (frame) return false;
    // Layout is always in design units; the frame's zoom does the scaling.
    const CRect size(0, 0, baseW_, baseH_);

    panel_ = std::make_unique<Panel>();
    Panel &p = *panel_;
    p.env.c = controller_;
    p.env.readout = [&p](int id) { p.ShowReadout(id); };
    p.env.popup = [&p](const CPoint &where, std::vector<MenuGroup> groups, std::function<void(int)> pick) {
        p.Popup(where, groups, std::move(pick));
    };
    p.hwnd = static_cast<HWND>(parent);

    frame = new CFrame(size, this);
    LoadSettings();
    frame->setBackgroundColor(BgColor());
    p.frame = frame;
    Env &env = p.env;
    OxoSynth2Controller *c = controller_;

    // ---- placement helpers: coordinates are the Mac panel's (section-relative) ----
    CCoord ox = 0, oy = 0;
    const auto add = [&](CView *v) { frame->addView(v); };
    const auto addParam = [&](ParamView *v) {
        frame->addView(v);
        p.params.push_back(v);
    };
    const auto section = [&](double x, double y, double w, double h, const char *title) {
        ox = x;
        oy = y;
        add(new SectionBox(CRect(x, y, x + w, y + h), title));
    };
    const auto knob = [&](int id, const char *label, double x, double y, double sz = 24) {
        addParam(new Knob(ox + x, oy + y, env, id, label, sz));
    };
    const auto toggle = [&](int id, const char *title, double x, double y, double w = 40) {
        addParam(new Toggle(ox + x, oy + y, w, env, id, title));
    };
    const auto cycler = [&](int id, const char *title, double x, double y, double w = 40) {
        addParam(new CycleButton(ox + x, oy + y, w, env, id, title));
    };
    const auto display = [&](int id, double x, double y, double w = 30, bool cycles = false, bool two = false,
                             std::function<void(ValueDisplay &)> tap = nullptr) {
        addParam(new ValueDisplay(ox + x, oy + y, w, env, id, cycles, two, std::move(tap)));
    };
    const auto badge = [&](const char *text, double x, double y, int id = -1) { addParam(new Badge(ox + x, oy + y, env, id, text)); };
    const auto label = [&](const char *text, double x, double y) { add(new Label(ox + x, oy + y, text)); };
    const auto divider = [&](double x1, double y1, double x2, double y2) { add(new Divider(ox + x1, oy + y1, ox + x2, oy + y2)); };
    const auto radio = [&](int id, std::vector<std::string> labels, int from, double x, double y, bool horizontal = false, bool trailing = false,
                           bool showLabels = true, double rowH = 13) {
        std::vector<LedRadio::Option> o;
        for (size_t i = 0; i < labels.size(); ++i) o.push_back({from + int(i), labels[i]});
        addParam(new LedRadio(ox + x, oy + y, env, id, std::move(o), horizontal, trailing, showLabels, rowH));
    };
    const auto waves = [&](int id, std::vector<WaveRadio::Shape> shapes, double x, double y) {
        addParam(new WaveRadio(ox + x, oy + y, env, id, std::move(shapes)));
    };
    const auto lfoWave = [&](int id, double x, double y) {
        addParam(new WaveCycle(ox + x, oy + y, env, id, {Wave::Saw, Wave::Tri, Wave::Square, Wave::SampleHold, Wave::Smooth, Wave::Sine}));
    };
    const auto ctrlSrc = [&](int id, double x, double y) {
        addParam(new MenuDisplay(ox + x, oy + y, 120, env, id, CtrlSrcLabel, CtrlSrcGroups));
    };
    const auto ctrlAssign = [&](int id, double x, double y) {
        addParam(new MenuDisplay(
            ox + x, oy + y, 120, env, id, [c](int v) { return AssignName(v, c->NativeValue(kEffectType)); },
            [c] {
                std::vector<MenuGroup> g(1);
                g[0].items.emplace_back(-1, "none");
                for (int t : kAssignTargets) g[0].items.emplace_back(t, AssignName(t, c->NativeValue(kEffectType)));
                return g;
            }));
    };

    // ---- title band ---------------------------------------------------------------
    add(new TitleBand(CRect(0, 4, 770, 30)));
    add(new PushButton(CRect(300, 7, 366, 27), "BG Color", [&p](PushButton &) { p.ChooseBackground(); }, kButton, 10));
    add(new PushButton(CRect(370, 7, 424, 27), "Color", [&p](PushButton &) { p.ChooseAccent(); }, kButton, 10));

    // ---- Oscillators ----------------------------------------------------------------
    section(6, 34, 312, 240, "Oscillators");
    divider(166, 22, 166, 76);
    divider(8, 81, 304, 81);
    divider(212, 87, 212, 234);
    divider(8, 163, 204, 163);
    badge("1", 14, 36);
    waves(kOsc1Shape, {{0, Wave::Sine}, {1, Wave::Saw}, {2, Wave::Pulse}, {3, Wave::Tri}}, 50, 40);
    knob(76, "det", 100, 36);
    knob(45, "FM", 136, 36);
    label("sub", 186, 24);
    waves(kSubShape, {{0, Wave::Sine}, {1, Wave::Tri}, {2, Wave::Saw}, {3, Wave::Square}}, 208, 44);
    knob(kSubGain, "sub", 260, 36);
    radio(kSubOctave, {"0oct", "-1oct"}, 0, 234, 70, true);
    badge("2", 14, 94);
    waves(1, {{1, Wave::Saw}, {2, Wave::Pulse}, {3, Wave::Tri}, {4, Wave::Noise}}, 50, 98);
    toggle(7, "ring", 104, 94);
    toggle(6, "sync", 150, 94);
    display(9, 260, 90, 32, false, true);
    label("key shift", 260, 106);
    toggle(4, "track", 36, 136, 52);
    knob(kOsc2Pitch, "pitch", 104, 132);
    knob(3, "fine", 146, 132);
    knob(5, "mix", 238, 132);
    knob(8, "p/w", 282, 132);
    toggle(10, "m.env", 36, 184, 52);
    knob(12, "A", 104, 184);
    knob(13, "D", 140, 184);
    knob(11, "amt", 176, 184);
    cycler(71, "dest.", 36, 220, 52);
    radio(71, {"osc2", "FM", "p/w"}, 0, 126, 220, true);
    knob(kOscPhase, "phase", 238, 206);
    knob(72, "tune", 282, 206);

    // ---- LFO ------------------------------------------------------------------------
    section(6, 278, 312, 130, "LFO");
    {
        const std::vector<std::string> dests = {"osc2", "osc1,2", "filter", "amp", "p/w", "FM", "pan"};
        badge("1", 14, 30, 57);
        lfoWave(42, 42, 30);
        cycler(kLfo1Dest, "dst", 78, 30, 32);
        knob(43, "spd", 30, 62);
        knob(44, "amt", 76, 62);
        label("-sync-", 52, 99);
        toggle(67, "tempo", 30, 114, 46);
        toggle(68, "key", 78, 114, 36);
        radio(kLfo1Dest, dests, 1, 124, 72, false, false, false);
        for (size_t i = 0; i < dests.size(); ++i) label(dests[i].c_str(), 156, 72 - 45.5 + 13.0 * double(i) + 6.5);
        radio(46, dests, 1, 188, 72, false, true, false);
        cycler(46, "dst", 234, 30, 32);
        lfoWave(47, 270, 30);
        badge("2", 298, 30, 58);
        knob(48, "spd", 236, 62);
        knob(49, "amt", 282, 62);
        label("-sync-", 260, 99);
        toggle(69, "tempo", 236, 114, 46);
        toggle(70, "key", 284, 114, 36);
    }

    // ---- Wheel / MIDI -----------------------------------------------------------------
    section(6, 412, 536, 66, "Wheel / MIDI");
    label("p.b range", 32, 28);
    display(40, 32, 46, 30, false, true);
    label("src1", 82, 31);
    ctrlSrc(kCtrlSrc1, 158, 31);
    label("->", 228, 31);
    ctrlAssign(87, 298, 31);
    label("src2", 82, 51);
    ctrlSrc(88, 158, 51);
    label("->", 228, 51);
    ctrlAssign(kCtrlAssign2, 298, 51);
    knob(50, "sens1", 400, 34, 22);
    knob(51, "sens2", 446, 34, 22);

    // ---- Amplifier ----------------------------------------------------------------------
    section(324, 34, 218, 80, "Amplifier");
    knob(25, "A", 20, 40);
    knob(26, "D", 55, 40);
    knob(27, "S", 90, 40);
    knob(28, "R", 125, 40);
    knob(29, "gain", 162, 40);
    knob(30, "vel", 198, 40);

    // ---- Filter --------------------------------------------------------------------------
    section(324, 118, 218, 156, "Filter");
    knob(15, "A", 20, 44);
    knob(16, "D", 52, 44);
    knob(17, "S", 84, 44);
    knob(18, "R", 116, 44);
    knob(21, "amt", 150, 44);
    cycler(kFilterType, "type", 196, 30, 36);
    radio(kFilterType, {"LP12", "LP24", "HP12", "BP12", "LPDL"}, 0, 196, 82, false, false, true, 14);
    knob(19, "frq", 20, 106);
    knob(20, "res", 52, 106);
    knob(23, "sat", 84, 106);
    knob(22, "trk", 116, 106);
    toggle(24, "vel", 150, 112, 36);

    // ---- Arpeggiator -----------------------------------------------------------------------
    section(324, 278, 218, 130, "Arpeggiator");
    toggle(59, "ON", 22, 30, 32);
    cycler(31, "type", 66, 30, 40);
    cycler(32, "range", 128, 30, 44);
    radio(31, {"updown", "up", "down", "random"}, 1, 40, 78, false, false, true, 14);
    radio(32, {"1oct", "2oct", "3oct", "4oct"}, 0, 126, 78, false, false, true, 14);
    knob(33, "beat", 190, 36);
    knob(34, "gate", 190, 88);

    // ---- Effect ---------------------------------------------------------------------------
    section(548, 34, 216, 80, "Effect");
    toggle(77, "ON", 22, 40, 32);
    display(kEffectType, 66, 40, 46, true, false, [c, &p](ValueDisplay &d) {
        std::vector<MenuGroup> g(1);
        const int n = Info(kEffectType).max - Info(kEffectType).min + 1;
        for (int v = 0; v < n; ++v) g[0].items.emplace_back(v, FormatParam(kEffectType, v, false, v));
        (void)c;
        ValueDisplay *dp = &d;
        p.Popup(CPoint(d.getViewSize().left, d.getViewSize().bottom), g, [dp](int v) { dp->Pick(v); });
    });
    knob(79, "ctl1", 118, 36);
    knob(80, "ctl2", 154, 36);
    knob(81, "level", 190, 36);

    // ---- Equalizer / Pan ------------------------------------------------------------------
    section(548, 118, 216, 58, "Equalizer / Pan");
    knob(61, "freq", 24, 30, 22);
    knob(62, "level", 66, 30, 22);
    knob(63, "Q", 108, 30, 22);
    knob(60, "tone", 150, 30, 22);
    knob(kPan, "L-R", 192, 30, 22);

    // ---- Tempo Delay ----------------------------------------------------------------------
    section(548, 180, 216, 58, "Tempo Delay");
    toggle(65, "ON", 17, 32, 28);
    display(82, 48, 32, 28, true);
    knob(35, "time", 84, 30, 22);
    knob(83, "sprd", 111, 30, 22);
    knob(36, "fdbk", 138, 30, 22);
    knob(kDelayTone, "tone", 165, 30, 22);
    knob(37, "d/w", 192, 30, 22);

    // ---- Chorus / Flanger -----------------------------------------------------------------
    section(548, 242, 216, 58, "Chorus / Flanger");
    toggle(66, "ON", 17, 32, 28);
    display(64, 48, 32, 28, true);
    knob(52, "time", 84, 30, 22);
    knob(53, "depth", 111, 30, 22);
    knob(54, "rate", 138, 30, 22);
    knob(55, "fdbk", 165, 30, 22);
    knob(56, "level", 192, 30, 22);

    // ---- Voice ----------------------------------------------------------------------------
    section(548, 304, 216, 174, "Voice");
    radio(kPlayMode, {"poly", "mono", "legato"}, 0, 34, 46, false, false, true, 14);
    display(kPolyphony, 130, 38, 30);
    label("poly", 130, 55);
    toggle(73, "unison", 36, 100, 56);
    label("portamento", 153, 77);
    knob(39, "", 128, 100);
    toggle(74, "auto", 178, 100, 38);
    display(93, 24, 138, 26);
    label("num", 24, 155);
    knob(75, "det", 68, 136);
    knob(92, "phase", 106, 136);
    knob(84, "sprd", 144, 136);
    knob(85, "pitch", 182, 136);

    // ---- bottom bar (origin 6, 484) ---------------------------------------------------------
    ox = 6;
    oy = 484;
    {
        auto *vol = new Knob(ox + 19, oy + 28, env, kVolumeParamId, "vol", 26);
        addParam(vol);
        p.lcd = new Lcd(CRect(ox + 44, oy, ox + 44 + 432, oy + 70), [&p](Lcd &l) { p.Browse(CPoint(l.getViewSize().left, l.getViewSize().bottom)); });
        add(p.lcd);
        add(new PushButton(CRect(ox + 392, oy + 9, ox + 428, oy + 29), "<", [&p](PushButton &) { p.StepProgram(-1); }));
        add(new PushButton(CRect(ox + 432, oy + 9, ox + 468, oy + 29), ">", [&p](PushButton &) { p.StepProgram(1); }));
        add(new PushButton(CRect(ox + 486, oy + 4, ox + 536, oy + 30), "save", [&p](PushButton &) { p.SavePatch(); }, kRed));
        add(new PushButton(CRect(ox + 486, oy + 38, ox + 536, oy + 64), "menu", [&p](PushButton &b) {
            p.MainMenu(CPoint(b.getViewSize().left, b.getViewSize().bottom));
        }));
    }

    // Resize grip (added last: on top of the panels).
    {
        constexpr CCoord g = 16;
        add(new Grip(CRect(size.getWidth() - g, size.getHeight() - g, size.getWidth(), size.getHeight()), size.getWidth(), size.getHeight(),
                     [this] { return frame ? frame->getZoom() : 1.0; }, [this](double z) { RequestScale(z); }, [] { SaveScale(); }));
    }

    // The library list is scanned on a worker; the LCD polls it.
    controller_->Library().StartRescan();

#if VSTGUI_VERSION_MAJOR >= 4 && VSTGUI_VERSION_MINOR >= 1
    frame->open(parent, platformType);
#else
    frame->open(parent);
#endif
    if (std::abs(gScale - 1.0) > 1e-3) frame->setZoom(gScale); // restore the last size
    p.Refresh();
    return true;
}

void PLUGIN_API OxoSynth2WinEditor::close() {
    if (frame) {
        SaveScale();
        frame->removeAll(true);
        frame->close(); // releases the frame (and the views it still owns)
        frame = nullptr;
    }
    panel_.reset();
    gMeasure = nullptr;
}

void OxoSynth2WinEditor::RequestScale(double zoom) {
    if (!plugFrame || baseW_ <= 0) return;
    zoom = std::clamp(zoom, kMinScale, kMaxScale);
    const int w = int(std::lround(baseW_ * zoom)), h = int(std::lround(baseH_ * zoom));
    const Steinberg::ViewRect cur = getRect();
    if (w == cur.getWidth() && h == cur.getHeight()) return;
    Steinberg::ViewRect vr(0, 0, w, h);
    plugFrame->resizeView(this, &vr); // the host answers with onSize
}

Steinberg::tresult PLUGIN_API OxoSynth2WinEditor::onSize(Steinberg::ViewRect *newSize) {
    if (frame && newSize && baseW_ > 0) {
        const double z = std::clamp(double(newSize->getWidth()) / baseW_, kMinScale, kMaxScale);
        if (std::abs(z - frame->getZoom()) > 1e-4) frame->setZoom(z);
        gScale = z;
    }
    // Not VSTGUIEditor::onSize: that would set the frame to the raw pixel
    // size and undo the zoom bookkeeping above.
    return Steinberg::Vst::EditorView::onSize(newSize);
}

Steinberg::tresult PLUGIN_API OxoSynth2WinEditor::checkSizeConstraint(Steinberg::ViewRect *rect) {
    if (!rect || baseW_ <= 0) return Steinberg::kResultFalse;
    // Keep the design proportions: one zoom from both dimensions.
    const double z = std::clamp((double(rect->getWidth()) / baseW_ + double(rect->getHeight()) / baseH_) / 2.0, kMinScale, kMaxScale);
    rect->right = rect->left + int(std::lround(baseW_ * z));
    rect->bottom = rect->top + int(std::lround(baseH_ * z));
    return Steinberg::kResultTrue;
}

CMessageResult OxoSynth2WinEditor::notify(CBaseObject *sender, const char *message) {
    if (message == CVSTGUITimer::kMsgTimer && panel_ && frame) panel_->Refresh();
    return VSTGUIEditor::notify(sender, message);
}

} // namespace oxo

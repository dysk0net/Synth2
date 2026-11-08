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

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

using namespace VSTGUI;

namespace oxo {

namespace {

// ---- look -----------------------------------------------------------------

const CColor kStripText{214, 220, 230, 255};
const CColor kKnob{18, 20, 24, 255};
const CColor kKnobEdge{92, 100, 114, 255};
const CColor kTrackOff{64, 70, 80, 255};
const CColor kLed{92, 224, 112, 255};
const CColor kLedOff{40, 74, 46, 255};
const CColor kLabel{170, 178, 192, 255};
const CColor kLcdBg{12, 38, 44, 255};
const CColor kLcdText{110, 232, 230, 255};
const CColor kBtn{70, 77, 90, 255};

// ---- user settings (colours, size), remembered ---------------------------------
// One setting for every instance, in %APPDATA%\Oxo Music\Oxo Synth2:
//   background.txt  "RRGGBB"  panel background  (BG Color)
//   accent.txt      "RRGGBB"  knob arc / dots / pointer  (Color)
//   scale.txt       "1.25"    window zoom
// The section panels are derived from the background (a little lighter or
// darker), so any background keeps the strips readable.

constexpr uint32_t kDefaultBackground = 0x22252A, kDefaultAccent = 0xFFB040;
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

CColor Shade(uint32_t rgb, int delta) {
    const auto ch = [&](int shift) { return uint8_t(std::clamp(int((rgb >> shift) & 0xFF) + delta, 0, 255)); };
    return CColor(ch(16), ch(8), ch(0), 255);
}
// Lighter on dark backgrounds, darker on light ones, so panels always stand out a little.
bool BackgroundIsLight() {
    const int r = int((gBackground >> 16) & 0xFF), g = int((gBackground >> 8) & 0xFF), b = int(gBackground & 0xFF);
    return (r * 299 + g * 587 + b * 114) / 1000 > 140;
}
CColor BgColor() { return Shade(gBackground, 0); }
CColor PanelColor() { return Shade(gBackground, BackgroundIsLight() ? -14 : 7); }
CColor PanelEdge() { return Shade(gBackground, BackgroundIsLight() ? -34 : 18); }
CColor PanelHeader() { return Shade(gBackground, BackgroundIsLight() ? -48 : 30); }
CColor HeaderText() { return BackgroundIsLight() ? CColor(30, 33, 38, 255) : kStripText; }
CColor LabelText() { return BackgroundIsLight() ? CColor(70, 76, 88, 255) : kLabel; }

constexpr CCoord kCellW = 54, kCellH = 60, kStripH = 18, kPad = 6, kGap = 6;
constexpr CCoord kHeaderH = 62, kMaxRowWidth = 980;

enum class Kind { Knob, Toggle, Step };

struct Item {
    int id;
    Kind kind;
    const char *label; // nullptr = the effect-aware display name
};
struct Section {
    const char *title;
    int cols;
    std::vector<Item> items;
};

using namespace s2;

// Order of items = row-major in the section's grid.
const std::vector<Section> &Sections() {
    static const std::vector<Section> s = {
        {"OSC", 6,
         {{kOsc1Shape, Kind::Step, "osc1"}, {kOsc2Shape, Kind::Step, "osc2"}, {kOsc2Pitch, Kind::Knob, "pitch"},
          {kOsc2Fine, Kind::Knob, "fine"}, {kOsc2KbdTrack, Kind::Toggle, "kbd trk"}, {kOscMix, Kind::Knob, "mix"},
          {kOsc2Sync, Kind::Toggle, "sync"}, {kOsc2Ring, Kind::Toggle, "ring"}, {kOscPulseWidth, Kind::Knob, "pw"},
          {kOscKeyShift, Kind::Knob, "key shift"}, {kOsc1FM, Kind::Knob, "fm"}, {kOsc1Detune, Kind::Knob, "detune"},
          {kSubGain, Kind::Knob, "sub"}, {kSubShape, Kind::Step, "sub shape"}, {kSubOctave, Kind::Toggle, "sub -2"},
          {kOscFineTune, Kind::Knob, "tune"}, {kOscPhase, Kind::Knob, "phase"}}},
        {"MOD ENV", 5,
         {{kModEnvOn, Kind::Toggle, "on"}, {kModEnvDest, Kind::Step, "dest"}, {kModEnvAmount, Kind::Knob, "amount"},
          {kModEnvAttack, Kind::Knob, "attack"}, {kModEnvDecay, Kind::Knob, "decay"}}},
        {"LFO 1", 4,
         {{kLfo1On, Kind::Toggle, "on"}, {kLfo1Dest, Kind::Step, "dest"}, {kLfo1Type, Kind::Step, "type"},
          {kLfo1Speed, Kind::Knob, "speed"}, {kLfo1Depth, Kind::Knob, "depth"}, {kLfo1TempoSync, Kind::Toggle, "tempo"},
          {kLfo1KeySync, Kind::Toggle, "key sync"}}},
        {"LFO 2", 4,
         {{kLfo2On, Kind::Toggle, "on"}, {kLfo2Dest, Kind::Step, "dest"}, {kLfo2Type, Kind::Step, "type"},
          {kLfo2Speed, Kind::Knob, "speed"}, {kLfo2Depth, Kind::Knob, "depth"}, {kLfo2TempoSync, Kind::Toggle, "tempo"},
          {kLfo2KeySync, Kind::Toggle, "key sync"}}},
        {"WHEEL", 3,
         {{kWheelSpeedSens, Kind::Knob, "sens 1"}, {kWheelAmountSens, Kind::Knob, "sens 2"},
          {kBendRange, Kind::Knob, "bend rng"}}},
        {"AMP", 6,
         {{kAmpAttack, Kind::Knob, "attack"}, {kAmpDecay, Kind::Knob, "decay"}, {kAmpSustain, Kind::Knob, "sustain"},
          {kAmpRelease, Kind::Knob, "release"}, {kAmpGain, Kind::Knob, "gain"}, {kAmpVelSens, Kind::Knob, "vel sens"}}},
        {"FILTER", 7,
         {{kFilterType, Kind::Step, "type"}, {kFilterFreq, Kind::Knob, "freq"}, {kFilterRes, Kind::Knob, "res"},
          {kFilterAmount, Kind::Knob, "amount"}, {kFilterKbdTrack, Kind::Knob, "kbd trk"},
          {kFilterSat, Kind::Knob, "sat"}, {kFilterVelSwitch, Kind::Toggle, "vel sw"},
          {kFilterAttack, Kind::Knob, "attack"}, {kFilterDecay, Kind::Knob, "decay"},
          {kFilterSustain, Kind::Knob, "sustain"}, {kFilterRelease, Kind::Knob, "release"}}},
        {"ARP", 5,
         {{kArpOn, Kind::Toggle, "on"}, {kArpType, Kind::Step, "type"}, {kArpOctRange, Kind::Step, "oct"},
          {kArpBeat, Kind::Step, "beat"}, {kArpGate, Kind::Knob, "gate"}}},
        {"PLAY", 5,
         {{kPlayMode, Kind::Step, "mode"}, {kPortaTime, Kind::Knob, "porta"}, {kPortaAuto, Kind::Toggle, "auto"},
          {kPolyphony, Kind::Knob, "poly"}, {kPan, Kind::Knob, "pan"}}},
        {"UNISON", 6,
         {{kUnisonOn, Kind::Toggle, "on"}, {kUnisonVoices, Kind::Knob, "voices"}, {kUnisonDetune, Kind::Knob, "detune"},
          {kUnisonPitch, Kind::Knob, "pitch"}, {kUnisonPan, Kind::Knob, "pan"}, {kUnisonPhase, Kind::Knob, "phase"}}},
        {"EFFECT", 5,
         {{kEffectOn, Kind::Toggle, "on"}, {kEffectType, Kind::Step, "type"}, {kEffectCtrl1, Kind::Knob, nullptr},
          {kEffectCtrl2, Kind::Knob, nullptr}, {kEffectLevel, Kind::Knob, nullptr}}},
        {"EQ", 4,
         {{kEqTone, Kind::Knob, "tone"}, {kEqFreq, Kind::Knob, "freq"}, {kEqLevel, Kind::Knob, "level"},
          {kEqQ, Kind::Knob, "Q"}}},
        {"DELAY", 7,
         {{kDelayOn, Kind::Toggle, "on"}, {kDelayType, Kind::Step, "type"}, {kDelayTime, Kind::Step, "time"},
          {kDelayFeedback, Kind::Knob, "feedback"}, {kDelayDryWet, Kind::Knob, "dry/wet"},
          {kDelaySpread, Kind::Knob, "spread"}, {kDelayTone, Kind::Knob, "tone"}}},
        {"CHORUS", 7,
         {{kChorusOn, Kind::Toggle, "on"}, {kChorusType, Kind::Step, "type"}, {kChorusTime, Kind::Knob, "time"},
          {kChorusDepth, Kind::Knob, "depth"}, {kChorusRate, Kind::Knob, "rate"},
          {kChorusFeedback, Kind::Knob, "feedback"}, {kChorusLevel, Kind::Knob, "level"}}},
    };
    return s;
}

int DefaultValue(int id) {
    return id == kVolumeParamId ? kVolumeParam.def : kInitPatchValues[size_t(std::clamp(id, 0, int(kParamCount) - 1))];
}

// ---- controls ---------------------------------------------------------------

// Everything a control needs from the panel.
struct Env {
    OxoSynth2Controller *c = nullptr;
    std::function<void(int)> readout; // show "name: value" on the LCD
};

class ParamView : public CView {
public:
    ParamView(const CRect &r, Env &env, int id, const char *label) : CView(r), env_(env), id_(id), label_(label ? label : "") {
        const ParamInfo &p = Info(id);
        min_ = p.min;
        max_ = p.max;
        value_ = env.c->NativeValue(id);
    }

    // Pulled from the controller (host automation, patch loads).
    void Sync() {
        const int32_t v = env_.c->NativeValue(id_);
        if (v != value_) {
            value_ = v;
            invalid();
        }
    }
    int Id() const { return id_; }

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
    std::string Label() const {
        if (!label_.empty()) return label_;
        return DisplayName(id_, env_.c->NativeValue(kEffectType));
    }
    std::string ValueText() const {
        const int sync = TempoSyncSwitch(id_);
        return FormatParam(id_, value_, sync >= 0 && env_.c->NativeValue(sync), env_.c->NativeValue(kEffectType));
    }
    void DrawLabel(CDrawContext *dc, const CRect &cell) const {
        CRect r(cell.left, cell.bottom - 14, cell.right, cell.bottom);
        dc->setFont(kNormalFontSmaller);
        dc->setFontColor(LabelText());
        dc->drawString(Label().c_str(), r, kCenterText);
    }

    Env &env_;
    int id_;
    std::string label_;
    int min_ = 0, max_ = 1, value_ = 0;
};

class Knob : public ParamView {
public:
    using ParamView::ParamView;

    void draw(CDrawContext *dc) override {
        const CRect v = getViewSize();
        const CCoord cx = v.left + v.getWidth() / 2, cy = v.top + 26, r = 17;
        dc->setDrawMode(kAntiAliasing);
        dc->setFillColor(kKnob);
        dc->setFrameColor(kKnobEdge);
        dc->setLineWidth(1.5);
        CRect disc(cx - r + 3, cy - r + 3, cx + r - 3, cy + r - 3);
        dc->drawEllipse(disc, kDrawFilledAndStroked);

        const double t = max_ > min_ ? double(value_ - min_) / double(max_ - min_) : 0.0;
        constexpr int kDots = 27;
        for (int i = 0; i < kDots; ++i) {
            const double a = (135.0 + 270.0 * i / (kDots - 1)) * M_PI / 180.0;
            const bool on = double(i) / (kDots - 1) <= t + 1e-9 && value_ != min_;
            dc->setFillColor(on ? AccentColor() : kTrackOff);
            const CCoord px = cx + std::cos(a) * (r + 1), py = cy + std::sin(a) * (r + 1);
            dc->drawEllipse(CRect(px - 1.4, py - 1.4, px + 1.4, py + 1.4), kDrawFilled);
        }
        const double a = (135.0 + 270.0 * t) * M_PI / 180.0;
        dc->setFrameColor(AccentColor());
        dc->setLineWidth(2.0);
        dc->drawLine(CPoint(cx + std::cos(a) * 3, cy + std::sin(a) * 3), CPoint(cx + std::cos(a) * (r - 5), cy + std::sin(a) * (r - 5)));
        DrawLabel(dc, v);
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
        const double px = e.modifiers.has(ModifierKey::Shift) ? 1000.0 : 160.0; // pixels for the full range
        Set(startValue_ + int(std::lround((startY_ - e.mousePosition.y) * double(max_ - min_) / px)));
        e.consumed = true;
    }
    void onMouseUpEvent(MouseUpEvent &e) override {
        Finish();
        e.consumed = true;
    }
    void onMouseCancelEvent(MouseCancelEvent &) override { Finish(); }
    void onMouseWheelEvent(MouseWheelEvent &e) override {
        const int range = max_ - min_;
        const int step = e.modifiers.has(ModifierKey::Shift) ? 1 : std::max(1, range / 40);
        if (e.deltaY == 0) return;
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
    bool dragging_ = false;
    double startY_ = 0;
    int startValue_ = 0;
};

class Toggle : public ParamView {
public:
    using ParamView::ParamView;
    void draw(CDrawContext *dc) override {
        const CRect v = getViewSize();
        const CCoord cx = v.left + v.getWidth() / 2, cy = v.top + 24;
        dc->setDrawMode(kAntiAliasing);
        const bool on = value_ != 0;
        dc->setFillColor(kBtn);
        dc->setFrameColor(kKnobEdge);
        dc->setLineWidth(1);
        dc->drawRect(CRect(cx - 16, cy - 10, cx + 16, cy + 10), kDrawFilledAndStroked);
        dc->setFillColor(on ? kLed : kLedOff);
        dc->drawEllipse(CRect(cx - 5, cy - 5, cx + 5, cy + 5), kDrawFilled);
        DrawLabel(dc, v);
        setDirty(false);
    }
    void onMouseDownEvent(MouseDownEvent &e) override {
        if (!e.buttonState.isLeft()) return;
        Edit([&] { Set(value_ ? 0 : 1); });
        e.consumed = true;
    }
    void onMouseWheelEvent(MouseWheelEvent &e) override {
        if (e.deltaY == 0) return;
        Edit([&] { Set(e.deltaY > 0 ? 1 : 0); });
        e.consumed = true;
    }
    void onMouseEnterEvent(MouseEnterEvent &) override { env_.readout(id_); }
};

// Discrete parameter: the formatted value in a box; click = next, Shift-click = back.
class Step : public ParamView {
public:
    using ParamView::ParamView;
    void draw(CDrawContext *dc) override {
        const CRect v = getViewSize();
        const CCoord cx = v.left + v.getWidth() / 2, cy = v.top + 24;
        dc->setDrawMode(kAntiAliasing);
        dc->setFillColor(kLcdBg);
        dc->setFrameColor(kKnobEdge);
        dc->setLineWidth(1);
        const CRect box(v.left + 2, cy - 11, v.right - 2, cy + 11);
        dc->drawRect(box, kDrawFilledAndStroked);
        dc->setFont(kNormalFontSmaller);
        dc->setFontColor(kLcdText);
        dc->drawString(ValueText().c_str(), box, kCenterText);
        (void)cx;
        DrawLabel(dc, v);
        setDirty(false);
    }
    void onMouseDownEvent(MouseDownEvent &e) override {
        if (!e.buttonState.isLeft()) return;
        Move(e.modifiers.has(ModifierKey::Shift) ? -1 : 1);
        e.consumed = true;
    }
    void onMouseWheelEvent(MouseWheelEvent &e) override {
        if (e.deltaY == 0) return;
        Move(e.deltaY > 0 ? 1 : -1);
        e.consumed = true;
    }
    void onMouseEnterEvent(MouseEnterEvent &) override { env_.readout(id_); }

private:
    void Move(int dir) {
        int v = value_ + dir;
        if (v > max_) v = min_;
        if (v < min_) v = max_;
        Edit([&] { Set(v); });
    }
};

class Button : public CView {
public:
    Button(const CRect &r, std::string text, std::function<void(Button &, const MouseDownEvent &)> onClick)
        : CView(r), text_(std::move(text)), onClick_(std::move(onClick)) {}
    void draw(CDrawContext *dc) override {
        const CRect v = getViewSize();
        dc->setDrawMode(kAntiAliasing);
        dc->setFillColor(kBtn);
        dc->setFrameColor(kKnobEdge);
        dc->setLineWidth(1);
        dc->drawRect(v, kDrawFilledAndStroked);
        dc->setFont(kNormalFontSmall);
        dc->setFontColor(kStripText);
        dc->drawString(text_.c_str(), v, kCenterText);
        setDirty(false);
    }
    void onMouseDownEvent(MouseDownEvent &e) override {
        if (!e.buttonState.isLeft()) return;
        onClick_(*this, e);
        e.consumed = true;
    }

private:
    std::string text_;
    std::function<void(Button &, const MouseDownEvent &)> onClick_;
};

class Strip : public CView {
public:
    Strip(const CRect &r, const char *title) : CView(r), title_(title) {}
    void draw(CDrawContext *dc) override {
        const CRect v = getViewSize();
        dc->setDrawMode(kAntiAliasing);
        dc->setFillColor(PanelColor());
        dc->setFrameColor(PanelEdge());
        dc->setLineWidth(1);
        dc->drawRect(v, kDrawFilledAndStroked);
        dc->setFillColor(PanelHeader());
        dc->drawRect(CRect(v.left, v.top, v.right, v.top + kStripH), kDrawFilled);
        dc->setFont(kNormalFontSmall);
        dc->setFontColor(HeaderText());
        dc->drawString(title_.c_str(), CRect(v.left + 6, v.top, v.right, v.top + kStripH), kLeftText);
        setDirty(false);
    }

private:
    std::string title_;
};

class Lcd : public CView {
public:
    Lcd(const CRect &r, std::function<void(Lcd &)> onClick) : CView(r), onClick_(std::move(onClick)) {}
    void Set(const std::string &l1, const std::string &l2) {
        if (l1 == line1_ && l2 == line2_) return;
        line1_ = l1;
        line2_ = l2;
        invalid();
    }
    void draw(CDrawContext *dc) override {
        const CRect v = getViewSize();
        dc->setDrawMode(kAntiAliasing);
        dc->setFillColor(kLcdBg);
        dc->setFrameColor(kKnobEdge);
        dc->setLineWidth(1);
        dc->drawRect(v, kDrawFilledAndStroked);
        dc->setFontColor(kLcdText);
        dc->setFont(kNormalFont);
        dc->drawString(line1_.c_str(), CRect(v.left + 8, v.top + 4, v.right - 4, v.top + 24), kLeftText);
        dc->setFont(kNormalFontSmall);
        dc->drawString(line2_.c_str(), CRect(v.left + 8, v.top + 28, v.right - 4, v.bottom - 2), kLeftText);
        setDirty(false);
    }
    void onMouseDownEvent(MouseDownEvent &e) override {
        if (!e.buttonState.isLeft()) return;
        onClick_(*this);
        e.consumed = true;
    }

private:
    std::string line1_, line2_;
    std::function<void(Lcd &)> onClick_;
};

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
        dc->setFrameColor(LabelText());
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
    int32_t shownSerial_ = -1;
    int readoutId = -1;
    std::chrono::steady_clock::time_point readoutAt{};
    std::string notice;
    std::chrono::steady_clock::time_point noticeAt{};
    SharedPointer<COptionMenu> menu;
    CCoord width = 0, height = 0;

    void Flash(const std::string &text) {
        notice = text;
        noticeAt = std::chrono::steady_clock::now();
    }

    static std::string Pad3(int n) {
        char b[16];
        std::snprintf(b, sizeof b, "%03d", n);
        return b;
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
                case 2: {
                    std::filesystem::path p;
                    if (PickFile(hwnd, true, env.c->PatchName() + ".sy1", p))
                        Flash(env.c->SavePatchFile(p.string()) ? "Saved " + Utf8(p.filename().wstring()) : "Could not save patch");
                    break;
                }
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
        std::string l1 = prog.slot >= 0 ? Pad3(prog.slot + 1) + ": " + env.c->PatchName() : env.c->PatchName();
        std::string l2;
        if (!notice.empty() && now - noticeAt < std::chrono::seconds(3)) {
            l2 = notice;
        } else if (readoutId >= 0 && now - readoutAt < std::chrono::milliseconds(1500)) {
            const int sync = s2::TempoSyncSwitch(readoutId);
            const int v = env.c->NativeValue(readoutId);
            l2 = std::string(s2::DisplayName(readoutId, env.c->NativeValue(s2::kEffectType))) + ": " +
                 s2::FormatParam(readoutId, v, sync >= 0 && env.c->NativeValue(sync), env.c->NativeValue(s2::kEffectType));
        } else if (prog.slot >= 0) {
            const std::string rel = prog.bank == s2::kFactoryProgramBank ? s2::kFactoryBankName : prog.bank;
            l2 = rel;
        }
        const std::string note = env.c->Library().TakeNotice();
        if (!note.empty()) Flash(note);
        lcd->Set(l1, l2);
    }
};

// ---- editor ---------------------------------------------------------------------

OxoSynth2WinEditor::OxoSynth2WinEditor(OxoSynth2Controller *controller)
    : VSTGUIEditor(static_cast<Steinberg::Vst::EditController *>(controller)), controller_(controller) {
    // Size is only known once the sections are packed: do it here, once.
    CCoord x = kGap, y = kHeaderH + kGap, rowH = 0, maxW = 0;
    for (const Section &s : Sections()) {
        const int rows = int((s.items.size() + size_t(s.cols) - 1) / size_t(s.cols));
        const CCoord w = kPad * 2 + s.cols * kCellW, h = kStripH + kPad * 2 + rows * kCellH;
        if (x + w > kMaxRowWidth && x > kGap) {
            x = kGap;
            y += rowH + kGap;
            rowH = 0;
        }
        x += w + kGap;
        rowH = std::max(rowH, h);
        maxW = std::max(maxW, x);
    }
    baseW_ = int(std::max<CCoord>(maxW, 520));
    baseH_ = int(y + rowH + kGap + 8); // + room for the resize grip
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
    p.hwnd = static_cast<HWND>(parent);
    p.width = size.getWidth();
    p.height = size.getHeight();

    frame = new CFrame(size, this);
    LoadSettings();
    frame->setBackgroundColor(BgColor());
    p.frame = frame;

    // Header: [< >] [ LCD ........ ] [browse|menu] [volume]
    const CCoord W = size.getWidth(), volW = 56, btnW = 62, prevW = 28;
    const CCoord lcdL = 8 + prevW + 4, btnL = W - 8 - volW - 4 - btnW, colorW = 62, lcdR = btnL - 4 - colorW - 4;
    p.lcd = new Lcd(CRect(lcdL, 8, lcdR, kHeaderH - 6),
                    [&p](Lcd &l) { p.Browse(CPoint(l.getViewSize().left, l.getViewSize().bottom)); });
    frame->addView(p.lcd);
    frame->addView(new Button(CRect(8, 8, 8 + prevW, 8 + 24), "<", [&p](Button &, const MouseDownEvent &) { p.StepProgram(-1); }));
    frame->addView(new Button(CRect(8, 8 + 26, 8 + prevW, 8 + 50), ">", [&p](Button &, const MouseDownEvent &) { p.StepProgram(1); }));
    frame->addView(new Button(CRect(btnL, 8, btnL + btnW, 8 + 24), "browse", [&p](Button &b, const MouseDownEvent &) {
        p.Browse(CPoint(b.getViewSize().left, b.getViewSize().bottom));
    }));
    frame->addView(new Button(CRect(btnL - 4 - colorW, 8, btnL - 4, 8 + 24), "BG Color", [&p](Button &, const MouseDownEvent &) {
        p.ChooseBackground();
    }));
    frame->addView(new Button(CRect(btnL - 4 - colorW, 8 + 26, btnL - 4, 8 + 50), "Color", [&p](Button &, const MouseDownEvent &) {
        p.ChooseAccent();
    }));
    frame->addView(new Button(CRect(btnL, 8 + 26, btnL + btnW, 8 + 50), "menu", [&p](Button &b, const MouseDownEvent &) {
        p.MainMenu(CPoint(b.getViewSize().left, b.getViewSize().bottom));
    }));
    {
        auto *vol = new Knob(CRect(W - 8 - volW, 0, W - 8, kCellH), p.env, kVolumeParamId, "volume");
        frame->addView(vol);
        p.params.push_back(vol);
    }

    // Sections, shelf-packed left to right.
    CCoord x = kGap, y = kHeaderH + kGap, rowH = 0;
    for (const Section &s : Sections()) {
        const int rows = int((s.items.size() + size_t(s.cols) - 1) / size_t(s.cols));
        const CCoord w = kPad * 2 + s.cols * kCellW, h = kStripH + kPad * 2 + rows * kCellH;
        if (x + w > kMaxRowWidth && x > kGap) {
            x = kGap;
            y += rowH + kGap;
            rowH = 0;
        }
        frame->addView(new Strip(CRect(x, y, x + w, y + h), s.title));
        for (size_t i = 0; i < s.items.size(); ++i) {
            const Item &it = s.items[i];
            const CCoord cx = x + kPad + CCoord(i % size_t(s.cols)) * kCellW, cy = y + kStripH + kPad + CCoord(i / size_t(s.cols)) * kCellH;
            const CRect cell(cx, cy, cx + kCellW, cy + kCellH);
            ParamView *v = it.kind == Kind::Knob     ? static_cast<ParamView *>(new Knob(cell, p.env, it.id, it.label))
                           : it.kind == Kind::Toggle ? static_cast<ParamView *>(new Toggle(cell, p.env, it.id, it.label))
                                                     : static_cast<ParamView *>(new Step(cell, p.env, it.id, it.label));
            frame->addView(v);
            p.params.push_back(v);
        }
        x += w + kGap;
        rowH = std::max(rowH, h);
    }

    // Resize grip (added last: on top of the panels).
    {
        constexpr CCoord g = 16;
        frame->addView(new Grip(
            CRect(size.getWidth() - g, size.getHeight() - g, size.getWidth(), size.getHeight()), size.getWidth(), size.getHeight(),
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

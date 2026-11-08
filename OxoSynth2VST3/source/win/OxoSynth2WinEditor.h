#pragma once

// Windows editor: a VSTGUI panel (the SwiftUI panel in Shared/ is mac / iPad
// only). Same parameters, same patch library, same hooks into
// SynthEditorCore; the look is its own.

// VSTGUI tests `#elif WIN32 || WINDOWS ...`; the SDK sets WINDOWS to 1, but
// by the time we get here (EditorCore/Storage.hpp has pulled in windows.h) it
// can be left defined as nothing, which MSVC rejects (C1017). Restore it.
#ifdef WINDOWS
#undef WINDOWS
#endif
#define WINDOWS 1

#include "public.sdk/source/vst/vstguieditor.h"

#include <memory>

namespace oxo {

class OxoSynth2Controller;

class OxoSynth2WinEditor : public Steinberg::Vst::VSTGUIEditor {
public:
    explicit OxoSynth2WinEditor(OxoSynth2Controller *controller);
    ~OxoSynth2WinEditor() override;

#if VSTGUI_VERSION_MAJOR >= 4 && VSTGUI_VERSION_MINOR >= 1
    bool PLUGIN_API open(void *parent, const VSTGUI::PlatformType &platformType) override;
#else
    bool PLUGIN_API open(void *parent) override;
#endif
    void PLUGIN_API close() override;

    VSTGUI::CMessageResult notify(VSTGUI::CBaseObject *sender, const char *message) override;
    // Resizable: a grip in the bottom-right corner (and the host's own window
    // edges) scale the whole panel, keeping its proportions.
    Steinberg::tresult PLUGIN_API canResize() override { return Steinberg::kResultTrue; }
    Steinberg::tresult PLUGIN_API onSize(Steinberg::ViewRect *newSize) override;
    Steinberg::tresult PLUGIN_API checkSizeConstraint(Steinberg::ViewRect *rect) override;
    // Ask the host for a window `zoom` times the design size (clamped).
    void RequestScale(double zoom);

    OBJ_METHODS(OxoSynth2WinEditor, Steinberg::Vst::VSTGUIEditor)

private:
    struct Panel;
    OxoSynth2Controller *controller_ = nullptr;
    int baseW_ = 0, baseH_ = 0; // design size (zoom 1.0)
    std::unique_ptr<Panel> panel_;
};

} // namespace oxo

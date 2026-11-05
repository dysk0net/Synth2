#pragma once

// Windows editor: a VSTGUI panel (the SwiftUI panel in Shared/ is mac / iPad
// only). Same parameters, same patch library, same hooks into
// SynthEditorCore; the look is its own.

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
    Steinberg::tresult PLUGIN_API canResize() override { return Steinberg::kResultFalse; }

    OBJ_METHODS(OxoSynth2WinEditor, Steinberg::Vst::VSTGUIEditor)

private:
    struct Panel;
    OxoSynth2Controller *controller_ = nullptr;
    std::unique_ptr<Panel> panel_;
};

} // namespace oxo

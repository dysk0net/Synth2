#pragma once

// Per-user files under ~/Library/Application Support/<OXO_DATA_FOLDER>
// (on iPad: inside the extension's own container, $HOME is sandboxed).

#include "EditorCore/Identity.h"
#include "EngineCore/MidiLearn.hpp"
#include "EngineCore/PatchLibrary.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>
#else
#include <dlfcn.h>
#endif

namespace oxo::storage {

#if defined(_WIN32)
// Windows: settings in %APPDATA%\<OXO_DATA_FOLDER>, the bank library in
// <Documents>\<OXO_LIBRARY_FOLDER> (the per-user "Known Folders", so a
// redirected profile works), the factory bank next to the DLL.
inline std::filesystem::path KnownFolder(const KNOWNFOLDERID &id) {
    PWSTR raw = nullptr;
    std::filesystem::path out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &raw)) && raw) out = raw;
    if (raw) CoTaskMemFree(raw);
    return out;
}

inline std::filesystem::path Root() {
    return KnownFolder(FOLDERID_RoamingAppData) / std::filesystem::u8path(OXO_DATA_FOLDER);
}

inline std::filesystem::path LibraryRoot() {
    return KnownFolder(FOLDERID_Documents) / std::filesystem::u8path(OXO_LIBRARY_FOLDER);
}

// <Name>.vst3\Contents\x86_64-win\<Name>.vst3 -> <Name>.vst3\Contents\Resources\Synth2 Factory
// (the same layout as the mac bundle: binary dir's parent / Resources).
inline std::filesystem::path FactoryRoot() {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&FactoryRoot), &module))
        return {};
    wchar_t buf[MAX_PATH * 4];
    const DWORD n = GetModuleFileNameW(module, buf, DWORD(sizeof buf / sizeof buf[0]));
    if (n == 0 || n >= sizeof buf / sizeof buf[0]) return {};
    return std::filesystem::path(std::wstring(buf, n)).parent_path().parent_path() / "Resources" / s2::kFactoryBankName;
}
#else
inline std::filesystem::path Root() {
    const char *home = std::getenv("HOME");
    return std::filesystem::path(home ? home : "") / "Library/Application Support" / OXO_DATA_FOLDER;
}
#endif

#if defined(OXO_LIBRARY_FOLDER) && !defined(_WIN32)
// mac bank library: ~/Music/<OXO_LIBRARY_FOLDER> (Oxo Drums' "~/Music/Oxo
// Music/<product>" convention), the only folder read; banks get in by Import.
inline std::filesystem::path LibraryRoot() {
    const char *home = std::getenv("HOME");
    return std::filesystem::path(home ? home : "") / "Music" / OXO_LIBRARY_FOLDER;
}

// The factory bank: <this bundle>/Contents/Resources/Synth2 Factory, found
// from our own binary (the VST3, or the copy nested in the AUv2 component).
inline std::filesystem::path FactoryRoot() {
    Dl_info info{};
    if (!dladdr(reinterpret_cast<const void *>(&FactoryRoot), &info) || !info.dli_fname) return {};
    return std::filesystem::path(info.dli_fname).parent_path().parent_path() / "Resources" / s2::kFactoryBankName;
}
#endif

inline std::filesystem::path MidiLearnPath() { return Root() / "midi_mappings.json"; }

inline s2::MidiLearnTable LoadMidiLearn() {
    std::ifstream f(MidiLearnPath(), std::ios::binary);
    if (!f) return {};
    std::stringstream ss;
    ss << f.rdbuf();
    return s2::DecodeMidiLearn(ss.str());
}

inline bool SaveMidiLearn(const s2::MidiLearnTable &cc) {
    std::error_code ec;
    std::filesystem::create_directories(Root(), ec);
    const std::filesystem::path path = MidiLearnPath(), tmp = path.string() + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << s2::EncodeMidiLearn(cc);
        if (!f) return false;
    }
    std::filesystem::rename(tmp, path, ec);
    return !ec;
}

} // namespace oxo::storage

#include "Config.h"
#include "Diagnostics.h"
#include <Shlobj.h>
#include <cstdio>
#include <cstring>

#pragma comment(lib, "shell32.lib")

// Minimal JSON read/write — no external dependencies.
// Only handles the flat key=value structure of AppConfig.

std::wstring Config::GetConfigPath()
{
    wchar_t* appData = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData))) {
        std::wstring path(appData);
        CoTaskMemFree(appData);
        path += L"\\CKFlip3D";
        CreateDirectoryW(path.c_str(), nullptr);
        path += L"\\config.json";
        return path;
    }
    return L"config.json";
}

// ---------------------------------------------------------------------------
// Tiny helpers for reading JSON values from a flat { "key": value } file.
// ---------------------------------------------------------------------------
namespace {

std::string ReadFileToString(const std::wstring& path)
{
    FILE* f = nullptr;
    _wfopen_s(&f, path.c_str(), L"rb");
    if (!f) return {};
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 65536) { fclose(f); return {}; }
    std::string buf(static_cast<size_t>(sz), '\0');
    fread(&buf[0], 1, static_cast<size_t>(sz), f);
    fclose(f);
    return buf;
}

bool FindBool(const std::string& json, const char* key, bool defaultVal)
{
    std::string needle = std::string("\"") + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return defaultVal;
    pos = json.find(':', pos);
    if (pos == std::string::npos) return defaultVal;
    auto rest = json.substr(pos + 1, 20);
    if (rest.find("true") != std::string::npos) return true;
    if (rest.find("false") != std::string::npos) return false;
    return defaultVal;
}

int FindInt(const std::string& json, const char* key, int defaultVal)
{
    std::string needle = std::string("\"") + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return defaultVal;
    pos = json.find(':', pos);
    if (pos == std::string::npos) return defaultVal;
    // Skip whitespace
    pos++;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
    if (pos >= json.size()) return defaultVal;
    return atoi(json.c_str() + pos);
}

// Reads a quoted string value ("key": "value") handling \\ and \" escapes.
// Returns defaultVal if the key is missing or malformed.  UTF-8 → UTF-16.
std::wstring FindString(const std::string& json, const char* key,
                        const std::wstring& defaultVal)
{
    std::string needle = std::string("\"") + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return defaultVal;
    pos = json.find(':', pos);
    if (pos == std::string::npos) return defaultVal;
    pos = json.find('"', pos + 1);
    if (pos == std::string::npos) return defaultVal;
    pos++;

    std::string utf8;
    while (pos < json.size() && json[pos] != '"') {
        char c = json[pos];
        if (c == '\\' && pos + 1 < json.size()) {
            char next = json[pos + 1];
            if (next == '\\' || next == '"') { utf8 += next; pos += 2; continue; }
        }
        utf8 += c;
        pos++;
    }
    if (pos >= json.size()) return defaultVal;   // unterminated string

    if (utf8.empty()) return std::wstring();
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                                   static_cast<int>(utf8.size()), nullptr, 0);
    if (wlen <= 0) return defaultVal;
    std::wstring wide(static_cast<size_t>(wlen), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                        static_cast<int>(utf8.size()), &wide[0], wlen);
    return wide;
}

// UTF-16 → escaped UTF-8 for the flat JSON writer.
std::string EscapeUtf8(const std::wstring& wide)
{
    if (wide.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(),
                                  static_cast<int>(wide.size()),
                                  nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string utf8(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(),
                        static_cast<int>(wide.size()),
                        &utf8[0], len, nullptr, nullptr);

    std::string escaped;
    escaped.reserve(utf8.size());
    for (char c : utf8) {
        if (c == '\\' || c == '"') escaped += '\\';
        escaped += c;
    }
    return escaped;
}

} // namespace

// ---------------------------------------------------------------------------
AppConfig Config::Load()
{
    AppConfig cfg;
    std::wstring path = GetConfigPath();
    std::string json = ReadFileToString(path);
    if (json.empty()) {
        // First run — save defaults so the file exists.  A file that EXISTS
        // and still read as nothing is a different story: settings the user
        // made are being silently ignored, and they deserve to know which way
        // it went rather than watching their choices evaporate.
        if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES)
            Diag::Report(Diag::Code::ConfigUnreadable, Diag::Sev::Warning,
                         L"The settings file could not be read",
                         L"config.json exists but came back empty or too large to "
                         L"parse; CKFlip3D is running on defaults and will "
                         L"overwrite it when settings are next applied");
        Save(cfg);
        return cfg;
    }

    cfg.antialiasing  = FindBool(json, "antialiasing",  cfg.antialiasing);
    cfg.animations    = FindBool(json, "animations",    cfg.animations);
    cfg.animEntryExit = FindBool(json, "animEntryExit", cfg.animEntryExit);
    cfg.animCycle     = FindBool(json, "animCycle",     cfg.animCycle);
    cfg.animClose     = FindBool(json, "animClose",     cfg.animClose);
    cfg.animLabel     = FindBool(json, "animLabel",     cfg.animLabel);
    cfg.animHover     = FindBool(json, "animHover",     cfg.animHover);
    cfg.motionBlur    = FindBool(json, "motionBlur",    cfg.motionBlur);
    cfg.livePreview    = FindBool(json, "livePreview",    cfg.livePreview);
    cfg.liveBackground = FindBool(json, "liveBackground", cfg.liveBackground);
    cfg.vsyncLivePreview   = FindBool(json, "vsyncLivePreview",   cfg.vsyncLivePreview);
    cfg.taskbarLivePreview = FindBool(json, "taskbarLivePreview", cfg.taskbarLivePreview);
    cfg.taskbarPreview     = FindBool(json, "taskbarPreview",     cfg.taskbarPreview);
    cfg.maxWindows    = static_cast<uint32_t>(FindInt(json, "maxWindows", static_cast<int>(cfg.maxWindows)));
    cfg.visualPreset  = FindInt(json,  "visualPreset", cfg.visualPreset);
    cfg.reflections   = FindBool(json, "reflections",  cfg.reflections);
    cfg.backgroundOpacity = static_cast<uint32_t>(
        FindInt(json, "backgroundOpacity", static_cast<int>(cfg.backgroundOpacity)));
    cfg.backgroundBlur = static_cast<uint32_t>(
        FindInt(json, "backgroundBlur", static_cast<int>(cfg.backgroundBlur)));
    cfg.showDesktopTile    = FindBool(json, "showDesktopTile",    cfg.showDesktopTile);
    cfg.selectedLabel      = FindBool(json, "selectedLabel",      cfg.selectedLabel);
    cfg.selectedLabelTitle = FindBool(json, "selectedLabelTitle", cfg.selectedLabelTitle);
    cfg.selectedLabelIcon  = FindBool(json, "selectedLabelIcon",  cfg.selectedLabelIcon);
    cfg.selectedLabelBox   = FindBool(json, "selectedLabelBox",   cfg.selectedLabelBox);
    cfg.autoPerfTune  = FindBool(json, "autoPerfTune",  cfg.autoPerfTune);
    cfg.perfProfile   = FindInt(json,  "perfProfile",   cfg.perfProfile);
    cfg.startDelayMs  = static_cast<uint32_t>(
        FindInt(json, "startDelayMs", static_cast<int>(cfg.startDelayMs)));
    cfg.ignoreFullscreen = FindBool(json, "ignoreFullscreen", cfg.ignoreFullscreen);
    cfg.mouseWheelCycle  = FindBool(json, "mouseWheelCycle",  cfg.mouseWheelCycle);
    cfg.keyboardNav      = FindBool(json, "keyboardNav",      cfg.keyboardNav);
    cfg.ignoredApps      = FindString(json, "ignoredApps",    cfg.ignoredApps);
    cfg.excludedApps     = FindString(json, "excludedApps",   cfg.excludedApps);
    cfg.activationHotkey = FindString(json, "activationHotkey", cfg.activationHotkey);
    cfg.commitHotkey     = FindString(json, "commitHotkey",     cfg.commitHotkey);
    cfg.cancelHotkey     = FindString(json, "cancelHotkey",     cfg.cancelHotkey);
    cfg.closeHotkey      = FindString(json, "closeHotkey",      cfg.closeHotkey);
    cfg.hotkeyToggleMode = FindBool(json, "hotkeyToggleMode", cfg.hotkeyToggleMode);
    cfg.pointerInCascade   = FindBool(json, "pointerInCascade",   cfg.pointerInCascade);
    cfg.mouseSelect        = FindBool(json, "mouseSelect",        cfg.mouseSelect);
    cfg.mouseSelectButton  = FindInt(json,  "mouseSelectButton",  cfg.mouseSelectButton);
    cfg.mouseDragEnabled   = FindBool(json, "mouseDragEnabled",   cfg.mouseDragEnabled);
    cfg.mouseDragButton    = FindInt(json,  "mouseDragButton",    cfg.mouseDragButton);
    cfg.closeFromCascade   = FindBool(json, "closeFromCascade",   cfg.closeFromCascade);
    cfg.mouseCloseButton   = FindInt(json,  "mouseCloseButton",   cfg.mouseCloseButton);
    cfg.closeKeyEnabled    = FindBool(json, "closeKeyEnabled",    cfg.closeKeyEnabled);
    cfg.searchEnabled      = FindBool(json, "searchEnabled",      cfg.searchEnabled);
    cfg.searchBox          = FindBool(json, "searchBox",          cfg.searchBox);
    cfg.searchMatchProcess = FindBool(json, "searchMatchProcess", cfg.searchMatchProcess);
    cfg.searchPosX  = static_cast<uint32_t>(
        FindInt(json, "searchPosX",  static_cast<int>(cfg.searchPosX)));
    cfg.searchPosY  = static_cast<uint32_t>(
        FindInt(json, "searchPosY",  static_cast<int>(cfg.searchPosY)));
    cfg.searchScale = static_cast<uint32_t>(
        FindInt(json, "searchScale", static_cast<int>(cfg.searchScale)));
    cfg.touchpadNav             = FindBool(json, "touchpadNav",             cfg.touchpadNav);
    cfg.touchpadCycleFingers    = FindInt(json,  "touchpadCycleFingers",    cfg.touchpadCycleFingers);
    cfg.touchpadReverse         = FindBool(json, "touchpadReverse",         cfg.touchpadReverse);
    cfg.touchpadSensitivity     = FindInt(json,  "touchpadSensitivity",     cfg.touchpadSensitivity);
    cfg.touchpadActivateGesture = FindInt(json,  "touchpadActivateGesture", cfg.touchpadActivateGesture);
    cfg.touchpadCommitGesture   = FindInt(json,  "touchpadCommitGesture",   cfg.touchpadCommitGesture);
    cfg.touchpadSmoothing       = FindInt(json,  "touchpadSmoothing",       cfg.touchpadSmoothing);
    cfg.touchpadCancelSwipe     = FindBool(json, "touchpadCancelSwipe",     cfg.touchpadCancelSwipe);
    cfg.windowSnap              = FindBool(json, "windowSnap",              cfg.windowSnap);
    cfg.showDebugInfo = FindBool(json, "showDebugInfo", cfg.showDebugInfo);
    cfg.appTheme      = FindInt(json,  "appTheme",      cfg.appTheme);

    // A value outside its range is not a crime — it is clamped and the program
    // runs — but silently running on a number the user did not ask for is how
    // a hand-edited config turns into "this setting does nothing".  Recorded
    // once, naming the setting rather than the whole file.
    const AppConfig raw = cfg;
    auto clamped = [&](const wchar_t* name, long asked, long used) {
        wchar_t detail[192];
        _snwprintf_s(detail, _countof(detail), _TRUNCATE,
                     L"\"%s\" was %ld in config.json, which is outside the range "
                     L"this setting accepts; CKFlip3D is using %ld",
                     name, asked, used);
        Diag::Report(Diag::Code::ConfigValueClamped, Diag::Sev::Warning,
                     L"A setting in config.json was out of range", detail);
    };

    // Clamp
    if (cfg.appTheme < 0) cfg.appTheme = 0;
    if (cfg.appTheme > 4) cfg.appTheme = 4;
    if (cfg.visualPreset < 0) cfg.visualPreset = 0;
    if (cfg.visualPreset > 1) cfg.visualPreset = 1;
    if (cfg.maxWindows < 2)  cfg.maxWindows = 2;
    if (cfg.maxWindows > 10) cfg.maxWindows = 10;
    if (static_cast<int>(cfg.backgroundOpacity) < 0) cfg.backgroundOpacity = 0;
    if (cfg.backgroundOpacity > 100) cfg.backgroundOpacity = 100;
    if (static_cast<int>(cfg.backgroundBlur) < 0) cfg.backgroundBlur = 0;
    if (cfg.backgroundBlur > 100) cfg.backgroundBlur = 100;
    if (cfg.perfProfile < -1) cfg.perfProfile = -1;
    if (cfg.perfProfile > 2)  cfg.perfProfile = 2;
    // Two or four fingers only.  A config written before three-finger
    // gestures were dropped folds onto the nearest surviving choice instead
    // of leaving the user with a binding that never fires.
    cfg.touchpadCycleFingers = cfg.touchpadCycleFingers >= 4 ? 4 : 2;
    if (cfg.touchpadSensitivity < 1)   cfg.touchpadSensitivity = 1;
    if (cfg.touchpadSensitivity > 100) cfg.touchpadSensitivity = 100;
    if (cfg.touchpadActivateGesture < 0) cfg.touchpadActivateGesture = 0;
    if (cfg.touchpadActivateGesture > 4) cfg.touchpadActivateGesture = 4;
    if (cfg.touchpadCommitGesture < 0) cfg.touchpadCommitGesture = 0;
    if (cfg.touchpadCommitGesture > 3) cfg.touchpadCommitGesture = 3;  // was 4 = three down
    if (cfg.touchpadSmoothing < 0)   cfg.touchpadSmoothing = 0;
    if (cfg.touchpadSmoothing > 100) cfg.touchpadSmoothing = 100;
    if (static_cast<int>(cfg.startDelayMs) < 1) cfg.startDelayMs = 1;
    if (cfg.startDelayMs > 1000) cfg.startDelayMs = 1000;
    // Mouse-button identifiers: 0 off, 1 left, 2 right, 3 middle, 4 X1, 5 X2.
    if (cfg.mouseSelectButton < 0 || cfg.mouseSelectButton > 5) cfg.mouseSelectButton = 1;
    if (cfg.mouseDragButton   < 0 || cfg.mouseDragButton   > 5) cfg.mouseDragButton   = 2;
    if (cfg.mouseCloseButton  < 0 || cfg.mouseCloseButton  > 5) cfg.mouseCloseButton  = 3;
    if (cfg.commitHotkey.empty()) cfg.commitHotkey = L"Enter";
    if (cfg.cancelHotkey.empty()) cfg.cancelHotkey = L"Escape";
    if (cfg.closeHotkey.empty())  cfg.closeHotkey  = L"Delete";
    if (cfg.searchPosX > 100) cfg.searchPosX = 100;
    if (cfg.searchPosY > 100) cfg.searchPosY = 100;
    if (cfg.searchScale < 50)  cfg.searchScale = 50;
    if (cfg.searchScale > 200) cfg.searchScale = 200;

    // Report the clamps that actually moved something.  Only the ranges a
    // hand-edited or hand-migrated file realistically gets wrong — the rest
    // would be noise about numbers nobody types.
    if (raw.maxWindows != cfg.maxWindows)
        clamped(L"maxWindows", static_cast<long>(raw.maxWindows),
                static_cast<long>(cfg.maxWindows));
    if (raw.backgroundOpacity != cfg.backgroundOpacity)
        clamped(L"backgroundOpacity", static_cast<long>(raw.backgroundOpacity),
                static_cast<long>(cfg.backgroundOpacity));
    if (raw.backgroundBlur != cfg.backgroundBlur)
        clamped(L"backgroundBlur", static_cast<long>(raw.backgroundBlur),
                static_cast<long>(cfg.backgroundBlur));
    if (raw.startDelayMs != cfg.startDelayMs)
        clamped(L"startDelayMs", static_cast<long>(raw.startDelayMs),
                static_cast<long>(cfg.startDelayMs));
    if (raw.touchpadCycleFingers != cfg.touchpadCycleFingers)
        clamped(L"touchpadCycleFingers", raw.touchpadCycleFingers,
                cfg.touchpadCycleFingers);

    // Combinations the Settings app refuses to save, which say nothing about
    // how the file got this way — an older build wrote it, or it was edited by
    // hand — but everything about why a feature that is switched ON does
    // nothing at all.
    if (cfg.searchEnabled && !cfg.hotkeyToggleMode
        && cfg.activationHotkey.find(L'+') != std::wstring::npos)
        Diag::Report(Diag::Code::ConfigConflict, Diag::Sev::Warning,
                     L"Search is on, but the cascade cannot stay open long enough to type",
                     L"\"searchEnabled\" needs \"hotkeyToggleMode\": releasing a "
                     L"hotkey with a modifier commits immediately, so the rest of "
                     L"the word reaches Windows as shortcuts");
    if (!cfg.windowSnap && !cfg.pointerInCascade)
        Diag::Report(Diag::Code::ConfigConflict, Diag::Sev::Warning,
                     L"Free stack movement is on, but the mouse is kept out of the cascade",
                     L"\"windowSnap\": false is the drag button's feature, and that "
                     L"button lives behind \"pointerInCascade\"; only a touchpad "
                     L"swipe can reach it as things stand");

    return cfg;
}

// ---------------------------------------------------------------------------
void Config::Save(const AppConfig& cfg)
{
    std::wstring path = GetConfigPath();
    // Write to a temp file in the same directory, then atomically swap it
    // in — a concurrent reader (reload broadcast, Settings app) sees either
    // the complete old file or the complete new one, never a truncated mix.
    std::wstring tmp = path + L".tmp";
    FILE* f = nullptr;
    _wfopen_s(&f, tmp.c_str(), L"wb");
    if (!f) {
        Diag::Report(Diag::Code::ConfigSaveFailed, Diag::Sev::Warning,
                     L"Settings could not be saved",
                     L"config.json could not be written — check that "
                     L"%APPDATA%\\CKFlip3D is writable and not held open by "
                     L"another program");
        return;
    }

    fprintf(f, "{\n");
    fprintf(f, "  \"antialiasing\": %s,\n",  cfg.antialiasing ? "true" : "false");
    fprintf(f, "  \"animations\": %s,\n",    cfg.animations   ? "true" : "false");
    fprintf(f, "  \"animEntryExit\": %s,\n", cfg.animEntryExit ? "true" : "false");
    fprintf(f, "  \"animCycle\": %s,\n",     cfg.animCycle     ? "true" : "false");
    fprintf(f, "  \"animClose\": %s,\n",     cfg.animClose     ? "true" : "false");
    fprintf(f, "  \"animLabel\": %s,\n",     cfg.animLabel     ? "true" : "false");
    fprintf(f, "  \"animHover\": %s,\n",     cfg.animHover     ? "true" : "false");
    fprintf(f, "  \"motionBlur\": %s,\n",    cfg.motionBlur   ? "true" : "false");
    fprintf(f, "  \"livePreview\": %s,\n",   cfg.livePreview  ? "true" : "false");
    fprintf(f, "  \"liveBackground\": %s,\n", cfg.liveBackground ? "true" : "false");
    fprintf(f, "  \"vsyncLivePreview\": %s,\n",   cfg.vsyncLivePreview   ? "true" : "false");
    fprintf(f, "  \"taskbarLivePreview\": %s,\n", cfg.taskbarLivePreview ? "true" : "false");
    fprintf(f, "  \"taskbarPreview\": %s,\n",     cfg.taskbarPreview     ? "true" : "false");
    fprintf(f, "  \"maxWindows\": %u,\n",    cfg.maxWindows);
    fprintf(f, "  \"visualPreset\": %d,\n",  cfg.visualPreset);
    fprintf(f, "  \"reflections\": %s,\n",   cfg.reflections ? "true" : "false");
    fprintf(f, "  \"backgroundOpacity\": %u,\n", cfg.backgroundOpacity);
    fprintf(f, "  \"backgroundBlur\": %u,\n",    cfg.backgroundBlur);
    fprintf(f, "  \"showDesktopTile\": %s,\n",    cfg.showDesktopTile    ? "true" : "false");
    fprintf(f, "  \"selectedLabel\": %s,\n",      cfg.selectedLabel      ? "true" : "false");
    fprintf(f, "  \"selectedLabelTitle\": %s,\n", cfg.selectedLabelTitle ? "true" : "false");
    fprintf(f, "  \"selectedLabelIcon\": %s,\n",  cfg.selectedLabelIcon  ? "true" : "false");
    fprintf(f, "  \"selectedLabelBox\": %s,\n",   cfg.selectedLabelBox   ? "true" : "false");
    fprintf(f, "  \"autoPerfTune\": %s,\n",  cfg.autoPerfTune ? "true" : "false");
    fprintf(f, "  \"perfProfile\": %d,\n",   cfg.perfProfile);
    fprintf(f, "  \"startDelayMs\": %u,\n",  cfg.startDelayMs);
    fprintf(f, "  \"ignoreFullscreen\": %s,\n", cfg.ignoreFullscreen ? "true" : "false");
    fprintf(f, "  \"mouseWheelCycle\": %s,\n",  cfg.mouseWheelCycle  ? "true" : "false");
    fprintf(f, "  \"keyboardNav\": %s,\n",      cfg.keyboardNav      ? "true" : "false");
    fprintf(f, "  \"ignoredApps\": \"%s\",\n",  EscapeUtf8(cfg.ignoredApps).c_str());
    fprintf(f, "  \"excludedApps\": \"%s\",\n", EscapeUtf8(cfg.excludedApps).c_str());
    fprintf(f, "  \"activationHotkey\": \"%s\",\n", EscapeUtf8(cfg.activationHotkey).c_str());
    fprintf(f, "  \"commitHotkey\": \"%s\",\n",     EscapeUtf8(cfg.commitHotkey).c_str());
    fprintf(f, "  \"cancelHotkey\": \"%s\",\n",     EscapeUtf8(cfg.cancelHotkey).c_str());
    fprintf(f, "  \"closeHotkey\": \"%s\",\n",      EscapeUtf8(cfg.closeHotkey).c_str());
    fprintf(f, "  \"hotkeyToggleMode\": %s,\n", cfg.hotkeyToggleMode ? "true" : "false");
    fprintf(f, "  \"pointerInCascade\": %s,\n",   cfg.pointerInCascade  ? "true" : "false");
    fprintf(f, "  \"mouseSelect\": %s,\n",        cfg.mouseSelect       ? "true" : "false");
    fprintf(f, "  \"mouseSelectButton\": %d,\n",  cfg.mouseSelectButton);
    fprintf(f, "  \"mouseDragEnabled\": %s,\n",   cfg.mouseDragEnabled  ? "true" : "false");
    fprintf(f, "  \"mouseDragButton\": %d,\n",    cfg.mouseDragButton);
    fprintf(f, "  \"closeFromCascade\": %s,\n",   cfg.closeFromCascade  ? "true" : "false");
    fprintf(f, "  \"mouseCloseButton\": %d,\n",   cfg.mouseCloseButton);
    fprintf(f, "  \"closeKeyEnabled\": %s,\n",    cfg.closeKeyEnabled   ? "true" : "false");
    fprintf(f, "  \"searchEnabled\": %s,\n",      cfg.searchEnabled      ? "true" : "false");
    fprintf(f, "  \"searchBox\": %s,\n",          cfg.searchBox          ? "true" : "false");
    fprintf(f, "  \"searchMatchProcess\": %s,\n", cfg.searchMatchProcess ? "true" : "false");
    fprintf(f, "  \"searchPosX\": %u,\n",         cfg.searchPosX);
    fprintf(f, "  \"searchPosY\": %u,\n",         cfg.searchPosY);
    fprintf(f, "  \"searchScale\": %u,\n",        cfg.searchScale);
    fprintf(f, "  \"touchpadNav\": %s,\n",             cfg.touchpadNav             ? "true" : "false");
    fprintf(f, "  \"touchpadCycleFingers\": %d,\n",    cfg.touchpadCycleFingers);
    fprintf(f, "  \"touchpadReverse\": %s,\n",         cfg.touchpadReverse         ? "true" : "false");
    fprintf(f, "  \"touchpadSensitivity\": %d,\n",     cfg.touchpadSensitivity);
    fprintf(f, "  \"touchpadActivateGesture\": %d,\n", cfg.touchpadActivateGesture);
    fprintf(f, "  \"touchpadCommitGesture\": %d,\n",   cfg.touchpadCommitGesture);
    fprintf(f, "  \"touchpadSmoothing\": %d,\n",       cfg.touchpadSmoothing);
    fprintf(f, "  \"touchpadCancelSwipe\": %s,\n",     cfg.touchpadCancelSwipe     ? "true" : "false");
    fprintf(f, "  \"windowSnap\": %s,\n",              cfg.windowSnap              ? "true" : "false");
    fprintf(f, "  \"showDebugInfo\": %s,\n",  cfg.showDebugInfo ? "true" : "false");
    // Settings-app-owned key — persisted here too so a core-side save
    // never drops the user's theme choice.
    fprintf(f, "  \"appTheme\": %d\n",       cfg.appTheme);
    fprintf(f, "}\n");
    fclose(f);

    if (!MoveFileExW(tmp.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        DeleteFileW(tmp.c_str());
}

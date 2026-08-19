// ---------------------------------------------------------------------------
// Reading and writing config.json.  A hand-rolled parser, deliberately: the
// file is a flat set of key/value pairs this program writes itself, and a JSON
// library would be a dependency the core does not otherwise need.
//
// Copyright © 2026 Karol Cymerman (CYMERKAROL) — https://github.com/CYMERKAROL/CKFlip3D
// ---------------------------------------------------------------------------
#include "Config.h"
#include "Diagnostics.h"
#include <Shlobj.h>
#include <cstdio>
#include <cstring>
#include <cwctype>

#pragma comment(lib, "shell32.lib")

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

/// The last '+'-separated token of a binding — its main key — or an empty
/// string when that token cannot serve as a navigation key.
///
/// Bare modifiers and mouse buttons are refused: neither can be an entry in a
/// navigation list (the hook keeps its own re-press cycling for them), and
/// seeding one would only produce an entry the parser later drops.
std::wstring NavTokenOfBinding(const std::wstring& combo)
{
    size_t plus = combo.find_last_of(L'+');
    std::wstring tok = (plus == std::wstring::npos) ? combo : combo.substr(plus + 1);

    size_t b = tok.find_first_not_of(L" \t");
    size_t e = tok.find_last_not_of(L" \t");
    if (b == std::wstring::npos) return {};
    tok = tok.substr(b, e - b + 1);

    std::wstring lower;
    lower.reserve(tok.size());
    for (wchar_t c : tok) lower += static_cast<wchar_t>(towlower(c));

    static const wchar_t* kNotKeys[] = {
        L"ctrl", L"control", L"shift", L"alt", L"win", L"windows", L"super",
        L"meta", L"lbutton", L"rbutton", L"mbutton", L"middlebutton",
        L"xbutton1", L"xbutton2", L"mouse4", L"mouse5",
    };
    for (const wchar_t* bad : kNotKeys)
        if (lower == bad) return {};

    return tok;
}

/// Switch every entry of a ';'-separated binding list OFF, keeping the entries
/// themselves ('!' prefix — see AppConfig::navForwardKeys).  Entries that are
/// already parked are left as they are rather than gaining a second '!'.
std::wstring ParkNavKeys(const std::wstring& list)
{
    std::wstring out;
    size_t start = 0;
    while (start <= list.size()) {
        size_t end = list.find(L';', start);
        if (end == std::wstring::npos) end = list.size();

        std::wstring tok = list.substr(start, end - start);
        size_t b = tok.find_first_not_of(L" \t");
        size_t e = tok.find_last_not_of(L" \t");
        tok = (b == std::wstring::npos) ? std::wstring() : tok.substr(b, e - b + 1);

        if (!tok.empty()) {
            if (!out.empty()) out += L';';
            if (tok[0] != L'!') out += L'!';
            out += tok;
        }

        if (end == list.size()) break;
        start = end + 1;
    }
    return out;
}

/// Does this ';'-separated binding list carry at least one entry that is not
/// parked?  "Delete" does, "!Delete" does not, and neither does "".
bool HasLiveEntry(const std::wstring& list)
{
    size_t start = 0;
    while (start <= list.size()) {
        size_t end = list.find(L';', start);
        if (end == std::wstring::npos) end = list.size();

        std::wstring tok = list.substr(start, end - start);
        size_t b = tok.find_first_not_of(L" \t");
        if (b != std::wstring::npos && tok[b] != L'!')
            return true;

        if (end == list.size()) break;
        start = end + 1;
    }
    return false;
}

} // namespace

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
    cfg.ignoredApps      = FindString(json, "ignoredApps",    cfg.ignoredApps);
    cfg.excludedApps     = FindString(json, "excludedApps",   cfg.excludedApps);
    cfg.activationHotkey = FindString(json, "activationHotkey", cfg.activationHotkey);
    cfg.hotkeyToggleMode = FindBool(json, "hotkeyToggleMode", cfg.hotkeyToggleMode);

    // ---- Commit / cancel / close: one key each, until Build 3 --------------
    // Same migration shape as the navigation lists below: read the list, and
    // when the file has none, seed it from the single key that file DOES carry
    // so an update changes nothing the user can feel.  Only when the key is
    // ABSENT — once the lists have been written they are the truth.
    {
        auto seeded = [&](const char* listKey, const char* legacyKey,
                          std::wstring& target) {
            const bool hadList =
                json.find(std::string("\"") + listKey + "\"") != std::string::npos;
            target = FindString(json, listKey, target);
            if (!hadList)
                target = FindString(json, legacyKey, target);
            return hadList;
        };

        seeded("commitKeys", "commitHotkey", cfg.commitKeys);
        seeded("cancelKeys", "cancelHotkey", cfg.cancelKeys);

        // The close key's old master switch (`closeKeyEnabled`) said exactly
        // what an empty — or wholly parked — list says, so it is gone the way
        // `keyboardNav` went.  A file that still carries it OFF meant "no close
        // key", and that has to keep meaning that: park the entry rather than
        // handing back a Delete somebody switched off.  Parked and not deleted,
        // so the Settings page shows it ready to come back.
        if (!seeded("closeKeys", "closeHotkey", cfg.closeKeys)
            && !FindBool(json, "closeKeyEnabled", true))
            cfg.closeKeys = ParkNavKeys(cfg.closeKeys);
    }

    // ---- Navigation keys, and the 1.5 file that has none of them ----------
    // Read AFTER activationHotkey, because a file written before 1.6 carries no
    // navigation lists at all and the defaults alone would not describe it:
    // back then the ACTIVATION key cycled the stack as a side effect of opening
    // it, whatever that key happened to be.  So the lists are seeded from THIS
    // file's hotkey — Win+Tab gives the shipped Tab, Ctrl+Alt+F gives F — and
    // someone updating finds the switcher behaving exactly as it did, with
    // every key now listed and removable.
    //
    // Seeded only when the key is ABSENT.  Once 1.6 has written the lists they
    // are the truth, changes of hotkey included: a key the user took off the
    // list must not come back because they later rebound the hotkey to it.
    {
        const bool hadNavKeys = json.find("\"navForwardKeys\"") != std::string::npos;
        cfg.navForwardKeys = FindString(json, "navForwardKeys", cfg.navForwardKeys);
        cfg.navBackKeys    = FindString(json, "navBackKeys",    cfg.navBackKeys);
        if (!hadNavKeys) {
            const std::wstring main = NavTokenOfBinding(cfg.activationHotkey);
            // Empty when the hotkey is a bare modifier or a mouse button; those
            // keep cycling through the hook's own re-press path, so the lists
            // get the arrows alone rather than an entry that cannot work.
            cfg.navForwardKeys = main.empty() ? L"Down;Right" : main + L";Down;Right";
            cfg.navBackKeys    = main.empty() ? L"Up;Left" : L"Shift+" + main + L";Up;Left";
        }

        // Legacy `keyboardNav` (1.5 and earlier): one switch over the four
        // hard-wired arrows.  1.6 replaced it with the two lists, where every
        // key can be removed or parked on its own — so the old switch had
        // become a second way to say something the lists already say.  A file
        // that still carries it OFF meant "no navigation keys", and that has to
        // keep meaning that: park every entry rather than handing somebody back
        // arrows they switched off.  Parked, not deleted, so the Navigation
        // keys page shows them ready to be switched back on.
        if (!FindBool(json, "keyboardNav", true)) {
            cfg.navForwardKeys = ParkNavKeys(cfg.navForwardKeys);
            cfg.navBackKeys    = ParkNavKeys(cfg.navBackKeys);
        }
    }
    cfg.pointerInCascade   = FindBool(json, "pointerInCascade",   cfg.pointerInCascade);
    cfg.mouseSelect        = FindBool(json, "mouseSelect",        cfg.mouseSelect);
    cfg.mouseSelectButton  = FindInt(json,  "mouseSelectButton",  cfg.mouseSelectButton);
    cfg.mouseDragEnabled   = FindBool(json, "mouseDragEnabled",   cfg.mouseDragEnabled);
    cfg.mouseDragButton    = FindInt(json,  "mouseDragButton",    cfg.mouseDragButton);
    cfg.closeFromCascade   = FindBool(json, "closeFromCascade",   cfg.closeFromCascade);
    cfg.mouseCloseButton   = FindInt(json,  "mouseCloseButton",   cfg.mouseCloseButton);
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
    cfg.touchpadReverse         = FindBool(json, "touchpadReverse",         cfg.touchpadReverse);
    cfg.touchpadSensitivity     = FindInt(json,  "touchpadSensitivity",     cfg.touchpadSensitivity);
    cfg.touchpadSmoothing       = FindInt(json,  "touchpadSmoothing",       cfg.touchpadSmoothing);
    cfg.touchpadCancelSwipe     = FindBool(json, "touchpadCancelSwipe",     cfg.touchpadCancelSwipe);
    cfg.touchpadContinuous      = FindBool(json, "touchpadContinuous",      cfg.touchpadContinuous);
    cfg.windowSnap              = FindBool(json, "windowSnap",              cfg.windowSnap);

    // ---- Touchpad gestures: one apiece, until Build 3 ----------------------
    // Each list replaces a single integer, and each integer's 0 meant "off" —
    // which is what an empty list says now.  Seeded from the integer only when
    // the list is absent, exactly like the key lists above.
    {
        auto hasKey = [&](const char* key) {
            return json.find(std::string("\"") + key + "\"") != std::string::npos;
        };

        if (hasKey("touchpadActivateGestures")) {
            cfg.touchpadActivateGestures =
                FindString(json, "touchpadActivateGestures", cfg.touchpadActivateGestures);
        } else {
            switch (FindInt(json, "touchpadActivateGesture", 1)) {
            case 0:  cfg.touchpadActivateGestures = L"";              break;
            case 2:  cfg.touchpadActivateGestures = L"TwoDownLeft";   break;
            case 3:  cfg.touchpadActivateGestures = L"FourDownRight"; break;
            case 4:  cfg.touchpadActivateGestures = L"FourDownLeft";  break;
            default: cfg.touchpadActivateGestures = L"TwoDownRight";  break;
            }
        }

        // Two or four fingers only.  A config written before three-finger
        // gestures were dropped folds onto the nearest surviving choice
        // instead of leaving the user with a binding that never fires.
        if (hasKey("touchpadCycleGestures")) {
            cfg.touchpadCycleGestures =
                FindString(json, "touchpadCycleGestures", cfg.touchpadCycleGestures);
        } else {
            cfg.touchpadCycleGestures =
                FindInt(json, "touchpadCycleFingers", 2) >= 4 ? L"FourSwipe" : L"TwoSwipe";
        }

        if (hasKey("touchpadCommitGestures")) {
            cfg.touchpadCommitGestures =
                FindString(json, "touchpadCommitGestures", cfg.touchpadCommitGestures);
        } else {
            switch (FindInt(json, "touchpadCommitGesture", 1)) {
            case 0:  cfg.touchpadCommitGestures = L"";        break;
            case 2:  cfg.touchpadCommitGestures = L"TwoTap";  break;
            case 3:  cfg.touchpadCommitGestures = L"TwoDown"; break;
            default: cfg.touchpadCommitGestures = L"OneTap";  break;
            }
        }
    }
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
    if (cfg.touchpadSensitivity < 1)   cfg.touchpadSensitivity = 1;
    if (cfg.touchpadSensitivity > 100) cfg.touchpadSensitivity = 100;
    if (cfg.touchpadSmoothing < 0)   cfg.touchpadSmoothing = 0;
    if (cfg.touchpadSmoothing > 100) cfg.touchpadSmoothing = 100;
    if (static_cast<int>(cfg.startDelayMs) < 1) cfg.startDelayMs = 1;
    if (cfg.startDelayMs > 1000) cfg.startDelayMs = 1000;
    // Mouse-button identifiers: 0 off, 1 left, 2 right, 3 middle, 4 X1, 5 X2.
    if (cfg.mouseSelectButton < 0 || cfg.mouseSelectButton > 5) cfg.mouseSelectButton = 1;
    if (cfg.mouseDragButton   < 0 || cfg.mouseDragButton   > 5) cfg.mouseDragButton   = 2;
    if (cfg.mouseCloseButton  < 0 || cfg.mouseCloseButton  > 5) cfg.mouseCloseButton  = 3;
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
    // A cascade with neither a commit nor a cancel key can only be closed with
    // the mouse or the touchpad — and with those off too, not at all.  The
    // Settings page keeps one live entry on each list, so a file that says
    // otherwise was hand-edited and its author deserves to hear about it.
    if (!HasLiveEntry(cfg.commitKeys) && !HasLiveEntry(cfg.cancelKeys))
        Diag::Report(Diag::Code::ConfigConflict, Diag::Sev::Warning,
                     L"No key can close the cascade",
                     L"\"commitKeys\" and \"cancelKeys\" are both empty (or wholly "
                     L"switched off), so once the cascade is open only the mouse "
                     L"and the touchpad can end the session");

    if (!cfg.windowSnap && !cfg.pointerInCascade)
        Diag::Report(Diag::Code::ConfigConflict, Diag::Sev::Warning,
                     L"Free stack movement is on, but the mouse is kept out of the cascade",
                     L"\"windowSnap\": false is the drag button's feature, and that "
                     L"button lives behind \"pointerInCascade\"; only a touchpad "
                     L"swipe can reach it as things stand");

    return cfg;
}

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
    fprintf(f, "  \"navForwardKeys\": \"%s\",\n", EscapeUtf8(cfg.navForwardKeys).c_str());
    fprintf(f, "  \"navBackKeys\": \"%s\",\n",    EscapeUtf8(cfg.navBackKeys).c_str());
    fprintf(f, "  \"ignoredApps\": \"%s\",\n",  EscapeUtf8(cfg.ignoredApps).c_str());
    fprintf(f, "  \"excludedApps\": \"%s\",\n", EscapeUtf8(cfg.excludedApps).c_str());
    fprintf(f, "  \"activationHotkey\": \"%s\",\n", EscapeUtf8(cfg.activationHotkey).c_str());
    fprintf(f, "  \"commitKeys\": \"%s\",\n",       EscapeUtf8(cfg.commitKeys).c_str());
    fprintf(f, "  \"cancelKeys\": \"%s\",\n",       EscapeUtf8(cfg.cancelKeys).c_str());
    fprintf(f, "  \"closeKeys\": \"%s\",\n",        EscapeUtf8(cfg.closeKeys).c_str());
    fprintf(f, "  \"hotkeyToggleMode\": %s,\n", cfg.hotkeyToggleMode ? "true" : "false");
    fprintf(f, "  \"pointerInCascade\": %s,\n",   cfg.pointerInCascade  ? "true" : "false");
    fprintf(f, "  \"mouseSelect\": %s,\n",        cfg.mouseSelect       ? "true" : "false");
    fprintf(f, "  \"mouseSelectButton\": %d,\n",  cfg.mouseSelectButton);
    fprintf(f, "  \"mouseDragEnabled\": %s,\n",   cfg.mouseDragEnabled  ? "true" : "false");
    fprintf(f, "  \"mouseDragButton\": %d,\n",    cfg.mouseDragButton);
    fprintf(f, "  \"closeFromCascade\": %s,\n",   cfg.closeFromCascade  ? "true" : "false");
    fprintf(f, "  \"mouseCloseButton\": %d,\n",   cfg.mouseCloseButton);
    fprintf(f, "  \"searchEnabled\": %s,\n",      cfg.searchEnabled      ? "true" : "false");
    fprintf(f, "  \"searchBox\": %s,\n",          cfg.searchBox          ? "true" : "false");
    fprintf(f, "  \"searchMatchProcess\": %s,\n", cfg.searchMatchProcess ? "true" : "false");
    fprintf(f, "  \"searchPosX\": %u,\n",         cfg.searchPosX);
    fprintf(f, "  \"searchPosY\": %u,\n",         cfg.searchPosY);
    fprintf(f, "  \"searchScale\": %u,\n",        cfg.searchScale);
    fprintf(f, "  \"touchpadNav\": %s,\n",             cfg.touchpadNav             ? "true" : "false");
    fprintf(f, "  \"touchpadActivateGestures\": \"%s\",\n",
            EscapeUtf8(cfg.touchpadActivateGestures).c_str());
    fprintf(f, "  \"touchpadCycleGestures\": \"%s\",\n",
            EscapeUtf8(cfg.touchpadCycleGestures).c_str());
    fprintf(f, "  \"touchpadCommitGestures\": \"%s\",\n",
            EscapeUtf8(cfg.touchpadCommitGestures).c_str());
    fprintf(f, "  \"touchpadReverse\": %s,\n",         cfg.touchpadReverse         ? "true" : "false");
    fprintf(f, "  \"touchpadSensitivity\": %d,\n",     cfg.touchpadSensitivity);
    fprintf(f, "  \"touchpadSmoothing\": %d,\n",       cfg.touchpadSmoothing);
    fprintf(f, "  \"touchpadCancelSwipe\": %s,\n",     cfg.touchpadCancelSwipe     ? "true" : "false");
    fprintf(f, "  \"touchpadContinuous\": %s,\n",      cfg.touchpadContinuous      ? "true" : "false");
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

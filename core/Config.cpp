#include "Config.h"
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
        // First run — save defaults so the file exists.
        Save(cfg);
        return cfg;
    }

    cfg.antialiasing  = FindBool(json, "antialiasing",  cfg.antialiasing);
    cfg.animations    = FindBool(json, "animations",    cfg.animations);
    cfg.animEntryExit = FindBool(json, "animEntryExit", cfg.animEntryExit);
    cfg.animCycle     = FindBool(json, "animCycle",     cfg.animCycle);
    cfg.animClose     = FindBool(json, "animClose",     cfg.animClose);
    cfg.animLabel     = FindBool(json, "animLabel",     cfg.animLabel);
    cfg.motionBlur    = FindBool(json, "motionBlur",    cfg.motionBlur);
    cfg.livePreview    = FindBool(json, "livePreview",    cfg.livePreview);
    cfg.liveBackground = FindBool(json, "liveBackground", cfg.liveBackground);
    cfg.vsyncLivePreview   = FindBool(json, "vsyncLivePreview",   cfg.vsyncLivePreview);
    cfg.taskbarLivePreview = FindBool(json, "taskbarLivePreview", cfg.taskbarLivePreview);
    cfg.taskbarPreview     = FindBool(json, "taskbarPreview",     cfg.taskbarPreview);
    cfg.maxWindows    = static_cast<uint32_t>(FindInt(json, "maxWindows", static_cast<int>(cfg.maxWindows)));
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
    cfg.hotkeyToggleMode = FindBool(json, "hotkeyToggleMode", cfg.hotkeyToggleMode);
    cfg.showDebugInfo = FindBool(json, "showDebugInfo", cfg.showDebugInfo);
    cfg.appTheme      = FindInt(json,  "appTheme",      cfg.appTheme);

    // Clamp
    if (cfg.appTheme < 0) cfg.appTheme = 0;
    if (cfg.appTheme > 4) cfg.appTheme = 4;
    if (cfg.maxWindows < 2)  cfg.maxWindows = 2;
    if (cfg.maxWindows > 10) cfg.maxWindows = 10;
    if (static_cast<int>(cfg.backgroundOpacity) < 0) cfg.backgroundOpacity = 0;
    if (cfg.backgroundOpacity > 100) cfg.backgroundOpacity = 100;
    if (static_cast<int>(cfg.backgroundBlur) < 0) cfg.backgroundBlur = 0;
    if (cfg.backgroundBlur > 100) cfg.backgroundBlur = 100;
    if (cfg.perfProfile < -1) cfg.perfProfile = -1;
    if (cfg.perfProfile > 2)  cfg.perfProfile = 2;
    if (static_cast<int>(cfg.startDelayMs) < 1) cfg.startDelayMs = 1;
    if (cfg.startDelayMs > 1000) cfg.startDelayMs = 1000;

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
    if (!f) return;

    fprintf(f, "{\n");
    fprintf(f, "  \"antialiasing\": %s,\n",  cfg.antialiasing ? "true" : "false");
    fprintf(f, "  \"animations\": %s,\n",    cfg.animations   ? "true" : "false");
    fprintf(f, "  \"animEntryExit\": %s,\n", cfg.animEntryExit ? "true" : "false");
    fprintf(f, "  \"animCycle\": %s,\n",     cfg.animCycle     ? "true" : "false");
    fprintf(f, "  \"animClose\": %s,\n",     cfg.animClose     ? "true" : "false");
    fprintf(f, "  \"animLabel\": %s,\n",     cfg.animLabel     ? "true" : "false");
    fprintf(f, "  \"motionBlur\": %s,\n",    cfg.motionBlur   ? "true" : "false");
    fprintf(f, "  \"livePreview\": %s,\n",   cfg.livePreview  ? "true" : "false");
    fprintf(f, "  \"liveBackground\": %s,\n", cfg.liveBackground ? "true" : "false");
    fprintf(f, "  \"vsyncLivePreview\": %s,\n",   cfg.vsyncLivePreview   ? "true" : "false");
    fprintf(f, "  \"taskbarLivePreview\": %s,\n", cfg.taskbarLivePreview ? "true" : "false");
    fprintf(f, "  \"taskbarPreview\": %s,\n",     cfg.taskbarPreview     ? "true" : "false");
    fprintf(f, "  \"maxWindows\": %u,\n",    cfg.maxWindows);
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
    fprintf(f, "  \"hotkeyToggleMode\": %s,\n", cfg.hotkeyToggleMode ? "true" : "false");
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

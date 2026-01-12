/**
 * Configuration Manager Implementation
 */

#include "config.h"
#include "logger.h"

#include <Windows.h>
#include <cstdio>

namespace FiveMFrameGen {
namespace Utils {

ConfigManager::ConfigManager(const char* filename) {
    // Build path in FiveM plugins directory
    char path[MAX_PATH];
    char* appData = nullptr;
    size_t len = 0;
    _dupenv_s(&appData, &len, "LOCALAPPDATA");
    
    if (appData) {
        snprintf(path, MAX_PATH, "%s\\FiveM\\FiveM.app\\plugins\\%s", appData, filename);
        free(appData);
    } else {
        snprintf(path, MAX_PATH, "%s", filename);
    }
    
    m_Path = path;
}

Config ConfigManager::Load() {
    Config config;
    
    Logger::Info("Loading configuration from: %s", m_Path.c_str());
    
    config.enabled = ReadBool("General", "Enabled", false);
    config.backend = static_cast<Backend>(ReadInt("General", "Backend", 1));
    config.quality = static_cast<QualityPreset>(ReadInt("General", "Quality", 1));
    config.targetFramerate = ReadFloat("General", "TargetFramerate", 60.0f);
    config.showOverlay = ReadBool("General", "ShowOverlay", true);
    config.hudLessMode = ReadBool("General", "HudLessMode", false);
    config.sharpness = ReadFloat("General", "Sharpness", 0.5f);
    
    // Validate
    if (config.sharpness < 0.0f) config.sharpness = 0.0f;
    if (config.sharpness > 1.0f) config.sharpness = 1.0f;
    
    if (static_cast<int>(config.backend) > 3) config.backend = Backend::FSR3;
    if (static_cast<int>(config.quality) > 2) config.quality = QualityPreset::Balanced;
    
    return config;
}

void ConfigManager::Save(const Config& config) {
    Logger::Info("Saving configuration to: %s", m_Path.c_str());
    
    WriteBool("General", "Enabled", config.enabled);
    WriteInt("General", "Backend", static_cast<int>(config.backend));
    WriteInt("General", "Quality", static_cast<int>(config.quality));
    WriteFloat("General", "TargetFramerate", config.targetFramerate);
    WriteBool("General", "ShowOverlay", config.showOverlay);
    WriteBool("General", "HudLessMode", config.hudLessMode);
    WriteFloat("General", "Sharpness", config.sharpness);
}

std::string ConfigManager::ReadString(const char* section, const char* key, const char* defaultValue) {
    char buffer[256];
    GetPrivateProfileStringA(section, key, defaultValue, buffer, sizeof(buffer), m_Path.c_str());
    return std::string(buffer);
}

int ConfigManager::ReadInt(const char* section, const char* key, int defaultValue) {
    return GetPrivateProfileIntA(section, key, defaultValue, m_Path.c_str());
}

float ConfigManager::ReadFloat(const char* section, const char* key, float defaultValue) {
    char buffer[64];
    char defaultStr[64];
    snprintf(defaultStr, sizeof(defaultStr), "%.6f", defaultValue);
    
    GetPrivateProfileStringA(section, key, defaultStr, buffer, sizeof(buffer), m_Path.c_str());
    return static_cast<float>(atof(buffer));
}

bool ConfigManager::ReadBool(const char* section, const char* key, bool defaultValue) {
    char buffer[16];
    const char* defaultStr = defaultValue ? "true" : "false";
    
    GetPrivateProfileStringA(section, key, defaultStr, buffer, sizeof(buffer), m_Path.c_str());
    
    return (_stricmp(buffer, "true") == 0 || 
            _stricmp(buffer, "1") == 0 ||
            _stricmp(buffer, "yes") == 0);
}

void ConfigManager::WriteString(const char* section, const char* key, const char* value) {
    WritePrivateProfileStringA(section, key, value, m_Path.c_str());
}

void ConfigManager::WriteInt(const char* section, const char* key, int value) {
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%d", value);
    WritePrivateProfileStringA(section, key, buffer, m_Path.c_str());
}

void ConfigManager::WriteFloat(const char* section, const char* key, float value) {
    char buffer[64];
    snprintf(buffer, sizeof(buffer), "%.6f", value);
    WritePrivateProfileStringA(section, key, buffer, m_Path.c_str());
}

void ConfigManager::WriteBool(const char* section, const char* key, bool value) {
    WritePrivateProfileStringA(section, key, value ? "true" : "false", m_Path.c_str());
}

} // namespace Utils
} // namespace FiveMFrameGen

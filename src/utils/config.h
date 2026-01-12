#pragma once

/**
 * Configuration Manager
 */

#ifndef FIVEM_FRAMEGEN_CONFIG_H
#define FIVEM_FRAMEGEN_CONFIG_H

#include <string>
#include "../include/fivem_framegen.h"

namespace FiveMFrameGen {
namespace Utils {

/**
 * Configuration file manager
 */
class ConfigManager {
public:
    explicit ConfigManager(const char* filename);
    ~ConfigManager() = default;
    
    /**
     * Load configuration from file
     */
    Config Load();
    
    /**
     * Save configuration to file
     */
    void Save(const Config& config);
    
    /**
     * Get config file path
     */
    const std::string& GetPath() const { return m_Path; }

private:
    std::string m_Path;
    
    /**
     * Read a string value from file
     */
    std::string ReadString(const char* section, const char* key, const char* defaultValue);
    
    /**
     * Read an integer value
     */
    int ReadInt(const char* section, const char* key, int defaultValue);
    
    /**
     * Read a float value
     */
    float ReadFloat(const char* section, const char* key, float defaultValue);
    
    /**
     * Read a boolean value
     */
    bool ReadBool(const char* section, const char* key, bool defaultValue);
    
    /**
     * Write a string value
     */
    void WriteString(const char* section, const char* key, const char* value);
    
    /**
     * Write an integer value
     */
    void WriteInt(const char* section, const char* key, int value);
    
    /**
     * Write a float value
     */
    void WriteFloat(const char* section, const char* key, float value);
    
    /**
     * Write a boolean value
     */
    void WriteBool(const char* section, const char* key, bool value);
};

} // namespace Utils
} // namespace FiveMFrameGen

#endif // FIVEM_FRAMEGEN_CONFIG_H

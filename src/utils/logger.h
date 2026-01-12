#pragma once

/**
 * Logging utility
 */

#ifndef FIVEM_FRAMEGEN_LOGGER_H
#define FIVEM_FRAMEGEN_LOGGER_H

#include <cstdarg>
#include <cstdio>
#include <string>

namespace FiveMFrameGen {
namespace Utils {

/**
 * Simple logging system
 */
class Logger {
public:
    enum class Level {
        Debug,
        Info,
        Warn,
        Error
    };
    
    /**
     * Initialize logger with file
     */
    static void Init(const char* filename);
    
    /**
     * Shutdown logger
     */
    static void Shutdown();
    
    /**
     * Set minimum log level
     */
    static void SetLevel(Level level);
    
    /**
     * Log debug message
     */
    static void Debug(const char* format, ...);
    
    /**
     * Log info message
     */
    static void Info(const char* format, ...);
    
    /**
     * Log warning message
     */
    static void Warn(const char* format, ...);
    
    /**
     * Log error message
     */
    static void Error(const char* format, ...);

private:
    static void Log(Level level, const char* format, va_list args);
    
    static FILE* s_File;
    static Level s_Level;
    static bool s_Initialized;
};

} // namespace Utils
} // namespace FiveMFrameGen

#endif // FIVEM_FRAMEGEN_LOGGER_H

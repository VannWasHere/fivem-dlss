/**
 * Logger Implementation
 */

#include "logger.h"

#include <Windows.h>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <mutex>

namespace FiveMFrameGen {
namespace Utils {

FILE* Logger::s_File = nullptr;
Logger::Level Logger::s_Level = Logger::Level::Info;
bool Logger::s_Initialized = false;

static std::mutex s_LogMutex;

void Logger::Init(const char* filename) {
    if (s_Initialized) return;
    
    // Get path in FiveM plugins directory
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
    
    s_File = fopen(path, "w");
    if (!s_File) {
        // Try current directory
        s_File = fopen(filename, "w");
    }
    
    s_Initialized = true;
    
    if (s_File) {
        Info("Logger initialized: %s", path);
    }
}

void Logger::Shutdown() {
    if (!s_Initialized) return;
    
    if (s_File) {
        fclose(s_File);
        s_File = nullptr;
    }
    
    s_Initialized = false;
}

void Logger::SetLevel(Level level) {
    s_Level = level;
}

void Logger::Debug(const char* format, ...) {
    va_list args;
    va_start(args, format);
    Log(Level::Debug, format, args);
    va_end(args);
}

void Logger::Info(const char* format, ...) {
    va_list args;
    va_start(args, format);
    Log(Level::Info, format, args);
    va_end(args);
}

void Logger::Warn(const char* format, ...) {
    va_list args;
    va_start(args, format);
    Log(Level::Warn, format, args);
    va_end(args);
}

void Logger::Error(const char* format, ...) {
    va_list args;
    va_start(args, format);
    Log(Level::Error, format, args);
    va_end(args);
}

void Logger::Log(Level level, const char* format, va_list args) {
    if (!s_Initialized) return;
    if (level < s_Level) return;
    
    std::lock_guard<std::mutex> lock(s_LogMutex);
    
    // Get timestamp
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
    
    struct tm localTime;
    localtime_s(&localTime, &time);
    
    std::stringstream ss;
    ss << std::put_time(&localTime, "%H:%M:%S");
    ss << "." << std::setfill('0') << std::setw(3) << ms.count();
    
    // Level string
    const char* levelStr = "";
    switch (level) {
        case Level::Debug: levelStr = "DEBUG"; break;
        case Level::Info:  levelStr = "INFO "; break;
        case Level::Warn:  levelStr = "WARN "; break;
        case Level::Error: levelStr = "ERROR"; break;
    }
    
    // Format message
    char message[1024];
    vsnprintf(message, sizeof(message), format, args);
    
    // Output to file
    if (s_File) {
        fprintf(s_File, "[%s] [%s] %s\n", ss.str().c_str(), levelStr, message);
        fflush(s_File);
    }
    
    // Output to debug console
    char debugOutput[1100];
    snprintf(debugOutput, sizeof(debugOutput), 
        "[FiveMFrameGen] [%s] %s\n", levelStr, message);
    OutputDebugStringA(debugOutput);
}

} // namespace Utils
} // namespace FiveMFrameGen

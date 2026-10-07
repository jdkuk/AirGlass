// Thread-safe file logger.
#pragma once

#include <string>

enum class LogLevel { Debug = 0, Info = 1, Warn = 2, Error = 3 };

void LogInit(const std::wstring& path, LogLevel minLevel);
void LogShutdown();
void LogWrite(LogLevel level, const char* fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

#define LOGD(...) LogWrite(LogLevel::Debug, __VA_ARGS__)
#define LOGI(...) LogWrite(LogLevel::Info, __VA_ARGS__)
#define LOGW(...) LogWrite(LogLevel::Warn, __VA_ARGS__)
#define LOGE(...) LogWrite(LogLevel::Error, __VA_ARGS__)

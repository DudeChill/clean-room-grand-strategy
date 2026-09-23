#pragma once
// Logging and debug-assertion helpers. Actionable messages only: every error
// names the entity, the field and the file that produced it where possible.

#include <cstdarg>
#include <cstdint>
#include <string>

namespace hoi {

enum class LogLevel : uint8_t { Debug = 0, Info, Warn, Error };

void log_set_level(LogLevel level);
LogLevel log_get_level();
void log_write(LogLevel level, const std::string& message);

void log_fmt(LogLevel level, const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

#define HOI_DEBUG(...) ::hoi::log_fmt(::hoi::LogLevel::Debug, __VA_ARGS__)
#define HOI_INFO(...) ::hoi::log_fmt(::hoi::LogLevel::Info, __VA_ARGS__)
#define HOI_WARN(...) ::hoi::log_fmt(::hoi::LogLevel::Warn, __VA_ARGS__)
#define HOI_ERROR(...) ::hoi::log_fmt(::hoi::LogLevel::Error, __VA_ARGS__)

#if defined(HOI_DEBUG_BUILD) && HOI_DEBUG_BUILD
#define HOI_ASSERT(cond, ...)                            \
    do {                                                 \
        if (!(cond)) {                                   \
            ::hoi::log_fmt(::hoi::LogLevel::Error,       \
                           "ASSERT FAILED: %s (%s:%d) " __VA_ARGS__, #cond, __FILE__, __LINE__); \
        }                                                \
    } while (0)
#else
#define HOI_ASSERT(cond, ...) ((void)0)
#endif

}  // namespace hoi

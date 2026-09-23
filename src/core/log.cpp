// Logging implementation: a single global level filter and single-line stderr
// output. Deliberately tiny and dependency-free; nothing here is on the
// simulation's deterministic path, so it may use <cstdio> freely.

#include "core/log.h"

#include <cstdio>
#include <vector>

namespace hoi {
namespace {

LogLevel& level_ref() {
    // Function-local static: no dynamic-init order dependency across TUs.
    static LogLevel level = LogLevel::Info;
    return level;
}

const char* level_tag(LogLevel level) {
    switch (level) {
        case LogLevel::Debug: return "[DEBUG]";
        case LogLevel::Info: return "[INFO]";
        case LogLevel::Warn: return "[WARN]";
        case LogLevel::Error: return "[ERROR]";
    }
    return "[UNKNOWN]";
}

}  // namespace

void log_set_level(LogLevel level) { level_ref() = level; }

LogLevel log_get_level() { return level_ref(); }

void log_write(LogLevel level, const std::string& message) {
    if (static_cast<int>(level) < static_cast<int>(level_ref())) return;
    // One fprintf call per message keeps a line atomic enough for interactive
    // use and for interleaved output from the headless runner.
    std::fprintf(stderr, "%s %s\n", level_tag(level), message.c_str());
}

void log_fmt(LogLevel level, const char* fmt, ...) {
    if (static_cast<int>(level) < static_cast<int>(level_ref())) return;

    char stack_buf[1024];
    va_list args;
    va_start(args, fmt);
    va_list probe;
    va_copy(probe, args);
    const int needed = std::vsnprintf(stack_buf, sizeof(stack_buf), fmt, args);
    va_end(args);

    if (needed < 0) {
        va_end(probe);
        log_write(level, "(log formatting failed)");
        return;
    }
    if (static_cast<size_t>(needed) < sizeof(stack_buf)) {
        va_end(probe);
        log_write(level, std::string(stack_buf, static_cast<size_t>(needed)));
        return;
    }
    // Truncation would hide the actionable part of the message (file/field/line),
    // so reformat into a heap buffer sized to what vsnprintf reported.
    std::vector<char> big(static_cast<size_t>(needed) + 1);
    std::vsnprintf(big.data(), big.size(), fmt, probe);
    va_end(probe);
    log_write(level, std::string(big.data(), static_cast<size_t>(needed)));
}

}  // namespace hoi

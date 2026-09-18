#include "logger.hpp"
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <memory>
#include <cstdarg>

namespace logger {

static spdlog::level::level_enum to_spdlog_level(Level level) {
    switch (level) {
    case Level::Debug:
        return spdlog::level::debug;
    case Level::Info:
        return spdlog::level::info;
    case Level::Warn:
        return spdlog::level::warn;
    case Level::Error:
        return spdlog::level::err;
    }
    return spdlog::level::info;
}

void init(Level level, const std::string& file) {
    std::vector<spdlog::sink_ptr> sinks;

    // 控制台输出
    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    console_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");
    sinks.push_back(console_sink);

    // 文件输出（可选）
    if (!file.empty()) {
        auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(file, true);
        file_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
        sinks.push_back(file_sink);
    }

    auto logger = std::make_shared<spdlog::logger>("term-ime", sinks.begin(), sinks.end());
    logger->set_level(to_spdlog_level(level));
    logger->flush_on(spdlog::level::warn);

    spdlog::register_logger(logger);
    spdlog::set_default_logger(logger);
}

void set_level(Level level) {
    spdlog::set_level(to_spdlog_level(level));
}

void debug(const char* fmt, ...) {
    if (!fmt) return;
    va_list args;
    va_start(args, fmt);
    char buf[1024];
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    spdlog::debug("{}", buf);
}

void info(const char* fmt, ...) {
    if (!fmt) return;
    va_list args;
    va_start(args, fmt);
    char buf[1024];
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    spdlog::info("{}", buf);
}

void warn(const char* fmt, ...) {
    if (!fmt) return;
    va_list args;
    va_start(args, fmt);
    char buf[1024];
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    spdlog::warn("{}", buf);
}

void error(const char* fmt, ...) {
    if (!fmt) return;
    va_list args;
    va_start(args, fmt);
    char buf[1024];
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    spdlog::error("{}", buf);
}

}  // namespace logger

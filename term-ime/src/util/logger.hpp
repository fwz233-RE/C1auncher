#pragma once

// 日志适配层 - 封装 spdlog，方便更换日志库

#include <string>

namespace logger {

// 日志级别
enum class Level { Debug, Info, Warn, Error };

// 初始化日志系统
void init(Level level = Level::Info, const std::string& file = "");

// 设置日志级别
void set_level(Level level);

// 日志输出
void debug(const char* fmt, ...);
void info(const char* fmt, ...);
void warn(const char* fmt, ...);
void error(const char* fmt, ...);

// 带位置的日志（用于调试）
#define LOG_DEBUG(fmt, ...) logger::debug("[%s:%d] " fmt, __FILE__, __LINE__, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...) logger::info(fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...) logger::warn(fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) logger::error("[%s:%d] " fmt, __FILE__, __LINE__, ##__VA_ARGS__)

}  // namespace logger

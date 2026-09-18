#include "core/event_loop.hpp"
#include "core/app.hpp"
#include "core/config.hpp"
#include "util/i18n.hpp"
#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/null_sink.h>
#include <unistd.h>
#include <signal.h>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>

int main(int argc, char* argv[]) {
    const std::string config_path = argc > 1 ? argv[1] : AppConfig::default_path();
    AppConfig config = AppConfig::load(config_path);
    // Configure logging only after loading preferences. Never write logs over
    // the terminal UI, and never force debug logging or truncate another
    // running instance's log. An empty path explicitly disables file logging.
    auto silent = std::make_shared<spdlog::logger>("term-ime",
                                                  std::make_shared<spdlog::sinks::null_sink_mt>());
    spdlog::set_default_logger(silent);
    try {
        if (!config.log_file.empty()) {
            const auto parent = std::filesystem::path(config.log_file).parent_path();
            if (!parent.empty())
                std::filesystem::create_directories(parent);
            auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(config.log_file, false);
            spdlog::set_default_logger(std::make_shared<spdlog::logger>("term-ime", sink));
        }
        spdlog::set_level(spdlog::level::from_str(config.log_level));
        spdlog::flush_on(spdlog::level::warn);
    } catch (const std::exception& e) {
        std::cerr << "日志初始化失败，已禁用日志: " << e.what() << '\n';
        spdlog::set_default_logger(silent);
    }
    spdlog::info("term-ime starting with config {}", config.source_path);

    // Create event loop
    spdlog::info("Creating event loop");
    EventLoop loop;

    // Create application
    spdlog::info("Creating app");
    App app;
    if (!app.init(config, &loop)) {
        spdlog::error("App init failed");
        // Print error to stderr so user can see it even when not in TTY
        std::cerr << std::endl;
        std::cerr << "╔══════════════════════════════════════════════════════════════╗" << std::endl;
        std::cerr << "║  term-ime 启动失败                                           ║" << std::endl;
        std::cerr << "╠══════════════════════════════════════════════════════════════╣" << std::endl;
        std::cerr << "║  原因: " << app.initialization_error() << std::endl;
        std::cerr << "║                                                              ║" << std::endl;
        std::cerr << "║  可能的原因:                                                ║" << std::endl;
        std::cerr << "║    • 未连接到终端 (stdin/stdout 被重定向)                   ║" << std::endl;
        std::cerr << "║    • 终端不支持 alternate screen (需要 xterm-256color 等)   ║" << std::endl;
        std::cerr << "║    • 无法创建 PTY (权限不足)                                ║" << std::endl;
        std::cerr << "║                                                              ║" << std::endl;
        std::cerr << "║  详细日志: " << (config.log_file.empty() ? "未启用（请设置 log_file）" : config.log_file) << std::endl;
        std::cerr << "╚══════════════════════════════════════════════════════════════╝" << std::endl;
        std::cerr << std::endl;
        return 1;
    }

    spdlog::info("Registering callbacks");

    // Register PTY reader
    loop.watch_fd(app.pty_fd(), [&app, &loop](const char* data, size_t len) {
        if (len == 0 || data == nullptr) {
            // PTY closed (EOF), exit gracefully
            spdlog::info("PTY closed, exiting");
            app.on_quit(0);
            loop.stop();
        } else {
            app.on_pty_data(data, len);
        }
    });

    // Register keyboard reader
    loop.watch_fd(STDIN_FILENO, [&app, &loop](const char* data, size_t len) {
        if (len == 0 || data == nullptr) {
            // stdin reached EOF/error (terminal gone); the EventLoop already
            // dropped the watch, so exit gracefully instead of spinning.
            spdlog::info("Keyboard input closed, exiting");
            app.on_quit(0);
            loop.stop();
            return;
        }
        app.on_keyboard_data(data, len);
        if (app.quit_requested()) {
            loop.stop();
        }
    });

    // Register signal handlers
    loop.watch_signal(SIGWINCH, [&app](int signum) { app.on_resize(signum); });

    loop.watch_signal(SIGINT, [&app, &loop](int signum) {
        app.on_quit(signum);
        loop.stop();
    });

    loop.watch_signal(SIGTERM, [&app, &loop](int signum) {
        app.on_quit(signum);
        loop.stop();
    });

    // Run event loop
    spdlog::info("Starting event loop");
    loop.run();
    spdlog::info("Event loop finished");

    return 0;
}

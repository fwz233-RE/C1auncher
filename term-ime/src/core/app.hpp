#pragma once

#include "terminal/pty.hpp"
#include "terminal/screen.hpp"
#include "terminal/parser.hpp"
#include "ime/rime_engine.hpp"
#include "ime/language.hpp"
#include "ui/renderer.hpp"
#include "ui/settings.hpp"
#include "input_processor.hpp"
#include "config.hpp"
#include "util/utf8.hpp"

#include <memory>
#include <string>
#include <atomic>
#include <functional>

class EventLoop;
// Application state
class App {
   public:
    App();
    ~App();

    // Initialize application with config. `event_loop` (owned by main()) is
    // used to arm the orphaned-ESC timeout timer; may be null to disable it.
    bool init(const AppConfig& config, EventLoop* event_loop = nullptr);
    const std::string& initialization_error() const { return initialization_error_; }

    // Handle PTY data
    void on_pty_data(const char* data, size_t len);

    // Handle keyboard data
    void on_keyboard_data(const char* data, size_t len);

    // Handle window resize signal
    void on_resize(int signum);

    // Handle quit signal / request
    void on_quit(int signum);

    // True after on_quit() was called (e.g. Ctrl+A+Ctrl+C) — the caller should
    // stop the event loop.
    bool quit_requested() const { return !initialized_; }

    // Render current state
    void render();

    // Get PTY fd for event loop
    int pty_fd() const;

    // Get TTY fd for event loop
    int tty_fd() const;

    // Get current language
    const LanguageConfig& current_language() const;

    // Switch language
    bool switch_language(const std::string& lang_id);

    // Switch UI language
    void switch_ui_language(const std::string& lang_code);

    // Get available UI languages
    static std::vector<std::pair<std::string, std::string>> available_ui_languages();

    // Toggle settings panel
    void toggle_settings();

    // Check if settings panel is visible
    bool is_settings_visible() const;

   private:
    friend struct AppTestPeer;
    Renderer renderer_;
    Pty pty_;
    std::unique_ptr<Screen> screen_;
    std::unique_ptr<Parser> parser_;
    std::unique_ptr<RimeIme> ime_;
    LanguageManager language_manager_;
    InputProcessor input_processor_;
    AppConfig config_;
    size_t selected_candidate_ = 0;
    bool initialized_ = false;
    bool ime_ready_ = false;
    std::string initialization_error_;
    bool need_render_ = false;
    // All keyboard/IME writes share one sink; tests replace it without spawning
    // a shell or initializing the process-global Rime engine.
    std::function<void(const std::vector<uint8_t>&)> input_sink_;
    ui::SettingsState settings_state_;  // Settings panel state
    // EventLoop owned by main(); used to arm/clear the lone-ESC timeout timer.
    EventLoop* event_loop_ = nullptr;
    // Pending lone-ESC timeout timer id; 0 = none armed.
    uint64_t esc_timer_id_ = 0;
    // Candidate bar windowing. rime hands back one page of candidates (see
    // menu/page_size in the schema); the bar can only show
    // ui::FitCandidateBar().count of them on a narrow terminal, so the visible
    // slice is shifted by this offset. Digit keys select inside the visible
    // slice; '.'/',' shift by one slice and page through rime once the slice
    // reaches the end of the page.
    size_t candidate_window_ = 0;
    int candidate_slots_ = 0;         // candidates the last draw really showed
    std::string candidate_page_sig_;  // detects a new rime page / composition

    void on_language_change(const LanguageConfig& lang);
    // IME context snapshot. The PTY-output path repaints the status bar far more
    // often than the user types, and shell output cannot change the IME context,
    // so it reuses this instead of issuing three separate rime queries per
    // chunk (defect 17).
    struct ImeSnapshot {
        std::string mode;
        std::string buffer;
        std::vector<Candidate> candidates;
    };
    ImeSnapshot ime_snapshot_;
    void refresh_ime_snapshot();
    std::vector<Candidate> update_candidate_window();
    void render_candidates_bar(bool refresh = true);
    void handle_input_event(const input_sm::Result& event);
    void on_escape_timeout();
    void write_committed(const std::u32string& text);
    // Shift the visible candidate window by one group; rolls onto the previous /
    // next rime page when the window would run past the page edge.
    void advance_candidate_window(int direction);  // <0 previous, >0 next
    void on_settings_change(const std::string& key, const std::string& value);
    void on_settings_close();
    void render_settings_panel();
};
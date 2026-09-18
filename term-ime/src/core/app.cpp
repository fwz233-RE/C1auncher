#include "app.hpp"
#include "event_loop.hpp"
#include "util/i18n.hpp"
#include <spdlog/spdlog.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <cerrno>
#include <cstring>
#include <algorithm>

// The same disambiguation applies in the shell, during composition and in
// settings. A read() boundary is never an Escape key boundary.
static constexpr uint64_t kEscapeTimeoutMs = 50;

App::App() {
    input_sink_ = [this](const std::vector<uint8_t>& bytes) {
        if (!pty_.write(bytes))
            spdlog::warn("Failed to forward input to the shell");
    };
    renderer_.set_response_handler([this](const std::string& response) {
        // Protocol replies go straight to the child, never through the IME or
        // the settings keyboard handler.
        input_sink_(std::vector<uint8_t>(response.begin(), response.end()));
    });
}

App::~App() {
    if (event_loop_ && esc_timer_id_)
        event_loop_->clear_timer(esc_timer_id_);
    if (initialized_)
        renderer_.restore();
}

bool App::init(const AppConfig& config, EventLoop* event_loop) {
    initialization_error_.clear();
    config_ = config;
    event_loop_ = event_loop;
    I18n::init(I18n::parse_lang(config_.ui_language));
    try {
        renderer_.init();
        if (!renderer_.is_initialized()) {
            initialization_error_ = "Cannot initialize terminal stdin/stdout";
            return false;
        }
        if (!pty_.spawn(config.resolved_shell())) {
            initialization_error_ = pty_.spawn_error();
            spdlog::error("{}", initialization_error_);
            renderer_.restore();
            return false;
        }
        struct winsize ws {};
        if (ioctl(renderer_.get_tty_fd(), TIOCGWINSZ, &ws) < 0 || ws.ws_row < 2 ||
            ws.ws_row > 1000 || ws.ws_col == 0 || ws.ws_col > 1000) {
            ws.ws_row = 24;
            ws.ws_col = 80;
        }
        pty_.resize(ws.ws_row - 1, ws.ws_col);
        screen_ = std::make_unique<Screen>(ws.ws_row - 1, ws.ws_col);
        parser_ = std::make_unique<Parser>(*screen_);
        renderer_.attach_terminal(*parser_);
        language_manager_.load(config_);
        language_manager_.on_language_change([this](const LanguageConfig& lang) { on_language_change(lang); });

        const std::string hint = "\x1b[H\x1b[2K" + I18n::t("status.initializing");
        renderer_.forward_output(hint.data(), hint.size());
        ime_ = std::make_unique<RimeIme>(config_.rime_shared_data_dir, config_.rime_user_data_dir);
        ime_->set_fuzzy_pinyin(config_.fuzzy_pinyin);
        try {
            const auto& language = language_manager_.current();
            ime_ready_ = ime_->initialize() && language.enabled && !language.schema.empty() &&
                         ime_->select_schema(ime_->fuzzy_variant(language.schema));
        } catch (const std::exception& e) {
            spdlog::error("IME initialization failed: {}", e.what());
            ime_ready_ = false;
        }
        if (!ime_ready_) {
            ime_->set_mode(ImeMode::English);
            spdlog::warn("Rime is unavailable; using English passthrough (EN!)");
        }
        static const char clear_hint[] = "\x1b[H\x1b[2K";
        renderer_.forward_output(clear_hint, sizeof(clear_hint) - 1);

        ui::settings_init(settings_state_, config_);
        settings_state_.on_change = [this](const std::string& key, const std::string& value) {
            on_settings_change(key, value);
        };
        settings_state_.on_close = [this]() { on_settings_close(); };
        initialized_ = true;
        render_candidates_bar();
        return true;
    } catch (const std::exception& e) {
        initialization_error_ = e.what();
        spdlog::error("Exception during init: {}", e.what());
        renderer_.restore();
        return false;
    }
}

void App::on_pty_data(const char* data, size_t len) {
    // Renderer is the sole owner of framing and parsing PTY output, including
    // while settings suppress host painting. Feeding Parser again would apply
    // every character and cursor operation twice.
    renderer_.forward_output(data, len, settings_state_.visible);
    if (!settings_state_.visible)
        render_candidates_bar(false);
}

void App::refresh_ime_snapshot() {
    ime_snapshot_ = ImeSnapshot{};
    if (!ime_ready_ || !ime_) {
        ime_snapshot_.mode = "EN!";
        return;
    }
    if (config_.show_mode_indicator)
        ime_snapshot_.mode = ime_->mode() == ImeMode::Chinese ? "拼" : "EN";
    ime_snapshot_.candidates = ime_->candidates();
    ime_snapshot_.buffer = ime_->buffer();
}

std::vector<Candidate> App::update_candidate_window() {
    const auto& all = ime_snapshot_.candidates;
    std::string sig = ime_snapshot_.buffer + '\x1e';
    for (const auto& c : all) {
        for (char32_t ch : c.text)
            sig += utf8::encode(ch);
        sig += '\x1f';
    }
    if (sig != candidate_page_sig_) {
        candidate_page_sig_ = std::move(sig);
        candidate_window_ = 0;
    }
    if (all.empty()) {
        candidate_window_ = 0;
        candidate_slots_ = 0;
        return {};
    }
    candidate_window_ = std::min(candidate_window_, all.size() - 1);
    struct winsize ws {};
    int cols = 80;
    if (ioctl(renderer_.get_tty_fd(), TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        cols = ws.ws_col;
    std::vector<Candidate> tail(all.begin() + candidate_window_, all.end());
    candidate_slots_ = std::max(1, ui::FitCandidateBar(cols, ime_snapshot_.mode, ime_snapshot_.buffer,
                                                     tail, config_.max_candidates).count);
    tail.resize(static_cast<size_t>(candidate_slots_));
    return tail;
}

void App::render_candidates_bar(bool refresh) {
    if (refresh)
        refresh_ime_snapshot();
    const auto shown = update_candidate_window();
    size_t selected = 0;
    if (selected_candidate_ >= candidate_window_ && selected_candidate_ < candidate_window_ + shown.size())
        selected = selected_candidate_ - candidate_window_;
    renderer_.render_candidates(shown, selected, ime_snapshot_.buffer, ime_snapshot_.mode, config_.max_candidates);
}

void App::advance_candidate_window(int direction) {
    if (!ime_ready_ || !ime_)
        return;
    refresh_ime_snapshot();
    update_candidate_window();
    const size_t step = static_cast<size_t>(std::max(1, candidate_slots_));
    if (direction > 0 && candidate_window_ + step < ime_snapshot_.candidates.size()) {
        candidate_window_ += step;
        return;
    }
    if (direction < 0 && candidate_window_ > 0) {
        candidate_window_ = candidate_window_ >= step ? candidate_window_ - step : 0;
        return;
    }
    const size_t previous_window = candidate_window_;
    const std::string previous_page = candidate_page_sig_;
    if (direction > 0)
        ime_->page_down();
    else
        ime_->page_up();
    refresh_ime_snapshot();
    update_candidate_window();
    if (candidate_page_sig_ == previous_page) {
        // No next/previous Rime page: keep the current window instead of
        // wrapping around the same page unexpectedly.
        candidate_window_ = previous_window;
    } else if (direction < 0) {
        // Walk the windows of the previous page to its last visible group.
        while (candidate_window_ + static_cast<size_t>(candidate_slots_) < ime_snapshot_.candidates.size()) {
            candidate_window_ += static_cast<size_t>(candidate_slots_);
            update_candidate_window();
        }
    }
}

void App::write_committed(const std::u32string& text) {
    if (text.empty())
        return;
    std::string bytes;
    for (char32_t ch : text)
        bytes += utf8::encode(ch);
    input_sink_(std::vector<uint8_t>(bytes.begin(), bytes.end()));
}

void App::handle_input_event(const input_sm::Result& event) {
    // Paste contents are literal bytes even when they contain shortcuts or
    // complete escape sequences. Settings consume a paste without activating
    // controls; a paste into the shell cancels stale preedit, not the payload.
    if (event.paste) {
        if (event.paste_begin && ime_ready_ && ime_ && ime_->state() != ImeState::Inactive) {
            ime_->cancel();
            selected_candidate_ = 0;
            need_render_ = true;
        }
        if (!settings_state_.visible && event.forward)
            input_sink_(event.data);
        return;
    }
    // Dispatch complete events, never the byte that happened to finish them.
    if (event.toggle_mode) {
        if (!settings_state_.visible && ime_ready_ && ime_) {
            ime_->cancel();
            ime_->toggle_mode();
            candidate_window_ = 0;
            need_render_ = true;
        }
        return;
    }
    if (!event.forward || event.data.empty())
        return;
    const auto& key = event.data;
    if (key.size() == 2 && key[0] == 1) {
        if (key[1] == 's' || key[1] == 'S') {
            if (ime_ready_ && ime_)
                ime_->cancel();
            toggle_settings();
            return;
        }
        if (key[1] == 3) {
            on_quit(0);
            return;
        }
    }
    const bool arrow = key.size() == 3 && key[0] == 0x1b && (key[1] == '[' || key[1] == 'O') &&
                       key[2] >= 'A' && key[2] <= 'D';
    if (settings_state_.visible) {
        if (arrow)
            ui::settings_handle_key(settings_state_, key[2]);
        else if (key.size() == 1)
            ui::settings_handle_key(settings_state_, key[0]);
        need_render_ = true;
        return;
    }
    if (!ime_ || !ime_ready_ || ime_->mode() == ImeMode::English) {
        input_sink_(key);
        return;
    }
    const bool composing = ime_->state() != ImeState::Inactive;
    if (key.size() == 1 && key[0] == 0x1b && composing) {
        ime_->cancel();
        selected_candidate_ = 0;
        need_render_ = true;
        return;
    }
    if (composing) {
        int direction = 0;
        if (arrow)
            direction = (key[2] == 'A' || key[2] == 'D') ? -1 : 1;
        else if (key.size() == 4 && key[0] == 0x1b && key[1] == '[' && key[3] == '~') {
            if (key[2] == '5')
                direction = -1;
            else if (key[2] == '6')
                direction = 1;
        }
        if (direction) {
            advance_candidate_window(direction);
            need_render_ = true;
            return;
        }
        int edit_key = 0;
        if (key.size() == 3 && key[0] == 0x1b && (key[1] == '[' || key[1] == 'O')) {
            if (key[2] == 'H')
                edit_key = 0xFF50;  // Home
            else if (key[2] == 'F')
                edit_key = 0xFF57;  // End
        } else if (key.size() == 4 && key[0] == 0x1b && key[1] == '[' && key[3] == '~') {
            if (key[2] == '3')
                edit_key = 0xFFFF;  // Delete
            else if (key[2] == '1' || key[2] == '7')
                edit_key = 0xFF50;
            else if (key[2] == '4' || key[2] == '8')
                edit_key = 0xFF57;
        }
        if (edit_key) {
            ime_->process_key(edit_key);
            write_committed(ime_->take_commit());
            need_render_ = true;
            return;
        }
    }
    // Ctrl+A combinations, Alt keys and unknown escape sequences are opaque.
    // In particular, never turn the trailing letter/digit into IME input.
    if (key.size() != 1) {
        if (composing)
            ime_->cancel();
        input_sink_(key);
        need_render_ = need_render_ || composing;
        return;
    }
    const uint8_t ch = key[0];
    if (composing && (ch == ' ' || (ch >= '1' && ch <= '9'))) {
        // Refresh logical candidates before selection, even if painting was
        // deferred. Thus ni1 is identical regardless of read() chunking.
        refresh_ime_snapshot();
        update_candidate_window();
        const int slot = ch == ' ' ? 0 : ch - '1';
        if (slot < candidate_slots_) {
            write_committed(ime_->select(static_cast<int>(candidate_window_) + slot));
            need_render_ = true;
            return;
        }
        if (candidate_slots_ > 0)
            return;  // no selection of an invisible candidate
    }
    if (composing && (ch == 8 || ch == 127)) {
        ime_->backspace();
        selected_candidate_ = 0;
        need_render_ = true;
        return;
    }
    if (composing && (ch == ',' || ch == '<' || ch == '.' || ch == '>')) {
        advance_candidate_window(ch == ',' || ch == '<' ? -1 : 1);
        need_render_ = true;
        return;
    }
    if (ch == 3 || ch == 7) {
        if (composing)
            ime_->cancel();
        if (ch == 3 || !composing)
            input_sink_(key);
        need_render_ = need_render_ || composing;
        return;
    }
    bool accepted = false;
    if (ch >= 0x20 && ch < 0x7f)
        accepted = ime_->input(static_cast<char>(ch));
    else if (composing && (ch == '\r' || ch == '\n' || ch == '\t'))
        accepted = ime_->process_key(ch == '\t' ? 0xFF09 : 0xFF0D);
    else if (composing && ch >= 1 && ch <= 26)
        accepted = ime_->process_key('a' + ch - 1, 4);  // Rime ControlMask
    if (accepted) {
        write_committed(ime_->take_commit());
        selected_candidate_ = 0;
        need_render_ = true;
    } else {
        // A failed/unhandled IME input is never silently lost.
        input_sink_(key);
    }
}

void App::on_escape_timeout() {
    if (event_loop_ && esc_timer_id_) {
        event_loop_->clear_timer(esc_timer_id_);
        esc_timer_id_ = 0;
    }
    handle_input_event(input_processor_.flush_escape());
    if (need_render_)
        render();
}

void App::on_keyboard_data(const char* data, size_t len) {
    if (!data || !len)
        return;
    if (event_loop_ && esc_timer_id_) {
        event_loop_->clear_timer(esc_timer_id_);
        esc_timer_id_ = 0;
    }
    for (size_t i = 0; i < len; ++i) {
        handle_input_event(input_processor_.process(static_cast<uint8_t>(data[i])));
        if (quit_requested())
            return;
    }
    if (event_loop_ && input_processor_.in_escape())
        esc_timer_id_ = event_loop_->set_timer([this]() { on_escape_timeout(); }, kEscapeTimeoutMs, false);
    if (need_render_)
        render();
}

void App::on_resize(int signum) {
    (void)signum;
    struct winsize ws {};
    if (ioctl(renderer_.get_tty_fd(), TIOCGWINSZ, &ws) < 0 || ws.ws_row < 2 || ws.ws_row > 1000 ||
        ws.ws_col == 0 || ws.ws_col > 1000)
        return;
    if (parser_)
        parser_->resize(ws.ws_row - 1, ws.ws_col);
    else if (screen_)
        screen_->resize(ws.ws_row - 1, ws.ws_col);
    pty_.resize(ws.ws_row - 1, ws.ws_col);
    renderer_.update_scroll_region();
    if (screen_ && !settings_state_.visible)
        renderer_.redraw_shell(*screen_);
    render();
}

void App::on_quit(int signum) {
    (void)signum;
    if (event_loop_ && esc_timer_id_) {
        event_loop_->clear_timer(esc_timer_id_);
        esc_timer_id_ = 0;
    }
    renderer_.restore();
    initialized_ = false;
}

void App::render() {
    need_render_ = false;
    if (!screen_)
        return;
    renderer_.render(*screen_);
    if (settings_state_.visible)
        renderer_.render_settings(settings_state_);
    else
        render_candidates_bar();
}

int App::pty_fd() const { return pty_.fd(); }
int App::tty_fd() const { return renderer_.get_tty_fd(); }
const LanguageConfig& App::current_language() const { return language_manager_.current(); }
bool App::switch_language(const std::string& lang_id) { return language_manager_.switch_language(lang_id); }

void App::switch_ui_language(const std::string& lang_code) {
    I18n::set_lang(I18n::parse_lang(lang_code));
    config_.ui_language = lang_code;
    render();
}

std::vector<std::pair<std::string, std::string>> App::available_ui_languages() {
    return {{"en", "English"}, {"zh-CN", "简体中文"}};
}

void App::toggle_settings() {
    if (settings_state_.visible) {
        on_settings_close();
        return;
    }
    settings_state_.visible = true;
    ui::settings_init(settings_state_, config_);
    render();
}

bool App::is_settings_visible() const { return settings_state_.visible; }

void App::on_settings_change(const std::string& key, const std::string& value) {
    if (key == "ui_language") {
        I18n::set_lang(I18n::parse_lang(value));
        config_.ui_language = value;
        ui::settings_init(settings_state_, config_);
    } else if (key == "max_candidates") {
        const int requested = value.empty() ? 0 : value[0] - '0';
        config_.max_candidates = std::max(1, std::min(9, requested));
    } else if (key == "fuzzy_pinyin") {
        config_.fuzzy_pinyin = value == "on";
        if (ime_) {
            ime_->set_fuzzy_pinyin(config_.fuzzy_pinyin);
            const auto& language = language_manager_.current();
            ime_ready_ = language.enabled && !language.schema.empty() &&
                         ime_->select_schema(ime_->fuzzy_variant(language.schema));
            if (!ime_ready_)
                ime_->set_mode(ImeMode::English);
        }
    }
    candidate_window_ = 0;
    render();
}

void App::on_settings_close() {
    settings_state_.visible = false;
    if (screen_)
        renderer_.redraw_shell(*screen_);
    const std::string path = config_.source_path.empty() ? AppConfig::default_path() : config_.source_path;
    if (!config_.save(path))
        spdlog::error("Failed to save settings to {}", path);
    render();
}

void App::on_language_change(const LanguageConfig& lang) {
    if (ime_) {
        ime_ready_ = lang.enabled && !lang.schema.empty() && ime_->select_schema(ime_->fuzzy_variant(lang.schema));
        if (!ime_ready_)
            ime_->set_mode(ImeMode::English);
    }
    config_.active_language = lang.id;
    candidate_window_ = 0;
    render();
}

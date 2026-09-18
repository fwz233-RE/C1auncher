#pragma once

#include "../terminal/parser.hpp"
#include "../terminal/output_stream.hpp"
#include "../ime/engine.hpp"
#include "components.hpp"
#include "settings.hpp"
#include <functional>
#include <memory>
#include <string>
#include <utility>

class Renderer {
   public:
    Renderer();
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    void init();
    void restore();
    void update_scroll_region();
    void render(const Screen& screen);

    // Attach after App creates its Parser. forward_output becomes the only PTY
    // input owner: callers must NOT also feed the same bytes to the Parser.
    void attach_terminal(Parser& parser) { parser_ = &parser; }
    // Responses are terminal protocol bytes, not keyboard/IME events. App must
    // write them directly to the child PTY, including while settings are open.
    void set_response_handler(std::function<void(const std::string&)> handler) {
        response_handler_ = std::move(handler);
    }
    // Suppression is for fullscreen overlays: still frame and parse every
    // byte, but produce no host output until the caller redraws the model.
    void forward_output(const char* data, size_t len, bool suppress = false);
    void redraw_shell(const Screen& screen);
    void render_candidates(const std::vector<Candidate>& candidates, size_t selected, const std::string& buffer,
                           const std::string& mode = "EN", int max_items = 9);
    void render_settings(ui::SettingsState& state);
    int read_key();
    int get_tty_fd() const;
    bool is_initialized() const { return initialized_; }

   private:
    int tty_fd_ = -1;
    bool initialized_ = false;
    struct termios* saved_termios_ = nullptr;
    OutputStream output_stream_;
    Parser* parser_ = nullptr;
    // Before App attaches its model (startup hints), keep output safe as well.
    std::unique_ptr<Screen> startup_screen_;
    std::unique_ptr<Parser> startup_parser_;
    std::string last_bar_sig_;
    int bar_skip_count_ = 0;
    static constexpr int BAR_FORCE_REDRAW_EVERY = 16;
    bool bar_dirty_ = false;
    std::function<void(const std::string&)> response_handler_;

    bool answer_query(std::string_view token);
    void restore_private_modes(bool defaults = false);
    void forward_token(std::string_view token, bool suppress);
    void restore_shell_state(const Screen& screen);
    void render_element(const ui::Element& element);
};

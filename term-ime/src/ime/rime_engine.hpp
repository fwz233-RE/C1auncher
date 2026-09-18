#pragma once

#include "engine.hpp"
#include <rime_api.h>
#include <string>
#include <vector>
#include <memory>

// Rime-based input method engine wrapper
class RimeIme : public ImeEngine {
   public:
    // Initialize with optional data directories
    // shared_data_dir: system rime data directory (default: /usr/share/rime-data)
    // user_data_dir: user config directory (default: ~/.config/term-ime)
    explicit RimeIme(const std::string& shared_data_dir = "", const std::string& user_data_dir = "");
    ~RimeIme();

    bool input(char ch) override;
    // Non-printable Rime keysyms (Return/Tab/etc.) and commits produced by
    // ordinary input, such as Chinese punctuation.
    virtual bool process_key(int keysym, int modifiers = 0);
    virtual std::u32string take_commit();
    ImeState state() const override;
    ImeMode mode() const override;
    void set_mode(ImeMode mode) override;
    void toggle_mode() override;
    std::string buffer() const override;
    std::vector<Candidate> candidates() const override;
    std::u32string select(int index) override;
    void backspace() override;
    void cancel() override;
    void page_up() override;
    void page_down() override;

    // Rime-specific methods
    bool select_schema(const std::string& schema_id);
    std::vector<std::string> get_schema_list();
    std::string get_current_schema();

    // Initialize rime engine
    bool initialize();
    // Fuzzy pinyin (n/l, zh/z, r/l, r/y, hu/f, en-eng, in-ing). Implemented by
    // switching to the bundled <schema>_fuzzy variant — no redeploy involved.
    void set_fuzzy_pinyin(bool on);
    bool fuzzy_pinyin() const { return fuzzy_; }
    // The fuzzy twin of a bundled schema id, or the id itself when there is none.
    std::string fuzzy_variant(const std::string& schema_id) const;

   private:
    // Stateful deleter for rime_life_: closes the current session (if one was
    // created) and finalizes librime's global state exactly once.
    struct RimeShutdown {
        RimeIme* owner = nullptr;
        void operator()(RimeApi* api) const;
    };

    RimeApi* rime_ = nullptr;  // borrowed from rime_get_api(); never owned here
    // Non-null exactly between a successful rime_->initialize() and its
    // finalize(), so every failure path below and ~RimeIme release librime.
    std::unique_ptr<RimeApi, RimeShutdown> rime_life_;
    RimeSessionId session_ = 0;
    ImeMode mode_ = ImeMode::English;  // 默认英文模式，不影响终端正常使用
    std::string shared_data_dir_;
    std::string user_data_dir_;
    bool fuzzy_ = false;

    void update_state();
    std::u32string utf8_to_utf32(const std::string& utf8) const;
};
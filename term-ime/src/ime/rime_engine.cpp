#include "rime_engine.hpp"
#include "../util/utf8.hpp"
#include <spdlog/spdlog.h>
#include <cstring>
#include <algorithm>
#include <filesystem>

// Bundled rime-data dir (set by CMake when USE_BUNDLED_DEPS sources are
// built). Empty string when building against system rime-data.
#ifndef RIME_BUNDLED_DATA_DIR
#define RIME_BUNDLED_DATA_DIR ""
#endif

// rime_life_ is built with an explicit deleter: GCC cannot default-construct a
// unique_ptr whose deleter is a nested class of the unique_ptr's owner.
RimeIme::RimeIme(const std::string& shared_data_dir, const std::string& user_data_dir)
    : rime_life_(nullptr, RimeShutdown{this}), shared_data_dir_(shared_data_dir), user_data_dir_(user_data_dir) {}

RimeIme::~RimeIme() = default;  // rime_life_ destroys the session and finalizes librime

void RimeIme::RimeShutdown::operator()(RimeApi* api) const {
    if (!api) {
        return;
    }
    RimeSessionId session = owner ? owner->session_ : 0;
    if (session) {
        api->destroy_session(session);
        owner->session_ = 0;
    }
    spdlog::debug("Rime: finalizing global state (session={})", session);
    api->finalize();
}

bool RimeIme::initialize(bool prebuilt_only) {
    rime_ = rime_get_api();
    if (!rime_) {
        return false;
    }

    // Setup traits
    // C++ value-initialization avoids the C API macro's missing-field warnings.
    RimeTraits traits{};
    RIME_STRUCT_INIT(RimeTraits, traits);

    // Determine shared data directory — prefer term-ime's own bundled data, then
    // fall back to system rime-data (which ships prebuilt prism/table .bin so no
    // runtime deploy is needed). The user data dir is always term-ime's own.
    std::string shared_dir = shared_data_dir_;
    if (shared_dir.empty()) {
        // Get user's home directory for fallback paths
        const char* home = getenv("HOME");
        std::string user_local = home ? std::string(home) + "/.local/share/term-ime/rime-data" : "";

        std::error_code executable_error;
        const auto executable = std::filesystem::read_symlink("/proc/self/exe", executable_error);
        const std::string installed_data = executable_error ? "" :
            (executable.parent_path().parent_path() / "share/term-ime/rime-data").string();
        std::vector<std::string> search_paths = {
            installed_data,        // relocatable install, including --prefix
            RIME_BUNDLED_DATA_DIR,  // build-time bundled data
            user_local,             // user-local install
            "/usr/local/share/term-ime/rime-data",
            "/usr/share/term-ime/rime-data",
            "/usr/share/rime-data",
            "/usr/local/share/rime-data",
        };

        for (const auto& path : search_paths) {
            if (!path.empty() && std::filesystem::exists(path)) {
                shared_dir = path;
                break;
            }
        }

        if (shared_dir.empty()) {
            shared_dir = "/usr/share/rime-data";  // fallback
        }
    }
    traits.shared_data_dir = shared_dir.c_str();

    // Use default user data dir — term-ime's OWN XDG data dir (never ~/.rime or
    // the system rime user dir). rime stores its compiled prism/table + user db
    // here; user schema customizations also live here.
    std::string user_dir = user_data_dir_;
    if (user_dir.empty()) {
        const char* xdg_data = getenv("XDG_DATA_HOME");
        if (xdg_data && *xdg_data) {
            user_dir = std::filesystem::path(xdg_data) / "term-ime";
        } else {
            const char* home = getenv("HOME");
            if (home) {
                user_dir = std::filesystem::path(home) / ".local" / "share" / "term-ime";
            } else {
                user_dir = "/tmp/term-ime";
            }
        }
    }
    traits.user_data_dir = user_dir.c_str();

    traits.distribution_name = "term-ime";
    traits.distribution_code_name = "term-ime";
    traits.distribution_version = "1.0.0";
    traits.app_name = "rime.term-ime";

    // staging_dir (where compiled prism.bin/table.bin are written) and
    // prebuilt_data_dir (where prebuilt .bin are read from) MUST be absolute —
    // rime defaults them to the relative path "build", which resolves against
    // the process CWD and scatters deploy output to an unexpected location.
    std::string staging_dir = std::filesystem::path(user_dir) / "build";
    std::string prebuilt_dir = std::filesystem::path(shared_dir) / "build";
    traits.staging_dir = staging_dir.c_str();
    traits.prebuilt_data_dir = prebuilt_dir.c_str();

    rime_->setup(&traits);
    rime_->initialize(nullptr);
    // librime's global state exists from here on; own it so every exit path
    // (failures below, and ~RimeIme) releases it via finalize().
    rime_life_ = std::unique_ptr<RimeApi, RimeShutdown>(rime_, RimeShutdown{this});

    // Force a full maintenance check: this compiles (deploys) the dictionary into
    // prism.bin/table.bin when missing or stale. Without it, on a fresh user data
    // dir rime has no compiled dictionary and input (e.g. pinyin) produces no
    // candidates. full_check=True ensures deployment runs even on first launch.
    if (!prebuilt_only && rime_->start_maintenance(True)) {
        rime_->join_maintenance_thread();
    }

    // start_maintenance's workspace_update can fail to build schemas when the
    // shared data dir has no prebuilt build/ (our bundled rime-data ships source
    // .yaml, not compiled .bin). Explicitly deploy each schema file if its prism
    // is still missing in the user data dir's staging build/.
    if (!prebuilt_only && !shared_dir.empty() && rime_->deploy_schema) {
        // Non-throwing filesystem calls: a missing/unreadable shared data dir must
        // not throw out of here with rime left initialized.
        std::filesystem::path staging = std::filesystem::path(user_dir) / "build";
        std::error_code ec;
        for (auto& entry : std::filesystem::directory_iterator(shared_dir, ec)) {
            auto p = entry.path();
            if (p.filename().string().find(".schema.yaml") == std::string::npos)
                continue;
            std::string stem = p.stem().string();
            std::string schema_id = stem.substr(0, stem.rfind(".schema"));
            std::string prism_name = schema_id + ".prism.bin";
            if (!std::filesystem::exists(staging / prism_name, ec) &&
                !std::filesystem::exists(std::filesystem::path(prebuilt_dir) / prism_name, ec)) {
                spdlog::info("Deploying schema: {}", p.string());
                rime_->deploy_schema(p.string().c_str());
            }
        }
        if (ec) {
            spdlog::warn("Rime: cannot scan shared data dir {}: {}", shared_dir, ec.message());
        }
    }

    // Create session
    session_ = rime_->create_session();
    if (!session_) {
        spdlog::error("Rime: failed to create session, releasing input method");
        rime_life_.reset();  // finalize() now; no session to destroy
        return false;
    }
    return true;
}

bool RimeIme::input(char ch) {
    return process_key(static_cast<unsigned char>(ch));
}

bool RimeIme::process_key(int keysym, int modifiers) {
    if (mode_ == ImeMode::English || !rime_ || !session_)
        return false;
    return rime_->process_key(session_, keysym, modifiers);
}

std::u32string RimeIme::take_commit() {
    if (!rime_ || !session_)
        return U"";
    std::u32string result;
    RimeCommit commit{};
    RIME_STRUCT_INIT(RimeCommit, commit);
    if (rime_->get_commit(session_, &commit)) {
        if (commit.text)
            result = utf8_to_utf32(commit.text);
        rime_->free_commit(&commit);
    }
    return result;
}

ImeState RimeIme::state() const {
    if (!rime_ || !session_)
        return ImeState::Inactive;

    RimeContext context{};
    RIME_STRUCT_INIT(RimeContext, context);
    if (rime_->get_context(session_, &context)) {
        bool has_composition = context.composition.length > 0;
        bool has_candidates = context.menu.num_candidates > 0;
        rime_->free_context(&context);

        if (has_candidates) {
            return ImeState::Selecting;
        }
        if (has_composition) {
            return ImeState::Composing;
        }
    }
    return ImeState::Inactive;
}

ImeMode RimeIme::mode() const {
    return mode_;
}

void RimeIme::set_mode(ImeMode mode) {
    mode_ = mode;
}

void RimeIme::toggle_mode() {
    mode_ = (mode_ == ImeMode::Chinese) ? ImeMode::English : ImeMode::Chinese;
    // Clear rime session composition when switching modes
    if (rime_ && session_) {
        rime_->clear_composition(session_);
    }
}

std::string RimeIme::buffer() const {
    if (!rime_ || !session_)
        return "";

    RimeContext context{};
    RIME_STRUCT_INIT(RimeContext, context);
    if (rime_->get_context(session_, &context)) {
        const char* preedit = context.composition.preedit;
        std::string result(preedit ? preedit : "");
        rime_->free_context(&context);
        return result;
    }
    return "";
}

std::vector<Candidate> RimeIme::candidates() const {
    std::vector<Candidate> result;
    if (!rime_ || !session_)
        return result;

    RimeContext context{};
    RIME_STRUCT_INIT(RimeContext, context);
    if (rime_->get_context(session_, &context)) {
        for (int i = 0; i < context.menu.num_candidates; ++i) {
            Candidate cand;
            const char* text = context.menu.candidates[i].text;
            const char* comment = context.menu.candidates[i].comment;

            // Safety check for null pointers
            if (text) {
                cand.text = utf8_to_utf32(text);
            }
            cand.code = comment ? comment : "";
            result.push_back(cand);
        }
        rime_->free_context(&context);
    }
    return result;
}

std::u32string RimeIme::select(int index) {
    if (!rime_ || !session_ || index < 0)
        return U"";

    RimeContext context{};
    RIME_STRUCT_INIT(RimeContext, context);
    if (!rime_->get_context(session_, &context))
        return U"";
    const bool valid = index < context.menu.num_candidates;
    const size_t absolute_index = static_cast<size_t>(std::max(0, context.menu.page_no)) *
                                  static_cast<size_t>(std::max(0, context.menu.page_size)) +
                                  static_cast<size_t>(index);
    rime_->free_context(&context);
    if (!valid)
        return U"";

    // The UI's 1..9 slots are not Rime's selection keys or page size. Use the
    // candidate API so large pages and custom select_keys work identically.
    bool selected = false;
    if (RIME_API_AVAILABLE(rime_, select_candidate_on_current_page))
        selected = rime_->select_candidate_on_current_page(session_, static_cast<size_t>(index));
    else if (RIME_API_AVAILABLE(rime_, select_candidate))
        selected = rime_->select_candidate(session_, absolute_index);
    if (!selected)
        return U"";
    std::u32string result = take_commit();

    // If state is still Composing/Selecting but buffer is empty, clear it.
    // Otherwise keep it so the user can continue typing the remaining input.
    if (state() != ImeState::Inactive && buffer().empty()) {
        rime_->clear_composition(session_);
    }
    return result;
}

void RimeIme::backspace() {
    if (!rime_ || !session_)
        return;
    // Send XK_BackSpace to rime — it deletes one syllable character from the
    // composition (rather than canceling the whole input like cancel() does).
    rime_->process_key(session_, 0xFF08, 0);  // XK_BackSpace
    update_state();
}

void RimeIme::cancel() {
    if (!rime_ || !session_)
        return;

    // Send Escape to cancel
    rime_->process_key(session_, 0xFF1B, 0);  // XK_Escape
    update_state();
    // Always clear composition to ensure clean state
    rime_->clear_composition(session_);
}

void RimeIme::page_up() {
    if (!rime_ || !session_)
        return;
    rime_->process_key(session_, 0xFF55, 0);  // XK_Page_Up
}

void RimeIme::page_down() {
    if (!rime_ || !session_)
        return;
    rime_->process_key(session_, 0xFF56, 0);  // XK_Page_Down
}

bool RimeIme::select_schema(const std::string& schema_id) {
    if (!rime_ || !session_)
        return false;
    return rime_->select_schema(session_, schema_id.c_str());
}

std::vector<std::string> RimeIme::get_schema_list() {
    std::vector<std::string> result;
    if (!rime_)
        return result;

    RimeSchemaList list;
    if (rime_->get_schema_list(&list)) {
        for (size_t i = 0; i < list.size; ++i) {
            result.push_back(list.list[i].schema_id);
        }
        rime_->free_schema_list(&list);
    }
    return result;
}

std::string RimeIme::get_current_schema() {
    if (!rime_ || !session_)
        return "";

    char schema[64];
    if (rime_->get_current_schema(session_, schema, sizeof(schema))) {
        return schema;
    }
    return "";
}

std::string RimeIme::fuzzy_variant(const std::string& schema_id) const {
    // Bundled schemas that ship a fuzzy twin. Anything else keeps its id, so the
    // toggle simply has no effect on it.
    static const std::pair<const char*, const char*> kVariants[] = {
        {"luna_pinyin_simp", "luna_pinyin_simp_fuzzy"},
    };
    if (!fuzzy_ || schema_id.empty())
        return schema_id;
    for (const auto& variant : kVariants) {
        if (schema_id == variant.first)
            return variant.second;
    }
    return schema_id;
}

void RimeIme::set_fuzzy_pinyin(bool on) {
    // Only records the preference: the caller re-selects the schema through
    // fuzzy_variant(), which loads the other prism without any redeploy.
    fuzzy_ = on;
}

void RimeIme::update_state() {
    // State is updated automatically by librime
}

std::u32string RimeIme::utf8_to_utf32(const std::string& utf8) const {
    std::u32string result;
    if (utf8.empty())
        return result;

    const uint8_t* data = reinterpret_cast<const uint8_t*>(utf8.data());
    size_t pos = 0;
    while (pos < utf8.size()) {
        char32_t ch = utf8::decode(data, utf8.size(), pos);
        if (ch != 0) {  // Skip null characters
            result += ch;
        }
    }
    return result;
}
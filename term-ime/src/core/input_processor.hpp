#pragma once

#include <boost/sml.hpp>
#include <vector>
#include <cstdint>
#include <spdlog/spdlog.h>

namespace input_sm {

namespace sml = boost::sml;

// State tags
struct Normal {};
struct Escape {};
struct EscapeCSI {};
struct Prefix {};

// Output result from processing
struct Result {
    std::vector<uint8_t> data;
    bool forward = false;
    bool toggle_mode = false;
    // Paste bytes, including delimiters, bypass all key and IME dispatch.
    bool paste = false;
    bool paste_begin = false;
};

// Events
struct Byte {
    uint8_t value;
    Result* result;
    std::vector<uint8_t>* buffer;
};

// State machine definition
struct Machine {
    auto operator()() const noexcept {
        using namespace sml;

        // Guards
        const auto is_ctrl_a = [](const Byte& e) { return e.value == 1; };
        const auto is_esc = [](const Byte& e) { return e.value == 0x1b; };
        const auto is_csi = [](const Byte& e) { return e.value == '[' || e.value == 'O'; };
        // ECMA-48 CSI grammar: parameter bytes 0x30-0x3F, intermediate bytes
        // 0x20-0x2F, final byte 0x40-0x7E. The old rule (any letter is a
        // terminator) left sequences such as `ESC [ @` stuck in EscapeCSI,
        // swallowing every key that followed (defect 16).
        const auto is_final = [](const Byte& e) {
            const uint8_t v = e.value;
            if (v < 0x40 || v > 0x7e)
                return false;
            const auto& buf = *e.buffer;
            // SS3 (ESC O) is only defined for its own finals — arrows, Home/End
            // and F1-F4 — so a plain letter after `ESC O` must not be mistaken
            // for a completed sequence.
            if (buf.size() >= 2 && buf[1] == 'O') {
                return (v >= 'A' && v <= 'D') || v == 'F' || v == 'H' || (v >= 'P' && v <= 'S');
            }
            return true;
        };
        const auto is_param = [](const Byte& e) {
            // Bound storage without dropping bytes: an oversized sequence is
            // forwarded as an opaque event by the fallback transition below.
            return e.buffer->size() < 256 && e.value >= 0x20 && e.value <= 0x3f;
        };
        const auto is_space = [](const Byte& e) { return e.value == ' '; };

        // Actions
        const auto forward_byte = [](const Byte& e) {
            e.result->data = {e.value};
            e.result->forward = true;
        };

        const auto start_escape = [](const Byte& e) {
            e.buffer->clear();
            e.buffer->push_back(0x1b);
            spdlog::debug("SM: Normal -> Escape");
        };

        const auto buffer_byte = [](const Byte& e) { e.buffer->push_back(e.value); };

        const auto forward_escape = [](const Byte& e) {
            e.result->data = *e.buffer;
            e.result->data.push_back(e.value);
            e.result->forward = true;
            spdlog::debug("SM: Escape -> Normal (forward)");
        };

        const auto complete_escape = [](const Byte& e) {
            e.buffer->push_back(e.value);
            e.result->data = *e.buffer;
            e.result->forward = true;
            spdlog::debug("SM: EscapeCSI -> Normal (complete)");
        };

        const auto toggle_mode = [](const Byte& e) {
            e.result->toggle_mode = true;
            spdlog::debug("SM: Prefix -> Normal (toggle)");
        };

        const auto forward_prefix = [](const Byte& e) {
            e.result->data = {1, e.value};
            e.result->forward = true;
            spdlog::debug("SM: Prefix -> Normal (forward)");
        };

        const auto forward_literal_ctrl_a = [](const Byte& e) {
            e.result->data = {1};
            e.result->forward = true;
            spdlog::debug("SM: Prefix -> Normal (literal Ctrl+A)");
        };

        return make_transition_table(
            // Normal state
            *state<Normal> + event<Byte>[is_ctrl_a] = state<Prefix>,
            state<Normal> + event<Byte>[is_esc] / start_escape = state<Escape>,
            state<Normal> + event<Byte> / forward_byte,

            // Escape state
            state<Escape> + event<Byte>[is_csi] / buffer_byte = state<EscapeCSI>,
            state<Escape> + event<Byte> / forward_escape = state<Normal>,

            // EscapeCSI state
            state<EscapeCSI> + event<Byte>[is_final] / complete_escape = state<Normal>,
            state<EscapeCSI> + event<Byte>[is_param] / buffer_byte,
            // Any other byte cannot extend the sequence: end it here rather than
            // buffering forever (which would swallow the keys that follow).
            state<EscapeCSI> + event<Byte> / complete_escape = state<Normal>,

            // Prefix state
            state<Prefix> + event<Byte>[is_space] / toggle_mode = state<Normal>,
            state<Prefix> + event<Byte>[is_ctrl_a] / forward_literal_ctrl_a = state<Normal>,
            state<Prefix> + event<Byte> / forward_prefix = state<Normal>);
    }
};

}  // namespace input_sm

// Input processor using Boost.SML state machine
class InputProcessor {
   public:
    InputProcessor();

    // Process a byte, return result
    input_sm::Result process(uint8_t byte);

    // Check if currently in escape sequence
    bool in_escape() const;

    // Resolve an escape timeout. Preserve the entire unfinished sequence;
    // only {ESC} is an independent Escape key. Does not flush Ctrl+A prefixes.
    input_sm::Result flush_escape();

    // Reset state
    void reset();

   private:
    boost::sml::sm<input_sm::Machine> sm_;
    std::vector<uint8_t> buffer_;
    bool in_paste_ = false;
    size_t paste_end_matched_ = 0;
};
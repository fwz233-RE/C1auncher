#include "input_processor.hpp"

InputProcessor::InputProcessor() : sm_() {}

input_sm::Result InputProcessor::process(uint8_t byte) {
    input_sm::Result result;
    if (in_paste_) {
        // Stream immediately, with constant storage even for huge/malformed
        // pastes. Only the exact end delimiter exits this opaque-byte mode.
        static constexpr char end[] = "\x1b[201~";
        result.forward = result.paste = true;
        result.data = {byte};
        if (byte == static_cast<uint8_t>(end[paste_end_matched_]))
            ++paste_end_matched_;
        else
            paste_end_matched_ = byte == 0x1b ? 1 : 0;
        if (paste_end_matched_ == sizeof(end) - 1) {
            in_paste_ = false;
            paste_end_matched_ = 0;
        }
        return result;
    }
    input_sm::Byte event{byte, &result, &buffer_};
    sm_.process_event(event);
    static const std::vector<uint8_t> begin{0x1b, '[', '2', '0', '0', '~'};
    if (result.forward && result.data == begin) {
        in_paste_ = true;
        paste_end_matched_ = 0;
        result.paste = result.paste_begin = true;
    }
    return result;
}

bool InputProcessor::in_escape() const {
    using namespace input_sm;
    return sm_.is(boost::sml::state<Escape>) || sm_.is(boost::sml::state<EscapeCSI>);
}

input_sm::Result InputProcessor::flush_escape() {
    input_sm::Result result;
    if (in_escape()) {
        result.forward = true;
        result.data = buffer_;
        reset();
    }
    return result;
}

void InputProcessor::reset() {
    sm_ = boost::sml::sm<input_sm::Machine>{};
    buffer_.clear();
    in_paste_ = false;
    paste_end_matched_ = 0;
}
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// Frames terminal output before it reaches the host terminal. A callback never
// receives half a UTF-8 character or half an escape/control string, so UI output
// can safely be inserted between callbacks. Embedded C0 controls in ESC/CSI
// execute immediately without exposing or discarding the pending sequence.
// Oversized controls are discarded through their terminator (not flushed as
// a dangerous partial sequence).
class OutputStream {
   public:
    static constexpr size_t kMaxControlBytes = 16384;
    size_t buffered_bytes() const { return buffer_.size(); }

    template <class Emit>
    void feed(const char* data, size_t len, Emit emit) {
        size_t i = 0;
        while (i < len) {
            const auto c = static_cast<unsigned char>(data[i]);
            if (state_ == State::Ground) {
                if (c == 0x1b) {
                    buffer_ = "\x1b";
                    state_ = State::Escape;
                    ++i;
                } else if (c < 0x80) {
                    const size_t start = i++;
                    while (i < len && static_cast<unsigned char>(data[i]) < 0x80 && data[i] != '\x1b')
                        ++i;
                    emit(std::string_view(data + start, i - start));
                } else if (c >= 0xc2 && c <= 0xf4) {
                    utf8_need_ = c < 0xe0 ? 2 : (c < 0xf0 ? 3 : 4);
                    buffer_.assign(1, static_cast<char>(c));
                    state_ = State::Utf8;
                    ++i;
                } else {
                    emit(std::string_view("\xef\xbf\xbd", 3));
                    ++i;
                }
                continue;
            }
            if (state_ == State::Utf8) {
                bool valid = (c & 0xc0) == 0x80;
                if (buffer_.size() == 1) {
                    const auto lead = static_cast<unsigned char>(buffer_[0]);
                    valid = valid && !(lead == 0xe0 && c < 0xa0) && !(lead == 0xed && c >= 0xa0) &&
                            !(lead == 0xf0 && c < 0x90) && !(lead == 0xf4 && c >= 0x90);
                }
                if (!valid) {
                    reset();
                    emit(std::string_view("\xef\xbf\xbd", 3));
                    continue;  // reprocess the non-continuation (possibly ESC)
                }
                buffer_.push_back(static_cast<char>(c));
                ++i;
                if (buffer_.size() == utf8_need_)
                    finish(emit);
                continue;
            }

            ++i;
            if (c == 0x18 || c == 0x1a) {  // CAN / SUB abort a control
                reset();
                continue;
            }
            if (state_ == State::String || state_ == State::StringEsc) {
                const bool done = (osc_ && c == 7) || (state_ == State::StringEsc && c == '\\');
                append(c);
                if (done)
                    finish(emit);
                else
                    state_ = c == 0x1b ? State::StringEsc : State::String;
                continue;
            }
            if (c == 0x1b) {  // a new escape aborts an unfinished CSI/escape
                reset();
                buffer_ = "\x1b";
                state_ = State::Escape;
                continue;
            }
            if (c == 0x7f)
                continue;  // DEL is ignored without ending the sequence.
            if (c < 0x20) {
                // Execute embedded C0 immediately, even before a final byte
                // arrives. Keep it out of the pending ESC/CSI so both Parser
                // and Renderer see the same standalone control and sequence.
                emit(std::string_view(data + i - 1, 1));
                continue;
            }
            append(c);
            if (state_ == State::Escape) {
                if (c == '[')
                    state_ = State::Csi;
                else if (c == ']' || c == 'P' || c == 'X' || c == '^' || c == '_') {
                    osc_ = c == ']';
                    state_ = State::String;
                } else if (c >= 0x20 && c <= 0x2f)
                    state_ = State::EscapeIntermediate;
                else
                    finish(emit);
            } else if (state_ == State::EscapeIntermediate) {
                if (c >= 0x30 && c <= 0x7e)
                    finish(emit);
            } else if (state_ == State::Csi && c >= 0x40 && c <= 0x7e) {
                finish(emit);
            }
        }
    }

   private:
    enum class State { Ground, Escape, EscapeIntermediate, Csi, String, StringEsc, Utf8 };
    State state_ = State::Ground;
    std::string buffer_;
    size_t utf8_need_ = 0;
    bool dropped_ = false;
    bool osc_ = false;

    void append(unsigned char c) {
        if (dropped_)
            return;
        if (buffer_.size() >= kMaxControlBytes) {
            buffer_.clear();
            dropped_ = true;
        } else {
            buffer_.push_back(static_cast<char>(c));
        }
    }
    void reset() {
        state_ = State::Ground;
        buffer_.clear();
        dropped_ = false;
    }
    template <class Emit>
    void finish(Emit& emit) {
        if (!dropped_)
            emit(std::string_view(buffer_));
        reset();
    }
};

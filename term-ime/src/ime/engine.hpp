#pragma once

#include <string>
#include <vector>

enum class ImeState { Inactive, Composing, Selecting };

enum class ImeMode {
    Chinese,  // 中文模式 - 小写字母触发 IME
    English   // 英文模式 - 所有输入直接传递
};

struct Candidate {
    std::u32string text;
    std::string code;
};

class ImeEngine {
   public:
    virtual ~ImeEngine() = default;

    virtual bool input(char ch) = 0;
    virtual ImeState state() const = 0;
    virtual ImeMode mode() const = 0;
    virtual void set_mode(ImeMode mode) = 0;
    virtual void toggle_mode() = 0;
    virtual std::string buffer() const = 0;
    virtual std::vector<Candidate> candidates() const = 0;
    virtual std::u32string select(int index) = 0;
    virtual void backspace() = 0;  // delete one syllable char
    virtual void cancel() = 0;     // clear entire composition
    virtual void page_up() = 0;
    virtual void page_down() = 0;
};

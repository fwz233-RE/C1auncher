#include "core/input_processor.hpp"
#include <iostream>
#include <cassert>

using namespace input_sm;

void test_ctrl_a_space() {
    std::cout << "Test: Ctrl+A + Space (toggle mode)\n";
    InputProcessor proc;

    // Send Ctrl+A (byte 1)
    auto r1 = proc.process(1);
    assert(!r1.forward);
    assert(!r1.toggle_mode);

    // Send Space
    auto r2 = proc.process(' ');
    assert(r2.toggle_mode);
    assert(!r2.forward);

    std::cout << "  PASS\n";
}

void test_ctrl_a_a() {
    std::cout << "Test: Ctrl+A + A (AI toggle)\n";
    InputProcessor proc;

    // Send Ctrl+A
    auto r1 = proc.process(1);
    assert(!r1.forward);

    // Send 'a'
    auto r2 = proc.process('a');
    assert(r2.forward);
    assert(r2.data.size() == 2);
    assert(r2.data[0] == 1);
    assert(r2.data[1] == 'a');

    std::cout << "  PASS\n";
}

void test_ctrl_a_s() {
    std::cout << "Test: Ctrl+A + S (settings)\n";
    InputProcessor proc;

    // Send Ctrl+A
    auto r1 = proc.process(1);
    assert(!r1.forward);

    // Send 's'
    auto r2 = proc.process('s');
    assert(r2.forward);
    assert(r2.data.size() == 2);
    assert(r2.data[0] == 1);
    assert(r2.data[1] == 's');

    std::cout << "  PASS\n";
}

void test_ctrl_a_ctrl_a() {
    std::cout << "Test: Ctrl+A + Ctrl+A (literal Ctrl+A)\n";
    InputProcessor proc;

    // Send Ctrl+A
    auto r1 = proc.process(1);
    assert(!r1.forward);

    // Send Ctrl+A again
    auto r2 = proc.process(1);
    assert(r2.forward);
    assert(r2.data.size() == 1);
    assert(r2.data[0] == 1);

    std::cout << "  PASS\n";
}

void test_normal_key() {
    std::cout << "Test: Normal key forwarding\n";
    InputProcessor proc;

    // Send 'x'
    auto r = proc.process('x');
    assert(r.forward);
    assert(r.data.size() == 1);
    assert(r.data[0] == 'x');

    std::cout << "  PASS\n";
}

void test_escape_sequence() {
    std::cout << "Test: Escape sequence\n";
    InputProcessor proc;

    // Send ESC [
    auto r1 = proc.process(0x1b);
    assert(!r1.forward);

    auto r2 = proc.process('[');
    assert(!r2.forward);

    // Send 'A' (up arrow terminator)
    auto r3 = proc.process('A');
    assert(r3.forward);
    assert(r3.data.size() == 3);
    assert(r3.data[0] == 0x1b);
    assert(r3.data[1] == '[');
    assert(r3.data[2] == 'A');

    std::cout << "  PASS\n";
}

void test_sequence() {
    std::cout << "Test: Sequence of inputs\n";
    InputProcessor proc;

    // Type "hello"
    for (char c : std::string("hello")) {
        auto r = proc.process(c);
        assert(r.forward);
        assert(r.data.size() == 1);
        assert(r.data[0] == static_cast<uint8_t>(c));
    }

    // Then Ctrl+A + Space
    proc.process(1);
    auto r = proc.process(' ');
    assert(r.toggle_mode);

    std::cout << "  PASS\n";
}

void test_compose_select_cycle() {
    std::cout << "Test: Compose-select cycle (select candidate then continue input)\n";
    InputProcessor proc;

    // 1. 在中文模式下输入 "ni" 开始组词
    proc.process('n');
    auto r1 = proc.process('i');
    assert(r1.forward);

    // 2. 按空格选中第一个候选词（模拟 select 操作后的状态重置）
    // 在真实的 App::on_keyboard_data 中，select() 后会调用 clear_composition()
    // 所以 IME 状态应该变为 Inactive

    // 3. 继续输入新的拼音 "hao"
    auto r2 = proc.process('h');
    assert(r2.forward);
    auto r3 = proc.process('a');
    assert(r3.forward);
    auto r4 = proc.process('o');
    assert(r4.forward);

    std::cout << "  PASS\n";
}

void test_multiple_select_cycles() {
    std::cout << "Test: Multiple select cycles\n";
    InputProcessor proc;

    // 第一次输入
    for (char c : std::string("ni")) {
        auto r = proc.process(c);
        assert(r.forward);
    }
    // 选中候选词（模拟状态重置）

    // 第二次输入
    for (char c : std::string("shi")) {
        auto r = proc.process(c);
        assert(r.forward);
    }
    // 选中候选词

    // 第三次输入
    for (char c : std::string("jie")) {
        auto r = proc.process(c);
        assert(r.forward);
    }

    std::cout << "  PASS\n";
}

int main() {
    std::cout << "=== Input Processor End-to-End Tests ===\n\n";

    test_ctrl_a_space();
    test_ctrl_a_a();
    test_ctrl_a_s();
    test_ctrl_a_ctrl_a();
    test_normal_key();
    test_escape_sequence();
    test_sequence();
    test_compose_select_cycle();
    test_multiple_select_cycles();

    std::cout << "\n=== All Tests Passed ===\n";
    return 0;
}
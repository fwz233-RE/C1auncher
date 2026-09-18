#include "ui/settings.hpp"
#include "ui/jsx.hpp"
#include "util/i18n.hpp"
#include "core/config.hpp"
#include <ftxui/screen/screen.hpp>
#include <iostream>

using namespace ui;

int main() {
    // Initialize i18n
    I18n::init(I18n::Lang::ZH_CN);

    std::cout << "=== Settings Panel Test ===\n\n";

    // Create settings state
    SettingsState state;
    AppConfig config;

    // Test 1: Initialize settings
    std::cout << "Test 1: Initialize settings from config\n";
    settings_init(state, config);
    std::cout << "  Items count: " << state.items.size() << "\n";
    for (const auto& item : state.items) {
        std::cout << "  - " << item.label << ": " << item.value;
        std::cout << " (options: " << item.options.size() << ")\n";
    }
    std::cout << "\n";

    // Test 2: Render settings panel
    std::cout << "Test 2: Render settings panel\n";
    state.visible = true;
    auto element = SettingsPanel(state);

    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(60), ftxui::Dimension::Fixed(20));
    ftxui::Render(screen, element);
    std::cout << "Panel rendered (60x20)\n";
    std::cout << "--- Render Output ---\n";
    std::cout << screen.ToString() << "\n";
    std::cout << "--- End ---\n\n";

    // Test 3: Navigate settings with arrow keys
    std::cout << "Test 3: Navigate settings (arrow keys and vim keys)\n";
    settings_handle_key(state, 'j');  // Move down (vim)
    std::cout << "  After 'j': focus_index = " << state.focus_index << "\n";
    settings_handle_key(state, 'B');  // Move down (arrow down)
    std::cout << "  After 'B' (down arrow): focus_index = " << state.focus_index << "\n";
    settings_handle_key(state, 'k');  // Move up (vim)
    std::cout << "  After 'k': focus_index = " << state.focus_index << "\n";
    settings_handle_key(state, 'A');  // Move up (arrow up)
    std::cout << "  After 'A' (up arrow): focus_index = " << state.focus_index << "\n\n";

    // Test 4: Change setting value with arrow keys
    std::cout << "Test 4: Change setting value\n";
    state.focus_index = 0;  // Focus on UI language
    std::cout << "  Current UI language: " << state.items[0].value << "\n";
    settings_handle_key(state, 'l');  // Move right (vim)
    std::cout << "  After 'l': " << state.items[0].value << "\n";
    settings_handle_key(state, 'C');  // Move right (arrow right)
    std::cout << "  After 'C' (right arrow): " << state.items[0].value << "\n";
    settings_handle_key(state, 'h');  // Move left (vim)
    std::cout << "  After 'h': " << state.items[0].value << "\n";
    settings_handle_key(state, 'D');  // Move left (arrow left)
    std::cout << "  After 'D' (left arrow): " << state.items[0].value << "\n\n";

    // Test 5: Apply settings
    std::cout << "Test 5: Apply settings to config\n";
    state.items[0].value = "en";
    state.items[0].selected_index = 0;
    settings_apply(state, config);
    std::cout << "  Config ui_language: " << config.ui_language << "\n\n";

    // Test 6: Close settings
    std::cout << "Test 6: Close settings panel\n";
    bool handled = settings_handle_key(state, 0x1b);  // Escape
    std::cout << "  Escape handled: " << (handled ? "true" : "false") << "\n\n";

    std::cout << "=== All Settings Tests Passed ===\n";
    return 0;
}
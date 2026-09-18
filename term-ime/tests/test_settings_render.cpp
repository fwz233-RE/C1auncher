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

    // Create settings state
    SettingsState state;
    AppConfig config;
    settings_init(state, config);
    state.visible = true;

    // Render settings panel
    auto element = SettingsPanel(state);

    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(60), ftxui::Dimension::Fixed(20));
    ftxui::Render(screen, element);

    std::cout << "=== Settings Panel Render Output ===\n";
    std::cout << screen.ToString() << "\n";
    std::cout << "=== End ===\n";

    return 0;
}

#include "ui/jsx.hpp"
#include "ui/components.hpp"
#include "util/i18n.hpp"
#include "ime/engine.hpp"
#include <ftxui/screen/screen.hpp>
#include <iostream>
#include <vector>

using namespace ui;

int main() {
    // Initialize i18n with default translations
    I18n::init(I18n::Lang::ZH_CN);

    std::cout << "=== JSX Style UI Framework Test ===\n\n";

    // Test 1: Basic elements
    std::cout << "Test 1: Basic Elements\n";
    auto text_el = Text("Hello World");
    auto hbox_el = HBox({Text("Left"), Filler(), Text("Right")});
    auto vbox_el = VBox({Text("Top"), Text("Bottom")});

    auto screen1 = ftxui::Screen::Create(ftxui::Dimension::Fixed(40), ftxui::Dimension::Fixed(1));
    ftxui::Render(screen1, hbox_el);
    std::cout << "HBox: " << screen1.ToString() << "\n\n";

    // Test 2: Styled text with decorators
    std::cout << "Test 2: Styled Text\n";
    auto bold_el = Text("Bold Text") | Bold();
    auto dim_el = Text("Dim Text") | Dim();
    auto colored_el = Text("Green Text") | TextColor(FtxuiColor::Green);

    auto screen2 = ftxui::Screen::Create(ftxui::Dimension::Fixed(20), ftxui::Dimension::Fixed(1));
    ftxui::Render(screen2, colored_el);
    std::cout << "Colored: " << screen2.ToString() << "\n\n";

    // Test 3: ModeIndicator component
    std::cout << "Test 3: ModeIndicator Component\n";
    auto mode_cn = ModeIndicator({.mode = "中文", .is_chinese = true});
    auto mode_en = ModeIndicator({.mode = "EN", .is_chinese = false});

    auto screen3 = ftxui::Screen::Create(ftxui::Dimension::Fixed(20), ftxui::Dimension::Fixed(1));
    ftxui::Render(screen3, mode_cn);
    std::cout << "Chinese mode: " << screen3.ToString() << "\n";

    auto screen3b = ftxui::Screen::Create(ftxui::Dimension::Fixed(20), ftxui::Dimension::Fixed(1));
    ftxui::Render(screen3b, mode_en);
    std::cout << "English mode: " << screen3b.ToString() << "\n\n";

    // Test 4: CandidateItem component
    std::cout << "Test 4: CandidateItem Component\n";
    auto item_selected = CandidateItem({.index = 1, .text = U"你好", .selected = true});
    auto item_normal = CandidateItem({.index = 2, .text = U"世界", .selected = false});

    auto screen4 = ftxui::Screen::Create(ftxui::Dimension::Fixed(15), ftxui::Dimension::Fixed(1));
    ftxui::Render(screen4, item_selected);
    std::cout << "Selected: " << screen4.ToString() << "\n";

    auto screen4b = ftxui::Screen::Create(ftxui::Dimension::Fixed(15), ftxui::Dimension::Fixed(1));
    ftxui::Render(screen4b, item_normal);
    std::cout << "Normal: " << screen4b.ToString() << "\n\n";

    // Test 5: Full MainBar component
    std::cout << "Test 5: MainBar Component (Full UI)\n";
    std::vector<Candidate> candidates;
    Candidate c1;
    c1.text = U"你好";
    c1.code = "nihao";
    candidates.push_back(c1);

    Candidate c2;
    c2.text = U"您好";
    c2.code = "nihao";
    candidates.push_back(c2);

    Candidate c3;
    c3.text = U"你好啊";
    c3.code = "nihaoa";
    candidates.push_back(c3);

    auto main_bar =
        MainBar({.mode = "中文", .lang_name = "简体中文", .candidates = candidates, .selected = 0, .buffer = "nihao"});

    auto screen6 = ftxui::Screen::Create(ftxui::Dimension::Fixed(80), ftxui::Dimension::Fixed(1));
    ftxui::Render(screen6, main_bar);
    std::cout << "MainBar: " << screen6.ToString() << "\n\n";

    // Test 6: EmptyBar (no candidates)
    std::cout << "Test 6: EmptyBar Component\n";
    auto empty_bar = EmptyBar({.mode = "EN"});

    auto screen7 = ftxui::Screen::Create(ftxui::Dimension::Fixed(80), ftxui::Dimension::Fixed(1));
    ftxui::Render(screen7, empty_bar);
    std::cout << "EmptyBar: " << screen7.ToString() << "\n\n";

    // Test 7: HintsBar
    std::cout << "Test 7: HintsBar Component\n";
    auto hints = HintsBar();

    auto screen8 = ftxui::Screen::Create(ftxui::Dimension::Fixed(60), ftxui::Dimension::Fixed(1));
    ftxui::Render(screen8, hints);
    std::cout << "HintsBar: " << screen8.ToString() << "\n\n";

    std::cout << "=== All Tests Passed ===\n";
    return 0;
}
#include "ui/components.hpp"
#include <ftxui/screen/screen.hpp>
#include <gtest/gtest.h>

static std::string render_line(const ui::Element& element, int width) {
    auto frame = ftxui::Screen::Create(ftxui::Dimension::Fixed(width), ftxui::Dimension::Fixed(1));
    ftxui::Render(frame, element);
    std::string text;
    for (int col = 0; col < width; ++col)
        text += frame.PixelAt(col, 0).character;
    return text;
}

TEST(CandidateBarRegression, HiddenModeDoesNotLeaveEmptyBrackets) {
    const auto text = render_line(ui::ModeIndicator({.mode = ""}), 20);
    EXPECT_EQ(text.find('['), std::string::npos);
    EXPECT_EQ(text.find(']'), std::string::npos);
}

TEST(CandidateBarRegression, LongPreeditLeavesAVisibleSelectableCandidate) {
    const std::string preedit(200, 'a');
    const std::vector<Candidate> candidates{{U"你好", ""}, {U"您好", ""}};
    const auto fit = ui::FitCandidateBar(20, "拼", preedit, candidates, 9);
    EXPECT_EQ(fit.count, 1);
    ui::MainBarProps props{};
    props.mode = "拼";
    props.candidates = candidates;
    props.buffer = preedit;
    props.term_width = 20;
    const auto text = render_line(ui::MainBar(props), 20);
    EXPECT_NE(text.find("1."), std::string::npos);
    EXPECT_NE(text.find("…"), std::string::npos);
}

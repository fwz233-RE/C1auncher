#include "jsx.hpp"
#include <ftxui/dom/elements.hpp>

namespace ui {

// ============================================================================
// Basic Elements
// ============================================================================

Element Text(const std::string& content) {
    return ftxui::text(content);
}

Element Text(const char* content) {
    return ftxui::text(std::string(content));
}

Element HBox(Elements children) {
    return ftxui::hbox(std::move(children));
}

Element VBox(Elements children) {
    return ftxui::vbox(std::move(children));
}

Element DBox(Elements children) {
    return ftxui::dbox(std::move(children));
}

Element Filler() {
    return ftxui::filler();
}

Element Empty() {
    return ftxui::emptyElement();
}

Element Separator() {
    return ftxui::separator();
}

Element Separator(const std::string& ch) {
    return ftxui::separatorCharacter(ch);
}

// ============================================================================
// Decorators
// ============================================================================

Decorator Bold() {
    return ftxui::bold;
}

Decorator Dim() {
    return ftxui::dim;
}

Decorator Italic() {
    return ftxui::italic;
}

Decorator Inverted() {
    return ftxui::inverted;
}

Decorator Underlined() {
    return ftxui::underlined;
}

Decorator Blink() {
    return ftxui::blink;
}

Decorator FgColor(FtxuiColor c) {
    return ftxui::color(c);
}

Decorator BgColor(FtxuiColor c) {
    return ftxui::bgcolor(c);
}

Decorator TextColor(FtxuiColor c) {
    return FgColor(c);
}

Decorator Width(int value) {
    return ftxui::size(ftxui::WIDTH, ftxui::EQUAL, value);
}

Decorator Height(int value) {
    return ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, value);
}

Decorator MinWidth(int value) {
    return ftxui::size(ftxui::WIDTH, ftxui::GREATER_THAN, value);
}

Decorator MinHeight(int value) {
    return ftxui::size(ftxui::HEIGHT, ftxui::GREATER_THAN, value);
}

Decorator MaxWidth(int value) {
    return ftxui::size(ftxui::WIDTH, ftxui::LESS_THAN, value);
}

Decorator MaxHeight(int value) {
    return ftxui::size(ftxui::HEIGHT, ftxui::LESS_THAN, value);
}

Decorator Flex() {
    return ftxui::flex;
}

Decorator FlexGrow() {
    return ftxui::flex_grow;
}

Decorator FlexShrink() {
    return ftxui::flex_shrink;
}

// ============================================================================
// Styled Text
// ============================================================================

Element StyledText(const std::string& content, FtxuiColor c) {
    return ftxui::text(content) | ftxui::color(c);
}

Element StyledText(const std::string& content, Decorator d) {
    return ftxui::text(content) | d;
}

Element BoldText(const std::string& content) {
    return ftxui::text(content) | ftxui::bold;
}

Element DimText(const std::string& content) {
    return ftxui::text(content) | ftxui::dim;
}

// ============================================================================
// Utility Functions
// ============================================================================

Decorator Chain(Decorator a, Decorator b) {
    return [a, b](Element e) { return b(a(e)); };
}

Decorator If(bool condition, Decorator then_dec, Decorator else_dec) {
    if (condition) {
        return then_dec;
    }
    return else_dec ? else_dec : [](Element e) { return e; };
}

// ============================================================================
// Animation Support
// ============================================================================

const std::vector<std::string>& SpinnerFrames() {
    static const std::vector<std::string> frames = {"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"};
    return frames;
}

std::string SpinnerFrame(size_t index) {
    const auto& frames = SpinnerFrames();
    return frames[index % frames.size()];
}

Element ProgressBar(float progress, int width) {
    progress = std::max(0.0f, std::min(1.0f, progress));
    int filled = static_cast<int>(progress * width);
    int empty = width - filled;

    // Use ASCII characters for progress bar
    std::string bar = std::string(filled, '#') + std::string(empty, '-');
    return ftxui::text(bar);
}

}  // namespace ui

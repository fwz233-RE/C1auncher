#include "components.hpp"
#include "../util/utf8.hpp"
#include "../util/i18n.hpp"
#include <ftxui/dom/elements.hpp>

namespace ui {

// ============================================================================
// Helper Functions
// ============================================================================

namespace {

// Refined 24-bit color palette - green status bar background.
namespace color {
// Status bar background - deep green
const FtxuiColor kBarBg = FtxuiColor::Palette256::DarkGreen;
// Mode indicator text - bright white on green
const FtxuiColor kMode = FtxuiColor::White;
const FtxuiColor kBracket = FtxuiColor::Palette256::DarkSeaGreen;
// Pinyin buffer - bright sky blue
const FtxuiColor kPinyin = FtxuiColor::Palette256::DeepSkyBlue1;
// Candidates - warm gold
const FtxuiColor kCandidate = FtxuiColor::Palette256::Gold1;
// Selected candidate - bright green highlight
const FtxuiColor kSelectedBg = FtxuiColor::Palette256::SpringGreen1;
const FtxuiColor kSelectedFg = FtxuiColor::Palette256::DarkGreen;
// Hints - light green-gray
const FtxuiColor kHint = FtxuiColor::Palette256::DarkSeaGreen;
const FtxuiColor kSeparator = FtxuiColor::Palette256::DarkSeaGreen4;
}  // namespace color

std::string u32_to_utf8(const std::u32string& s) {
    std::string result;
    for (char32_t c : s) {
        if (c != 0) {
            result += utf8::encode(c);
        }
    }
    return result;
}

// Single mode color (no per-language switching).
FtxuiColor GetModeColor(const std::string& /*mode*/) {
    return color::kMode;
}

}  // namespace

// ============================================================================
// ModeIndicator
// ============================================================================

Element ModeIndicator(const ModeIndicatorProps& props) {
    if (props.mode.empty())
        return Text("");
    return HBox({Text(" [") | Dim() | TextColor(color::kBracket), Text(props.mode) | Bold() | TextColor(color::kMode),
                 Text("] ") | Dim() | TextColor(color::kBracket)});
}

// ============================================================================
// CandidateItem
// ============================================================================

namespace {

// Cut `s` to at most `cols` display columns, appending '…' when it was cut.
// Used for the single-candidate fallback so nothing ever overflows the line.
std::string truncate_to_cols(const std::string& s, int cols) {
    if (cols <= 0)
        return "";
    if (utf8::string_width(s) <= cols)
        return s;
    std::string out;
    int w = 0;
    size_t pos = 0;
    while (pos < s.size()) {
        int len = utf8::char_len(static_cast<uint8_t>(s[pos]));
        if (len < 1)
            len = 1;
        if (pos + static_cast<size_t>(len) > s.size())
            break;
        size_t seq = 0;
        char32_t ch = utf8::decode(reinterpret_cast<const uint8_t*>(s.data()) + pos, len, seq);
        int cw = utf8::width(ch);
        if (w + cw > cols - 1)
            break;  // keep one column for the ellipsis
        out.append(s, pos, static_cast<size_t>(len));
        w += cw;
        pos += static_cast<size_t>(len);
    }
    return out + "…";
}

}  // namespace

Element CandidateItem(const CandidateItemProps& props) {
    std::string text_str = u32_to_utf8(props.text);
    // A candidate longer than its slot is cut with an ellipsis below, so no
    // per-item scrolling is needed here.
    std::string display_text = text_str;
    if (props.max_text_width > 0) {
        display_text = truncate_to_cols(display_text, props.max_text_width);
    }

    std::string label = std::to_string(props.index) + "." + display_text;

    if (props.selected) {
        return Text(" " + label + " ") | Bold() | BgColor(color::kSelectedBg) | TextColor(color::kSelectedFg);
    } else {
        return Text(" " + label + " ") | TextColor(color::kCandidate);
    }
}

// ============================================================================
// CandidateBar
// ============================================================================

Element CandidateBar(const CandidateBarProps& props) {
    Elements items;

    // Mode indicator
    bool is_chinese = props.mode.find("拼") != std::string::npos;
    items.push_back(ModeIndicator({.mode = props.mode, .is_chinese = is_chinese}));

    // Pinyin buffer - bright blue, no underline
    items.push_back(Text(" " + props.buffer + " ") | TextColor(color::kPinyin));

    // Candidates
    for (size_t i = 0; i < props.candidates.size() && i < 9; ++i) {
        items.push_back(CandidateItem(
            {.index = static_cast<int>(i + 1), .text = props.candidates[i].text, .selected = (i == props.selected)}));
    }

    return HBox(std::move(items)) | BgColor(color::kBarBg) | Height(1);
}

// ============================================================================
// EmptyBar
// ============================================================================

Element EmptyBar(const EmptyBarProps& props) {
    bool is_chinese = props.mode.find("拼") != std::string::npos;

    Elements items;

    // Mode indicator
    items.push_back(ModeIndicator({.mode = props.mode, .is_chinese = is_chinese}));

    // Filler
    items.push_back(Filler());

    // Hints
    items.push_back(HintsBar());

    return HBox(std::move(items)) | BgColor(color::kBarBg) | Height(1);
}

// ============================================================================
// StatusBar
// ============================================================================

Element StatusBar(const StatusBarProps& props) {
    Elements items;

    // Language and mode
    items.push_back(Text(" [" + props.lang_name + " " + props.mode + "]") | Bold());

    return HBox(std::move(items)) | TextColor(GetModeColor(props.mode));
}

// ============================================================================
// HintsBar
// ============================================================================

Element HintItem(const HintItemProps& props) {
    return Text(" " + props.key + " " + props.action + " ") | Dim() | TextColor(color::kHint);
}

Element HintsBar() {
    return HBox({HintItem({.key = "^A Space", .action = I18n::t("hint.toggle_mode")}),
                 Text("|") | TextColor(color::kHint), HintItem({.key = "^A S", .action = I18n::t("settings.title")})});
}

// ============================================================================
// MainBar
// ============================================================================

// Fixed part of the bar: " [mode] " (1+1+w+1+1) + " buffer " (1+w+1).
// Item overhead: " N." + " " — use the widest (selected) form for every item so
// moving the selection can never invalidate the budget.
static int bar_fixed_width(const std::string& mode, const std::string& buffer) {
    return (mode.empty() ? 0 : 4 + utf8::string_width(mode)) + 2 + utf8::string_width(buffer);
}
static constexpr int kBarItemOverhead = 6;
static std::string bar_preedit(int cols, const std::string& mode, const std::string& buffer) {
    const int mode_cols = mode.empty() ? 0 : 4 + utf8::string_width(mode);
    // Reserve space for at least one selectable candidate, rather than letting
    // a long preedit push every candidate outside the physical terminal.
    return truncate_to_cols(buffer, std::max(0, cols - mode_cols - 2 - kBarItemOverhead - 2));
}

CandidateBarFit FitCandidateBar(int term_width, const std::string& mode, const std::string& buffer,
                                const std::vector<Candidate>& candidates, int max_items) {
    CandidateBarFit fit;
    if (term_width <= 0)
        term_width = 80;
    if (max_items > 9)
        max_items = 9;  // selector keys are single digits
    if (max_items < 1)
        max_items = 1;

    const int fixed = bar_fixed_width(mode, bar_preedit(term_width, mode, buffer));
    const int limit = std::min(static_cast<int>(candidates.size()), max_items);
    if (limit <= 0) {
        fit.count = 0;
        return fit;
    }

    // Prefer full texts: the largest count whose items fit untruncated.
    for (int count = limit; count >= 1; --count) {
        int total = fixed;
        for (int i = 0; i < count; ++i)
            total += kBarItemOverhead + utf8::string_width(u32_to_utf8(candidates[i].text));
        if (total <= term_width) {
            fit.count = count;
            return fit;  // text_cols stays 0 = keep the whole text
        }
    }

    // Nothing fits as-is (tiny terminal / very long preedit): one candidate with
    // a truncated text beats a line that runs past the right edge.
    fit.count = 1;
    fit.text_cols = std::max(2, term_width - fixed - kBarItemOverhead);
    return fit;
}

Element MainBar(const MainBarProps& props) {
    // 即使没有候选词，也要显示拼音（buffer 可能非空）
    if (props.candidates.empty() && props.buffer.empty()) {
        return EmptyBar({.mode = props.mode});
    }

    int term_w = props.term_width;
    if (term_w <= 0)
        term_w = 80;

    const std::string displayed_buffer = bar_preedit(term_w, props.mode, props.buffer);

    // The logical selection window and rendered bar share the same geometry.
    // Shared with App::render_candidates_bar() so the drawn set and the
    // digit-selectable set are always the same set.
    ui::CandidateBarFit fit = FitCandidateBar(term_w, props.mode, props.buffer, props.candidates, props.max_items);
    size_t display_count = static_cast<size_t>(fit.count);

    // ---- Build items ----
    Elements items;

    // Mode indicator
    bool is_chinese = props.mode.find("拼") != std::string::npos;
    items.push_back(ModeIndicator({.mode = props.mode, .is_chinese = is_chinese}));

    // Pinyin buffer - bright blue, no underline
    items.push_back(Text(" " + displayed_buffer + " ") | TextColor(color::kPinyin));

    // Candidates
    for (size_t i = 0; i < display_count; ++i) {
        items.push_back(CandidateItem({.index = static_cast<int>(i + 1),
                                       .text = props.candidates[i].text,
                                       .selected = (i == props.selected),
                                       .max_text_width = fit.text_cols}));
    }

    // Filler
    items.push_back(Filler());

    // Cancel hint
    return HBox(std::move(items)) | BgColor(color::kBarBg) | Height(1);
}

}  // namespace ui

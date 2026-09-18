#pragma once

#include "jsx.hpp"
#include "../ime/engine.hpp"
#include <string>
#include <vector>

// UI Components for term-ime
// Each component is a function that takes props and returns an Element

namespace ui {

// ============================================================================
// Candidate Bar Components
// ============================================================================

// Props for ModeIndicator
struct ModeIndicatorProps {
    std::string mode;
    bool is_chinese = false;
};

Element ModeIndicator(const ModeIndicatorProps& props);

// Props for CandidateItem
struct CandidateItemProps {
    int index;
    std::u32string text;
    bool selected = false;
    // Max display columns for the text; 0 = unlimited. Set by the bar so a wide
    // candidate is cut with an ellipsis instead of overflowing the line.
    int max_text_width = 0;
};

Element CandidateItem(const CandidateItemProps& props);

// Props for CandidateBar
struct CandidateBarProps {
    std::vector<Candidate> candidates;
    size_t selected = 0;
    std::string buffer;
    std::string mode;
};

Element CandidateBar(const CandidateBarProps& props);

// Props for EmptyBar (shown when no candidates)
struct EmptyBarProps {
    std::string mode;
};

Element EmptyBar(const EmptyBarProps& props);

// ============================================================================
// Status Bar Components
// ============================================================================

// Props for StatusBar
struct StatusBarProps {
    std::string mode;
    std::string lang_name;
};

Element StatusBar(const StatusBarProps& props);

// ============================================================================
// Hints Components
// ============================================================================

struct HintItemProps {
    std::string key;
    std::string action;
};

Element HintItem(const HintItemProps& props);

Element HintsBar();

// ============================================================================
// Main UI Component
// ============================================================================

// Props for MainBar (combines status + candidates)
struct MainBarProps {
    std::string mode;
    std::string lang_name;
    std::vector<Candidate> candidates;
    size_t selected = 0;
    std::string buffer;
    int term_width = 80;  // Terminal width in columns
    int max_items = 9;    // Upper bound on candidates drawn (width may fit fewer)
};

Element MainBar(const MainBarProps& props);

// ---- Candidate bar geometry ------------------------------------------------
// One bar line is " [mode] " + " buffer " + N candidate items (" N.text "), so
// how many candidates are visible depends on the terminal width, the preedit
// length and the candidate texts. The renderer and App's key/paging logic both
// use this function, so the visible set and the selectable set cannot disagree
// (previously the bar silently hid candidates that the digit keys could still
// reach, and rime's page size skipped them on page-down).
struct CandidateBarFit {
    int count = 1;      // candidates that fit on the line
    int text_cols = 0;  // per-candidate text budget in columns; 0 = keep full text
};

CandidateBarFit FitCandidateBar(int term_width, const std::string& mode, const std::string& buffer,
                                const std::vector<Candidate>& candidates, int max_items);

}  // namespace ui

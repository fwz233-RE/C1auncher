#pragma once

#include <ftxui/dom/elements.hpp>
#include <string>
#include <vector>
#include <functional>

// JSX-style UI framework for FTXUI
// Provides declarative, component-based UI construction

namespace ui {

// Re-export FTXUI types for convenience
using Element = ftxui::Element;
using Elements = ftxui::Elements;
using Decorator = ftxui::Decorator;
using FtxuiColor = ftxui::Color;

// ============================================================================
// Basic Elements (wrappers around FTXUI)
// ============================================================================

// Text element
Element Text(const std::string& content);
Element Text(const char* content);

// Layout containers
Element HBox(Elements children);
Element VBox(Elements children);
Element DBox(Elements children);

// Flexible space
Element Filler();

// Empty element
Element Empty();

// Separator
Element Separator();
Element Separator(const std::string& ch);

// ============================================================================
// Decorators (styling)
// ============================================================================

Decorator Bold();
Decorator Dim();
Decorator Italic();
Decorator Inverted();
Decorator Underlined();
Decorator Blink();

// Color decorators
Decorator FgColor(FtxuiColor c);
Decorator BgColor(FtxuiColor c);
Decorator TextColor(FtxuiColor c);  // Alias for FgColor

// Size decorators
Decorator Width(int value);
Decorator Height(int value);
Decorator MinWidth(int value);
Decorator MinHeight(int value);
Decorator MaxWidth(int value);
Decorator MaxHeight(int value);

// Flex decorators
Decorator Flex();
Decorator FlexGrow();
Decorator FlexShrink();

// ============================================================================
// Styled Text (combines text with decorators)
// ============================================================================

Element StyledText(const std::string& content, FtxuiColor c);
Element StyledText(const std::string& content, Decorator d);
Element BoldText(const std::string& content);
Element DimText(const std::string& content);

// ============================================================================
// Utility Functions
// ============================================================================

// Chain decorators
Decorator Chain(Decorator a, Decorator b);

// Conditional decorator
Decorator If(bool condition, Decorator then_dec, Decorator else_dec = nullptr);

// ============================================================================
// Component System
// ============================================================================

// Props base struct
struct Props {};

// Component function type
template <typename P>
using ComponentFunc = std::function<Element(const P&)>;

// ============================================================================
// Animation Support
// ============================================================================

// Spinner frames
const std::vector<std::string>& SpinnerFrames();

// Get current spinner frame
std::string SpinnerFrame(size_t index);

// Progress bar
Element ProgressBar(float progress, int width = 20);

}  // namespace ui

#pragma once

#include <string>
#include <vector>
#include <unordered_map>

// Kaomoji (emoticon) entry
struct Kaomoji {
    std::string text;               // The kaomoji itself
    std::string name;               // Name/description
    std::vector<std::string> tags;  // Search tags
};

// Kaomoji library manager
class KaomojiLib {
   public:
    KaomojiLib();

    // Initialize with default kaomoji
    void initialize();

    // Search kaomoji by keyword
    std::vector<Kaomoji> search(const std::string& keyword) const;

    // Get kaomoji by category
    std::vector<Kaomoji> by_category(const std::string& category) const;

    // Get all categories
    std::vector<std::string> categories() const;

    // Get random kaomoji
    Kaomoji random() const;

    // Add custom kaomoji
    void add(const Kaomoji& kaomoji);

    // Load from file
    bool load(const std::string& path);

    // Save to file
    bool save(const std::string& path) const;

   private:
    std::vector<Kaomoji> kaomojis_;
    std::unordered_map<std::string, std::vector<size_t>> tag_index_;
    std::unordered_map<std::string, std::vector<size_t>> category_index_;

    void build_index();
    void add_default_kaomojis();
};

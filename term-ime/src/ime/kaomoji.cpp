#include "kaomoji.hpp"
#include <algorithm>
#include <random>
#include <fstream>
#include <spdlog/spdlog.h>

KaomojiLib::KaomojiLib() = default;

void KaomojiLib::initialize() {
    add_default_kaomojis();
    build_index();
    spdlog::info("KaomojiLib initialized with {} kaomojis", kaomojis_.size());
}

void KaomojiLib::add_default_kaomojis() {
    // Happy / Joy
    kaomojis_.push_back({"(◕‿◕)", "happy", {"happy", "joy", "smile", "开心", "高兴"}});
    kaomojis_.push_back({"(≧◡≦)", "very happy", {"happy", "joy", "excited", "开心", "兴奋"}});
    kaomojis_.push_back({"(｡◕‿◕｡)", "cute happy", {"happy", "cute", "开心", "可爱"}});
    kaomojis_.push_back({"ヽ(>∀<☆)ノ", "excited", {"excited", "happy", "兴奋", "开心"}});
    kaomojis_.push_back({"(*≧ω≦*)", "blush happy", {"happy", "blush", "cute", "开心", "害羞"}});

    // Love / Affection
    kaomojis_.push_back({"(´,,•ω•,,)♡", "love", {"love", "heart", "爱", "喜欢"}});
    kaomojis_.push_back({"(♡ω♡)", "love", {"love", "heart", "爱"}});
    kaomojis_.push_back({"(◕‿◕)♡", "love you", {"love", "heart", "爱"}});
    kaomojis_.push_back({"♡(◡‿◡)", "sweet love", {"love", "sweet", "爱", "甜蜜"}});
    kaomojis_.push_back({"( ˘ ³˘)♥", "kiss", {"kiss", "love", "亲亲", "爱"}});

    // Sad / Crying
    kaomojis_.push_back({"(╥﹏╥)", "crying", {"sad", "cry", "哭", "难过"}});
    kaomojis_.push_back({"(´;ω;`)", "sobbing", {"sad", "cry", "哭"}});
    kaomojis_.push_back({"(T_T)", "tears", {"sad", "cry", "哭"}});
    kaomojis_.push_back({"(｡•́︿•̀｡)", "sad", {"sad", "upset", "难过"}});
    kaomojis_.push_back({"(╯_╰)", "disappointed", {"sad", "disappointed", "失望"}});

    // Angry / Frustrated
    kaomojis_.push_back({"(╬ Ò﹏Ó)", "angry", {"angry", "mad", "生气", "愤怒"}});
    kaomojis_.push_back({"(ノಠ益ಠ)ノ", "rage", {"angry", "rage", "愤怒"}});
    kaomojis_.push_back({"(╯°□°）╯︵ ┻━┻", "flip table", {"angry", "flip", "rage", "掀桌", "愤怒"}});
    kaomojis_.push_back({"┻━┻ ︵ヽ(°□° )ノ︵ ┻━┻", "double flip", {"angry", "flip", "掀桌"}});

    // Surprised / Shocked
    kaomojis_.push_back({"(°o°)", "surprised", {"surprised", "shock", "惊讶"}});
    kaomojis_.push_back({"(⊙_⊙)", "shocked", {"shocked", "surprised", "震惊"}});
    kaomojis_.push_back({"Σ(°△°|||)", "very shocked", {"shocked", "surprised", "震惊"}});
    kaomojis_.push_back({"(ﾟДﾟ)", "what", {"shocked", "what", "什么"}});

    // Cute / Shy
    kaomojis_.push_back({"(⁄ ⁄•⁄ω⁄•⁄ ⁄)", "shy", {"shy", "blush", "害羞"}});
    kaomojis_.push_back({"(◕ᴗ◕✿)", "cute", {"cute", "sweet", "可爱"}});
    kaomojis_.push_back({"(｡・//ε//・｡)", "embarrassed", {"shy", "embarrassed", "害羞", "尴尬"}});
    kaomojis_.push_back({"(⁄ ⁄>⁄ ▽ ⁄<⁄ ⁄)", "blushing", {"blush", "shy", "脸红", "害羞"}});

    // Greeting / Waving
    kaomojis_.push_back({"(◕‿◕)ﾉ", "hello", {"hello", "wave", "greet", "你好", "打招呼"}});
    kaomojis_.push_back({"＼(≧▽≦)／", "yay", {"yay", "celebrate", "欢呼"}});
    kaomojis_.push_back({"( •̀ω•́ )σ", "pointing", {"point", "look", "看", "指"}});

    // Animal themed
    kaomojis_.push_back({"(=^･ω･^=)", "cat", {"cat", "animal", "猫"}});
    kaomojis_.push_back({"(=｀ω´=)", "grumpy cat", {"cat", "grumpy", "猫", "生气"}});
    kaomojis_.push_back({"ʕ•ᴥ•ʔ", "bear", {"bear", "animal", "熊"}});
    kaomojis_.push_back({"(・ω・)", "small animal", {"animal", "cute", "动物"}});
    kaomojis_.push_back({"ヾ(•ω•`)o", "waving animal", {"animal", "wave", "动物"}});

    // Action
    kaomojis_.push_back({"(☞ﾟヮﾟ)☞", "point right", {"point", "right", "指"}});
    kaomojis_.push_back({"☜(ﾟヮﾟ☜)", "point left", {"point", "left", "指"}});
    kaomojis_.push_back({"(づ￣ ³￣)づ", "hug", {"hug", "love", "抱抱"}});
    kaomojis_.push_back({"(￣▽￣)ノ", "bye", {"bye", "wave", "再见"}});

    // Neutral / Thinking
    kaomojis_.push_back({"(￣～￣)", "thinking", {"think", "hmm", "思考"}});
    kaomojis_.push_back({"(・_・;)", "unsure", {"unsure", "confused", "不确定"}});
    kaomojis_.push_back({"(ー_ー)!!", "sweat", {"sweat", "awkward", "汗"}});
    kaomojis_.push_back({"¯\\_(ツ)_/¯", "shrug", {"shrug", "idk", "摊手", "不知道"}});

    // Sleepy
    kaomojis_.push_back({"(－ω－) zzZ", "sleeping", {"sleep", "tired", "睡觉", "困"}});
    kaomojis_.push_back({"(｡-ω-)zzz", "sleepy", {"sleep", "tired", "困"}});
    kaomojis_.push_back({"(๑ᵕ⌄ᵕ๑)", "peaceful", {"peaceful", "sleep", "安详"}});

    // Cool / Smug
    kaomojis_.push_back({"(￣▽￣)", "smug", {"smug", "cool", "得意"}});
    kaomojis_.push_back({"(⌐■_■)", "cool", {"cool", "sunglasses", "酷"}});
    kaomojis_.push_back({"( ͡° ͜ʖ ͡°)", "lenny", {"lenny", "smug", "有趣"}});
}

void KaomojiLib::build_index() {
    tag_index_.clear();
    category_index_.clear();

    for (size_t i = 0; i < kaomojis_.size(); ++i) {
        const auto& k = kaomojis_[i];

        // Index by tags
        for (const auto& tag : k.tags) {
            tag_index_[tag].push_back(i);
        }

        // Index by name (as category)
        category_index_[k.name].push_back(i);
    }
}

std::vector<Kaomoji> KaomojiLib::search(const std::string& keyword) const {
    std::vector<Kaomoji> results;
    std::vector<size_t> indices;

    // Search by tag
    auto it = tag_index_.find(keyword);
    if (it != tag_index_.end()) {
        indices = it->second;
    }

    // Also search by name
    auto it2 = category_index_.find(keyword);
    if (it2 != category_index_.end()) {
        for (size_t idx : it2->second) {
            if (std::find(indices.begin(), indices.end(), idx) == indices.end()) {
                indices.push_back(idx);
            }
        }
    }

    // If no exact match, do partial search
    if (indices.empty()) {
        for (size_t i = 0; i < kaomojis_.size(); ++i) {
            const auto& k = kaomojis_[i];
            for (const auto& tag : k.tags) {
                if (tag.find(keyword) != std::string::npos) {
                    indices.push_back(i);
                    break;
                }
            }
        }
    }

    for (size_t idx : indices) {
        results.push_back(kaomojis_[idx]);
    }

    return results;
}

std::vector<Kaomoji> KaomojiLib::by_category(const std::string& category) const {
    std::vector<Kaomoji> results;
    auto it = category_index_.find(category);
    if (it != category_index_.end()) {
        for (size_t idx : it->second) {
            results.push_back(kaomojis_[idx]);
        }
    }
    return results;
}

std::vector<std::string> KaomojiLib::categories() const {
    std::vector<std::string> cats;
    for (const auto& [name, _] : category_index_) {
        cats.push_back(name);
    }
    return cats;
}

Kaomoji KaomojiLib::random() const {
    if (kaomojis_.empty()) {
        return {"(◕‿◕)", "happy", {"happy"}};
    }
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<size_t> dist(0, kaomojis_.size() - 1);
    return kaomojis_[dist(gen)];
}

void KaomojiLib::add(const Kaomoji& kaomoji) {
    kaomojis_.push_back(kaomoji);
    build_index();
}

bool KaomojiLib::load(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return false;
    }

    // TODO: Load from JSON file
    return true;
}

bool KaomojiLib::save(const std::string& path) const {
    std::ofstream file(path);
    if (!file.is_open()) {
        return false;
    }

    // TODO: Save to JSON file
    return true;
}

#pragma once

#include <fcitx-utils/utf8.h>
#include <cstdint>
#include <string_view>

namespace wetype {

enum class TextLanguage { Unknown, Latin, Cjk };

inline bool isCjk(std::uint32_t c) {
    return (c >= 0x3400 && c <= 0x4DBF) || (c >= 0x4E00 && c <= 0x9FFF) ||
           (c >= 0xF900 && c <= 0xFAFF) || (c >= 0x20000 && c <= 0x2FA1F) ||
           (c >= 0x30000 && c <= 0x323AF) ||
           (c >= 0x3000 && c <= 0x30FF) || (c >= 0x3100 && c <= 0x312F) ||
           (c >= 0x31A0 && c <= 0x31BF) || (c >= 0x31F0 && c <= 0x31FF) ||
           (c >= 0x1100 && c <= 0x11FF) || (c >= 0x3130 && c <= 0x318F) ||
           (c >= 0xA960 && c <= 0xA97F) || (c >= 0xAC00 && c <= 0xD7AF) ||
           (c >= 0xD7B0 && c <= 0xD7FF) || (c >= 0xFF00 && c <= 0xFFEF);
}

// Unknown context remains unchanged; never guess from an invalid UTF-8 string.
inline TextLanguage endingLanguage(std::string_view text) {
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' ||
                             text.back() == '\n' || text.back() == '\r')) {
        text.remove_suffix(1);
    }
    if (text.empty() || !fcitx::utf8::validate(text)) return TextLanguage::Unknown;
    return isCjk(fcitx::utf8::getLastChar(text)) ? TextLanguage::Cjk : TextLanguage::Latin;
}

inline std::string_view punctuation(char key, TextLanguage language) {
    if (language == TextLanguage::Cjk) return key == ',' ? "，" : "。";
    return key == ',' ? "," : ".";
}

} // namespace wetype

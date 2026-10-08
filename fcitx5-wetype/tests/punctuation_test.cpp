#include "punctuation.h"

#include <iostream>
#include <stdexcept>
#include <utility>

int main() {
    using wetype::TextLanguage;
    try {
        const std::pair<std::string_view, TextLanguage> cases[] = {
            {"", TextLanguage::Unknown}, {" \t\r\n", TextLanguage::Unknown},
            {"hello", TextLanguage::Latin}, {"nihao", TextLanguage::Latin},
            {"123", TextLanguage::Latin}, {"café", TextLanguage::Latin},
            {"你好", TextLanguage::Cjk}, {"中文 \n", TextLanguage::Cjk},
            {"𠀀", TextLanguage::Cjk}, {"かな", TextLanguage::Cjk},
            {"한글", TextLanguage::Cjk}, {"，。", TextLanguage::Cjk},
            {std::string_view("\xF0\x80\x80\x80", 4), TextLanguage::Unknown},
            {std::string_view("\xE4\xBD", 2), TextLanguage::Unknown},
        };
        for (const auto &[text, language] : cases) {
            if (wetype::endingLanguage(text) != language) {
                throw std::runtime_error("unexpected language classification");
            }
        }
        if (wetype::punctuation(',', TextLanguage::Cjk) != "，" ||
            wetype::punctuation('.', TextLanguage::Cjk) != "。" ||
            wetype::punctuation(',', TextLanguage::Latin) != "," ||
            wetype::punctuation('.', TextLanguage::Unknown) != ".") {
            throw std::runtime_error("unexpected punctuation width");
        }
        std::cout << "PASS punctuation: Latin, CJK/extension, invalid UTF-8 and unknown context\n";
    } catch (const std::exception &error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}

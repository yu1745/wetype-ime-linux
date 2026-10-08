#include "glossary.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

static void require(bool ok, const char *message) {
    if (!ok) throw std::runtime_error(message);
}

int main() {
    char name[] = "/tmp/wetype-glossary-test.XXXXXX";
    const char *dir = ::mkdtemp(name);
    if (!dir) return 1;
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::filesystem::remove_all(path); }
    } cleanup{dir};
    try {
        const std::string path = (cleanup.path / "glossary.tsv").string();
        const std::string longGloss(900, 'x');
        {
            std::ofstream file(path);
            file << "# comment\n\nmissing-tab\n\tempty-key\n"
                 << "你好\tn. hello\n世界\tn. world\r\n"
                 << "世界\tn. planet\r\n"
                 << "前缀\te.g. example\n"
                 << "长词\t" << longGloss << "\tignored translation\n"
                 << "多列\tv. run\tsecond translation\n"
                 << "空释义\t\n只有词性\tn. \n"
                 << "末尾\tlast";
        }
        wetype::Glossary glossary;
        require(glossary.load(path) == 6, "unexpected entry count");
        const auto expect = [&](const char *word, const std::string &gloss) {
            const auto *found = glossary.lookup(word);
            require(found && *found == gloss, "wrong glossary translation");
        };
        expect("你好", "hello");
        expect("世界", "planet");
        expect("前缀", "e.g. example");
        expect("长词", longGloss);
        expect("多列", "run");
        expect("末尾", "last");
        require(!glossary.lookup("不存在"), "unknown word should not match");
        require(!glossary.lookup("空释义"), "empty gloss should not match");
        require(!glossary.lookup("只有词性"), "POS-only gloss should not match");
        require(glossary.load(path + ".missing") == 0, "missing file should be optional");
        require(!glossary.lookup("你好"), "reload should not retain old entries");
        std::cout << "PASS glossary parsing, long lines, CRLF, duplicates, optional file\n";
    } catch (const std::exception &error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}

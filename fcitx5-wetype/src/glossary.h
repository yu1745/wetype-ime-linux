#pragma once

#include <fstream>
#include <string>
#include <unordered_map>
#include <utility>

namespace wetype {

// Optional, display-only TSV dictionary: word<TAB>[part-of-speech. ]gloss.
// Compatible with qingjian's glossary-en.tsv; no data is bundled here.
class Glossary {
public:
    std::size_t load(const std::string &path) {
        entries_.clear();
        std::ifstream input(path);
        std::string line;
        while (std::getline(input, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty() || line.front() == '#') continue;
            const auto tab = line.find('\t');
            if (tab == std::string::npos || tab == 0) continue;
            const auto nextTab = line.find('\t', tab + 1);
            std::string gloss = line.substr(tab + 1, nextTab == std::string::npos
                                                       ? std::string::npos
                                                       : nextTab - tab - 1);
            // Strip only recognized POS prefixes, not prose such as "e.g. ...".
            static const char *prefixes[] = {
                "n. ", "v. ", "a. ", "adj. ", "adv. ", "pron. ", "prep. ",
                "conj. ", "int. ", "num. ", "art. ", "det. ", "aux. ",
                "vt. ", "vi. ", "interj. "};
            for (const char *prefix : prefixes) {
                if (gloss.rfind(prefix, 0) == 0) {
                    gloss.erase(0, std::char_traits<char>::length(prefix));
                    break;
                }
            }
            if (!gloss.empty()) entries_[line.substr(0, tab)] = std::move(gloss);
        }
        return entries_.size();
    }

    const std::string *lookup(const std::string &word) const {
        const auto it = entries_.find(word);
        return it == entries_.end() ? nullptr : &it->second;
    }

private:
    std::unordered_map<std::string, std::string> entries_;
};

} // namespace wetype

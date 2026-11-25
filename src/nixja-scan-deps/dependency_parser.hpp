#pragma once

#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Decode Makefile escaping in the output of clang-scan-deps.
inline std::vector<std::string> make_words(std::string_view text) {
    std::vector<std::string> words;
    std::string word;
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '\\' && i + 1 < text.size()) {
            word += text[++i];
        } else if (c == '$' && i + 1 < text.size() && text[i + 1] == '$') {
            word += '$';
            ++i;
        } else if (c == ' ' || c == '\t' || c == '\r') {
            if (!word.empty()) { words.push_back(std::move(word)); word.clear(); }
        } else {
            word += c;
        }
    }
    if (!word.empty()) words.push_back(std::move(word));
    return words;
}

inline std::unordered_map<std::string, std::unordered_set<std::string>>
parse_makefile(std::string content) {
    std::unordered_map<std::string, std::unordered_set<std::string>> graph;
    content = std::regex_replace(content, std::regex(R"(\\\r?\n)"), " ");
    std::istringstream stream(content);
    std::string line;
    while (std::getline(stream, line)) {
        size_t colon = 0;
        for (; colon < line.size(); ++colon) {
            if (line[colon] == '\\') { ++colon; continue; }
            if (line[colon] == ':') break;
        }
        if (colon >= line.size()) continue;
        const auto targets = make_words(std::string_view(line).substr(0, colon));
        const auto dependencies = make_words(std::string_view(line).substr(colon + 1));
        for (const auto& target : targets)
            graph[target].insert(dependencies.begin(), dependencies.end());
    }
    return graph;
}


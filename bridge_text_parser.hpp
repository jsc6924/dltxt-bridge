#pragma once

#include <boost/asio.hpp>

#include <nlohmann/json.hpp>

#include <optional>
#include <regex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "bridge_documents.hpp"

namespace bridge_text {

namespace net = boost::asio;
using json = nlohmann::json;

constexpr char16_t kCornerOpenQuote = u'\u300E';
constexpr char16_t kCornerCloseQuote = u'\u300F';
constexpr char16_t kKagiOpenQuote = u'\u300C';
constexpr char16_t kKagiCloseQuote = u'\u300D';
constexpr char16_t kChineseOpenQuote = u'\u201C';
constexpr char16_t kChineseCloseQuote = u'\u201D';

struct RegexConfig {
    std::string original_prefix_regex;
    std::string translated_prefix_regex;
    std::string other_prefix_regex;
    std::string original_white_regex;
    std::string translated_white_regex;
    std::string original_suffix_regex;
    std::string translated_suffix_regex;
};

struct MatchedGroups {
    std::u16string prefix;
    std::u16string white;
    std::u16string text;
    std::u16string suffix;
};

struct ParsedPair {
    MatchedGroups original;
    MatchedGroups translated;
    std::size_t original_line_index = 0;
    std::size_t translated_line_index = 0;
};

struct ParsedTranslatedLine {
    MatchedGroups translated;
    std::size_t translated_line_index = 0;
};

inline RegexConfig regex_config_from_json(const json& payload) {
    if (!payload.is_object()) {
        throw std::invalid_argument("Parser regex payload must be an object");
    }

    const auto require_string = [&payload](const char* key) -> std::string {
        if (!payload.contains(key) || !payload[key].is_string()) {
            throw std::invalid_argument(std::string{"Parser regex payload is missing string field: "} + key);
        }
        return payload[key].get<std::string>();
    };

    return RegexConfig{
        require_string("originalPrefixRegex"),
        require_string("translatedPrefixRegex"),
        payload.value("otherPrefixRegex", std::string{}),
        payload.value("originalWhiteRegex", std::string{}),
        payload.value("translatedWhiteRegex", std::string{}),
        payload.value("originalSuffixRegex", std::string{}),
        payload.value("translatedSuffixRegex", std::string{}),
    };
}

inline RegexConfig regex_config_from_response(const json& response) {
    if (!response.is_object() || !response.contains("result")) {
        throw std::invalid_argument("Parser regex response must contain a result object");
    }

    if (response["result"].is_null()) {
        throw std::runtime_error("Parser regex response returned null result");
    }

    return regex_config_from_json(response["result"]);
}

class StandardTextParser {
    std::regex original_line_regex_;
    std::regex translated_line_regex_;

    static std::regex build_line_regex(const std::string& prefix, const std::string& white, const std::string& suffix) {
        return std::regex(
            "^(" + prefix + ")(" + white + ")(.*?)(" + suffix + ")$",
            std::regex::ECMAScript);
    }

    static std::u16string trim_right(std::u16string text) {
        while (!text.empty()) {
            const char16_t ch = text.back();
            if (ch != u' ' && ch != u'\t' && ch != u'\r' && ch != u'\n') {
                break;
            }
            text.pop_back();
        }
        return text;
    }

    static std::optional<MatchedGroups> match_line(const std::u16string& line, const std::regex& regex) {
        const std::string utf8_line = bridge_documents::utf16_to_utf8(line);
        std::smatch match;
        if (!std::regex_match(utf8_line, match, regex) || match.size() != 5) {
            return std::nullopt;
        }

        return MatchedGroups{
            bridge_documents::utf8_to_utf16(match[1].str()),
            bridge_documents::utf8_to_utf16(match[2].str()),
            bridge_documents::utf8_to_utf16(match[3].str()),
            bridge_documents::utf8_to_utf16(match[4].str()),
        };
    }

    static bool contains(std::u16string_view text, char16_t value) {
        return text.find(value) != std::u16string_view::npos;
    }

    static std::pair<bool, bool> check_valid(std::u16string_view text) {
        std::vector<std::size_t> stack;

        for (std::size_t index = 0; index < text.size(); ++index) {
            const char16_t ch = text[index];
            if (ch == kCornerOpenQuote) {
                stack.push_back(index);
            } else if (ch == kCornerCloseQuote) {
                if (!stack.empty()) {
                    const std::size_t start = stack.back();
                    stack.pop_back();
                    if (start == 0 && index == text.size() - 1) {
                        return {true, true};
                    }
                } else if (index == text.size() - 1) {
                    return {false, true};
                }
            }
        }

        return {(!stack.empty() && stack.front() == 0), false};
    }

    static void adjust(MatchedGroups& original, MatchedGroups& translated) {
        if (contains(original.white, kKagiOpenQuote) || contains(original.suffix, kKagiCloseQuote) || original.text.empty()) {
            return;
        }

        if (original.text.front() != kCornerOpenQuote && original.text.back() != kCornerCloseQuote) {
            return;
        }

        const auto [move_prefix, move_suffix] = check_valid(original.text);
        if (!move_prefix && !move_suffix) {
            return;
        }

        if (move_prefix) {
            original.white.push_back(kCornerOpenQuote);
            original.text.erase(original.text.begin());
        }

        if (move_suffix && !original.text.empty()) {
            original.suffix.insert(original.suffix.begin(), kCornerCloseQuote);
            original.text.pop_back();
        }

        if (move_prefix && !translated.text.empty()) {
            const char16_t first = translated.text.front();
            if (first == kCornerOpenQuote || first == kChineseOpenQuote) {
                translated.white.push_back(first);
                translated.text.erase(translated.text.begin());
            }
        }

        if (move_suffix && !translated.text.empty()) {
            const char16_t last = translated.text.back();
            if (last == kCornerCloseQuote || last == kChineseCloseQuote) {
                translated.suffix.insert(translated.suffix.begin(), last);
                translated.text.pop_back();
            }
        }
    }

public:
    explicit StandardTextParser(const RegexConfig& config)
        : original_line_regex_(build_line_regex(
            config.original_prefix_regex,
            config.original_white_regex,
            config.original_suffix_regex)),
          translated_line_regex_(build_line_regex(
            config.translated_prefix_regex,
            config.translated_white_regex,
            config.translated_suffix_regex)) {}

    template <typename SendRequest>
    static net::awaitable<StandardTextParser> create_from_request_sender(SendRequest send_request) {
        const json response = co_await send_request("dltxt/get_parser_regex", json::object());
        co_return StandardTextParser(regex_config_from_response(response));
    }

    std::vector<ParsedPair> parse_paired_lines(std::u16string_view text) const {
        return parse_paired_lines(bridge_documents::split_lines(text));
    }

    std::vector<ParsedPair> parse_paired_lines(const std::vector<std::u16string>& lines) const {
        std::vector<ParsedPair> parsed;
        std::optional<MatchedGroups> pending_original;
        std::size_t pending_original_line = 0;

        for (std::size_t line_index = 0; line_index < lines.size(); ++line_index) {
            const std::u16string line = trim_right(lines[line_index]);
            if (auto original = match_line(line, original_line_regex_)) {
                if (pending_original.has_value()) {
                    throw std::runtime_error("Unmatched original line before next original line");
                }

                pending_original = std::move(*original);
                pending_original_line = line_index;
                continue;
            }

            if (!pending_original.has_value()) {
                continue;
            }

            if (auto translated = match_line(line, translated_line_regex_)) {
                adjust(*pending_original, *translated);
                parsed.push_back(ParsedPair{
                    std::move(*pending_original),
                    std::move(*translated),
                    pending_original_line,
                    line_index,
                });
                pending_original.reset();
            }
        }

        if (pending_original.has_value()) {
            throw std::runtime_error("Dangling original line without matching translation");
        }

        return parsed;
    }

    std::vector<ParsedTranslatedLine> parse_translated_lines(std::u16string_view text) const {
        return parse_translated_lines(bridge_documents::split_lines(text));
    }

    std::vector<ParsedTranslatedLine> parse_translated_lines(const std::vector<std::u16string>& lines) const {
        std::vector<ParsedTranslatedLine> parsed;
        for (std::size_t line_index = 0; line_index < lines.size(); ++line_index) {
            const std::u16string line = trim_right(lines[line_index]);
            if (auto translated = match_line(line, translated_line_regex_)) {
                parsed.push_back(ParsedTranslatedLine{std::move(*translated), line_index});
            }
        }
        return parsed;
    }
};

}  // namespace bridge_text
#pragma once

#include <boost/asio.hpp>
#include <boost/regex.hpp>

#include <nlohmann/json.hpp>

#include <optional>
#include <stdexcept>
#include <memory>
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
    boost::wregex original_line_regex_;
    boost::wregex translated_line_regex_;

    static std::wstring wstring_from_u16(std::u16string_view text) {
        std::wstring result;
        result.reserve(text.size());

        if constexpr (sizeof(wchar_t) == sizeof(char16_t)) {
            for (const char16_t ch : text) {
                result.push_back(static_cast<wchar_t>(ch));
            }
            return result;
        }

        for (std::size_t index = 0; index < text.size(); ++index) {
            const char16_t ch = text[index];
            if (ch >= 0xD800 && ch <= 0xDBFF && index + 1 < text.size()) {
                const char16_t low = text[index + 1];
                if (low >= 0xDC00 && low <= 0xDFFF) {
                    const std::uint32_t codepoint =
                        0x10000u + ((static_cast<std::uint32_t>(ch) - 0xD800u) << 10u)
                        + (static_cast<std::uint32_t>(low) - 0xDC00u);
                    result.push_back(static_cast<wchar_t>(codepoint));
                    ++index;
                    continue;
                }
            }
            result.push_back(static_cast<wchar_t>(ch));
        }

        return result;
    }

    static std::u16string u16_from_wstring(std::wstring_view text) {
        std::u16string result;
        result.reserve(text.size());

        if constexpr (sizeof(wchar_t) == sizeof(char16_t)) {
            for (const wchar_t ch : text) {
                result.push_back(static_cast<char16_t>(ch));
            }
            return result;
        }

        for (const wchar_t ch : text) {
            const std::uint32_t codepoint = static_cast<std::uint32_t>(ch);
            if (codepoint <= 0xFFFFu) {
                result.push_back(static_cast<char16_t>(codepoint));
                continue;
            }

            const std::uint32_t value = codepoint - 0x10000u;
            result.push_back(static_cast<char16_t>(0xD800u + (value >> 10u)));
            result.push_back(static_cast<char16_t>(0xDC00u + (value & 0x3FFu)));
        }

        return result;
    }

    static std::wstring wstring_from_utf8(const std::string& text) {
        return wstring_from_u16(bridge_documents::utf8_to_utf16(text));
    }

    static boost::wregex build_line_regex(const std::string& prefix, const std::string& white, const std::string& suffix) {
        return boost::wregex(
            wstring_from_utf8("^(?<prefix>" + prefix + ")(?<white>" + white + ")(?<text>.*?)(?<suffix>" + suffix + ")$"),
            boost::regex_constants::perl);
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

    static std::optional<MatchedGroups> match_line(const std::u16string& line, const boost::wregex& regex) {
        const std::wstring wide_line = wstring_from_u16(line);
        boost::wsmatch match;
        if (!boost::regex_match(wide_line, match, regex)) {
            return std::nullopt;
        }

        const auto prefix = match[std::wstring{L"prefix"}];
        const auto white = match[std::wstring{L"white"}];
        const auto text = match[std::wstring{L"text"}];
        const auto suffix = match[std::wstring{L"suffix"}];
        if (!prefix.matched || !white.matched || !text.matched || !suffix.matched) {
            return std::nullopt;
        }

        return MatchedGroups{
            u16_from_wstring(prefix.str()),
            u16_from_wstring(white.str()),
            u16_from_wstring(text.str()),
            u16_from_wstring(suffix.str()),
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
        auto lines = bridge_documents::split_lines(text);
        for (std::size_t index = 0; index < lines.size(); ++index) {
        }
        return parse_paired_lines(lines);
    }

    std::vector<ParsedPair> parse_paired_lines(const std::vector<std::u16string>& lines) const {
        std::vector<ParsedPair> parsed;
        std::optional<MatchedGroups> pending_original;
        std::size_t pending_original_line = 0;

        for (std::size_t line_index = 0; line_index < lines.size(); ++line_index) {
            const std::u16string line = trim_right(lines[line_index]);
            if (auto original = match_line(line, original_line_regex_)) {
                // Prefer the most recent original line when data is incomplete or malformed.
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

struct ParserState {
    bridge_text::RegexConfig config;
    std::string stamp;
};

inline std::shared_ptr<StandardTextParser> &global_text_parser() {
    static std::shared_ptr<StandardTextParser> parser;
    return parser;
}

inline void set_global_text_parser(std::shared_ptr<StandardTextParser> parser) {
    global_text_parser() = std::move(parser);
}

}  // namespace bridge_text
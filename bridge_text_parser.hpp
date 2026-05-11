#pragma once

#include <boost/asio.hpp>
#include <boost/regex.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
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

struct TextBlockConfig {
    std::string pattern;
    std::string original_prefix_regex;
    std::string translated_prefix_regex;
    std::string original_white_regex;
    std::string translated_white_regex;
    std::string original_suffix_regex;
    std::string translated_suffix_regex;
};

enum class ParserType {
    Standard,
    TextBlock,
};

struct ParserConfig {
    ParserType type = ParserType::Standard;
    std::optional<RegexConfig> standard;
    std::optional<TextBlockConfig> text_block;

        ParserConfig() = default;

        ParserConfig(const RegexConfig& config)
                : type(ParserType::Standard),
                    standard(config) {}

        ParserConfig(RegexConfig&& config)
                : type(ParserType::Standard),
                    standard(std::move(config)) {}

        ParserConfig(const TextBlockConfig& config)
                : type(ParserType::TextBlock),
                    text_block(config) {}

        ParserConfig(TextBlockConfig&& config)
                : type(ParserType::TextBlock),
                    text_block(std::move(config)) {}
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

inline TextBlockConfig text_block_config_from_json(const json& payload) {
    if (!payload.is_object()) {
        throw std::invalid_argument("Parser text-block payload must be an object");
    }

    return TextBlockConfig{
        payload.value("pattern", std::string{}),
        payload.value("originalPrefixRegex", std::string{}),
        payload.value("translatedPrefixRegex", std::string{}),
        payload.value("originalWhiteRegex", std::string{}),
        payload.value("translatedWhiteRegex", std::string{}),
        payload.value("originalSuffixRegex", std::string{}),
        payload.value("translatedSuffixRegex", std::string{}),
    };
}

inline ParserType parser_type_from_string(std::string_view type) {
    if (type == "text-block") {
        return ParserType::TextBlock;
    }
    if (type == "standard") {
        return ParserType::Standard;
    }
    throw std::invalid_argument("Unsupported parser type: " + std::string(type));
}

inline std::string parser_type_to_string(ParserType type) {
    switch (type) {
    case ParserType::Standard:
        return "standard";
    case ParserType::TextBlock:
        return "text-block";
    }

    throw std::invalid_argument("Unsupported parser type enum");
}

inline ParserConfig parser_config_from_json(const json& payload) {
    if (!payload.is_object()) {
        throw std::invalid_argument("Parser config payload must be an object");
    }

    // Backward compatibility for the older flat standard-parser payload.
    if (!payload.contains("parserType")) {
        ParserConfig config;
        config.type = ParserType::Standard;
        config.standard = regex_config_from_json(payload);
        return config;
    }

    if (!payload["parserType"].is_string()) {
        throw std::invalid_argument("Parser config payload must contain string field: parserType");
    }

    const ParserType type = parser_type_from_string(payload["parserType"].get<std::string>());
    const json parser_payload = payload.value("parserConfig", json::object());

    ParserConfig config;
    config.type = type;
    if (type == ParserType::Standard) {
        config.standard = regex_config_from_json(parser_payload);
    } else {
        config.text_block = text_block_config_from_json(parser_payload);
        if (!config.text_block->pattern.size()) {
            throw std::invalid_argument("Parser text-block payload is missing string field: pattern");
        }
    }
    return config;
}

inline ParserConfig parser_config_from_response(const json& response) {
    if (!response.is_object() || !response.contains("result")) {
        throw std::invalid_argument("Parser regex response must contain a result object");
    }

    if (response["result"].is_null()) {
        throw std::runtime_error("Parser regex response returned null result");
    }

    return parser_config_from_json(response["result"]);
}

namespace detail {

inline std::wstring wstring_from_u16(std::u16string_view text) {
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

inline std::u16string u16_from_wstring(std::wstring_view text) {
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

inline std::wstring wstring_from_utf8(const std::string& text) {
    return wstring_from_u16(bridge_documents::utf8_to_utf16(text));
}

inline boost::wregex build_line_regex(const std::string& prefix, const std::string& white, const std::string& suffix) {
    return boost::wregex(
        wstring_from_utf8("^(?<prefix>" + prefix + ")(?<white>" + white + ")(?<text>.*?)(?<suffix>" + suffix + ")$"),
        boost::regex_constants::perl);
}

inline std::u16string trim_right(std::u16string text) {
    while (!text.empty()) {
        const char16_t ch = text.back();
        if (ch != u' ' && ch != u'\t' && ch != u'\r' && ch != u'\n') {
            break;
        }
        text.pop_back();
    }
    return text;
}

inline std::optional<MatchedGroups> match_line(const std::u16string& line, const boost::wregex& regex) {
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

inline bool contains(std::u16string_view text, char16_t value) {
    return text.find(value) != std::u16string_view::npos;
}

inline std::pair<bool, bool> check_valid(std::u16string_view text) {
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

inline void adjust(MatchedGroups& original, MatchedGroups& translated) {
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

inline std::vector<std::size_t> build_line_starts(std::wstring_view text) {
    std::vector<std::size_t> line_starts;
    line_starts.push_back(0);
    for (std::size_t index = 1; index < text.size(); ++index) {
        if (text[index - 1] == L'\n') {
            line_starts.push_back(index);
        }
    }
    return line_starts;
}

inline std::size_t query_line_number(const std::vector<std::size_t>& line_starts, std::size_t index) {
    const auto upper = std::upper_bound(line_starts.begin(), line_starts.end(), index);
    if (upper == line_starts.begin()) {
        return 0;
    }
    return static_cast<std::size_t>(std::distance(line_starts.begin(), upper) - 1);
}

template <typename Iterator>
inline std::size_t count_newlines(Iterator begin, Iterator end) {
    return static_cast<std::size_t>(std::count(begin, end, L'\n'));
}

}  // namespace detail

class ITextParser {
public:
    virtual ~ITextParser() = default;

    virtual std::vector<ParsedPair> parse_paired_lines(std::u16string_view text) const = 0;
    virtual std::vector<ParsedTranslatedLine> parse_translated_lines(std::u16string_view text) const = 0;
};

class StandardTextParser final : public ITextParser {
    boost::wregex original_line_regex_;
    boost::wregex translated_line_regex_;

public:
    explicit StandardTextParser(const RegexConfig& config)
        : original_line_regex_(detail::build_line_regex(
            config.original_prefix_regex,
            config.original_white_regex,
            config.original_suffix_regex)),
          translated_line_regex_(detail::build_line_regex(
            config.translated_prefix_regex,
            config.translated_white_regex,
            config.translated_suffix_regex)) {}

    template <typename SendRequest>
    static net::awaitable<StandardTextParser> create_from_request_sender(SendRequest send_request) {
        const json response = co_await send_request("dltxt/get_parser_regex", json::object());
        const ParserConfig config = parser_config_from_response(response);
        if (config.type != ParserType::Standard || !config.standard.has_value()) {
            throw std::invalid_argument("Runtime parser config is not a standard parser");
        }
        co_return StandardTextParser(*config.standard);
    }

    std::vector<ParsedPair> parse_paired_lines(std::u16string_view text) const override {
        return parse_paired_lines(bridge_documents::split_lines(text));
    }

    std::vector<ParsedPair> parse_paired_lines(const std::vector<std::u16string>& lines) const {
        std::vector<ParsedPair> parsed;
        std::optional<MatchedGroups> pending_original;
        std::size_t pending_original_line = 0;

        for (std::size_t line_index = 0; line_index < lines.size(); ++line_index) {
            const std::u16string line = detail::trim_right(lines[line_index]);
            if (auto original = detail::match_line(line, original_line_regex_)) {
                pending_original = std::move(*original);
                pending_original_line = line_index;
                continue;
            }

            if (!pending_original.has_value()) {
                continue;
            }

            if (auto translated = detail::match_line(line, translated_line_regex_)) {
                detail::adjust(*pending_original, *translated);
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

    std::vector<ParsedTranslatedLine> parse_translated_lines(std::u16string_view text) const override {
        return parse_translated_lines(bridge_documents::split_lines(text));
    }

    std::vector<ParsedTranslatedLine> parse_translated_lines(const std::vector<std::u16string>& lines) const {
        std::vector<ParsedTranslatedLine> parsed;
        for (std::size_t line_index = 0; line_index < lines.size(); ++line_index) {
            const std::u16string line = detail::trim_right(lines[line_index]);
            if (auto translated = detail::match_line(line, translated_line_regex_)) {
                parsed.push_back(ParsedTranslatedLine{std::move(*translated), line_index});
            }
        }
        return parsed;
    }
};

class TextBlockParser final : public ITextParser {
    boost::wregex block_regex_;
    boost::wregex original_line_regex_;
    boost::wregex translated_line_regex_;

public:
    explicit TextBlockParser(const TextBlockConfig& config)
        : block_regex_(
              detail::wstring_from_utf8(config.pattern),
              boost::regex_constants::perl | boost::regex_constants::no_mod_s),
          original_line_regex_(detail::build_line_regex(
              config.original_prefix_regex,
              config.original_white_regex,
              config.original_suffix_regex)),
          translated_line_regex_(detail::build_line_regex(
              config.translated_prefix_regex,
              config.translated_white_regex,
              config.translated_suffix_regex)) {}

    std::vector<ParsedPair> parse_paired_lines(std::u16string_view text) const override {
        std::vector<ParsedPair> parsed;
        const std::wstring wide_text = detail::wstring_from_u16(text);
        const std::vector<std::size_t> line_starts = detail::build_line_starts(wide_text);
        const std::wstring jp_name = L"jp";
        const std::wstring cn_name = L"cn";

        for (boost::wsregex_iterator it(wide_text.begin(), wide_text.end(), block_regex_), end; it != end; ++it) {
            const boost::wsmatch& match = *it;
            const auto jp_match = match[jp_name];
            const auto cn_match = match[cn_name];
            if (!jp_match.matched || !cn_match.matched) {
                continue;
            }

            auto original = detail::match_line(detail::u16_from_wstring(jp_match.str()), original_line_regex_);
            auto translated = detail::match_line(detail::u16_from_wstring(cn_match.str()), translated_line_regex_);
            if (!original.has_value() || !translated.has_value()) {
                continue;
            }

            detail::adjust(*original, *translated);
            const std::size_t block_line = detail::query_line_number(line_starts, static_cast<std::size_t>(match.position()));
            const std::size_t original_line = block_line + detail::count_newlines(match[0].first, jp_match.first);
            const std::size_t translated_line = block_line + detail::count_newlines(match[0].first, cn_match.first);

            parsed.push_back(ParsedPair{
                std::move(*original),
                std::move(*translated),
                original_line,
                translated_line,
            });
        }

        return parsed;
    }

    std::vector<ParsedTranslatedLine> parse_translated_lines(std::u16string_view text) const override {
        const std::vector<ParsedPair> pairs = parse_paired_lines(text);
        std::vector<ParsedTranslatedLine> parsed;
        parsed.reserve(pairs.size());
        for (const auto& pair : pairs) {
            parsed.push_back(ParsedTranslatedLine{pair.translated, pair.translated_line_index});
        }
        return parsed;
    }
};

inline std::shared_ptr<ITextParser> make_text_parser(const ParserConfig& config) {
    if (config.type == ParserType::TextBlock) {
        if (!config.text_block.has_value()) {
            throw std::invalid_argument("Text-block parser config is missing text-block settings");
        }
        return std::make_shared<TextBlockParser>(*config.text_block);
    }

    if (!config.standard.has_value()) {
        throw std::invalid_argument("Standard parser config is missing regex settings");
    }
    return std::make_shared<StandardTextParser>(*config.standard);
}

template <typename SendRequest>
net::awaitable<std::shared_ptr<ITextParser>> create_text_parser_from_request_sender(SendRequest send_request) {
    const json response = co_await send_request("dltxt/get_parser_regex", json::object());
    co_return make_text_parser(parser_config_from_response(response));
}

struct ParserState {
    bridge_text::ParserConfig config;
    std::string stamp;
};

inline std::shared_ptr<ITextParser>& global_text_parser() {
    static std::shared_ptr<ITextParser> parser;
    return parser;
}

inline void set_global_text_parser(std::shared_ptr<ITextParser> parser) {
    global_text_parser() = std::move(parser);
}

}  // namespace bridge_text
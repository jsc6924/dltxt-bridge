#pragma once

#include <boost/asio/thread_pool.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "batch.hpp"
#include "bridge_documents.hpp"
#include "bridge_protocol.hpp"
#include "bridge_similarity_index.hpp"
#include "bridge_text_parser.hpp"

namespace bridge_crossref {

using ParserState = bridge_text::ParserState;

struct LineInfo {
    std::string file_name;
    std::string file_base_name;
    std::size_t line_number = 0;
    std::u16string jp_line;
    std::u16string tr_line;
};

struct LineSearchResult {
    LineInfo line_info;
    int score = 0;
};

class CrossrefService {

    struct BuildState {
        bridge_similarity::SimilarityIndex index;
        std::unordered_map<bridge_similarity::LineId, LineInfo> line_infos;
        std::unordered_map<std::string, std::vector<bridge_similarity::LineId>> file_line_ids;
        std::unordered_map<std::string, int> open_document_versions;
        std::unordered_map<std::string, std::filesystem::file_time_type> disk_file_timestamps;
        std::vector<std::filesystem::path> workspace_root_paths;
        std::unordered_set<std::string> indexed_file_paths;
        bridge_similarity::LineId next_line_id = 1;
    };

    mutable std::mutex mutex_;
    mutable std::condition_variable rebuild_cv_;
    bridge_similarity::SimilarityIndex index_;
    std::unordered_map<bridge_similarity::LineId, LineInfo> line_infos_;
    std::unordered_map<std::string, std::vector<bridge_similarity::LineId>> file_line_ids_;
    std::unordered_map<std::string, int> open_document_versions_;
    std::unordered_map<std::string, std::filesystem::file_time_type> disk_file_timestamps_;
    std::vector<std::filesystem::path> workspace_root_paths_;
    std::unordered_set<std::string> indexed_file_paths_;
    std::optional<ParserState> parser_state_;
    std::function<void()> index_ready_notifier_;
    std::uint64_t rebuild_generation_ = 0;
    std::uint64_t applied_generation_ = 0;
    bridge_similarity::LineId next_line_id_ = 1;

    static std::string path_to_utf8(const std::filesystem::path& path) {
#ifdef _WIN32
        return bridge_documents::utf16_to_utf8(path.generic_u16string());
#else
        return path.generic_string();
#endif
    }

    static std::string file_name_to_utf8(const std::filesystem::path& path) {
#ifdef _WIN32
        return bridge_documents::utf16_to_utf8(path.filename().generic_u16string());
#else
        return path.filename().generic_string();
#endif
    }

    static std::string extension_to_utf8(const std::filesystem::path& path) {
#ifdef _WIN32
        return bridge_documents::utf16_to_utf8(path.extension().generic_u16string());
#else
        return path.extension().generic_string();
#endif
    }

    static bool is_hidden_name(const std::filesystem::path& path) {
        const auto name = path.filename().native();
        using PathChar = std::filesystem::path::value_type;
        return !name.empty() && name.front() == static_cast<PathChar>('.');
    }

    static bool is_archive_directory_name(const std::filesystem::path& path) {
        const std::string extension = extension_to_utf8(path);
        return extension == ".zip" || extension == ".ZIP"
            || extension == ".rar" || extension == ".RAR"
            || extension == ".7z" || extension == ".7Z";
    }

    static bool should_skip_directory(const std::filesystem::directory_entry& entry, const std::filesystem::path& path) {
        if (is_hidden_name(path) || is_archive_directory_name(path)) {
            return true;
        }

        std::error_code symlink_error;
        return entry.is_symlink(symlink_error) && !symlink_error;
    }

    static bool is_text_file(const std::filesystem::path& path) {
        const std::string extension = extension_to_utf8(path);
        return extension == ".txt" || extension == ".TXT";
    }

    static bool is_supported_uri(std::string_view uri) {
        if (uri.rfind("file://", 0) != 0) {
            return false;
        }

        return is_text_file(bridge_documents::file_path_from_uri(uri));
    }

    static bool is_supported_path(const std::filesystem::path& path) {
        return is_text_file(path);
    }

    static boost::asio::thread_pool& search_thread_pool() {
        static boost::asio::thread_pool pool((std::max)(2u, std::thread::hardware_concurrency()));
        return pool;
    }

    static std::u16string remove_space(std::u16string_view text) {
        std::u16string normalized;
        normalized.reserve(text.size());

        for (char16_t ch : text) {
            if (ch == u' ' || ch == u'\t' || ch == u'\r' || ch == u'\n' || ch == u'\f' || ch == u'\v' || ch == u'\u3000') {
                continue;
            }
            normalized.push_back(ch);
        }

        return normalized;
    }

    static int compute_score(std::u16string_view query, std::u16string_view text) {
        const std::u16string lhs = remove_space(query);
        const std::u16string rhs = remove_space(text);
        if (lhs == rhs) {
            return 100;
        }
        if (lhs.empty() && rhs.empty()) {
            return 100;
        }

        std::vector<std::size_t> previous(rhs.size() + 1);
        std::vector<std::size_t> current(rhs.size() + 1);

        for (std::size_t index = 0; index <= rhs.size(); ++index) {
            previous[index] = index;
        }

        for (std::size_t row = 0; row < lhs.size(); ++row) {
            current[0] = row + 1;
            for (std::size_t column = 0; column < rhs.size(); ++column) {
                const std::size_t cost = lhs[row] == rhs[column] ? 0 : 1;
                current[column + 1] = (std::min)({
                    previous[column + 1] + 1,
                    current[column] + 1,
                    previous[column] + cost,
                });
            }
            previous.swap(current);
        }

        const std::size_t total_length = lhs.size() + rhs.size();
        if (total_length == 0) {
            return 100;
        }

        const double similarity = 1.0 - static_cast<double>(previous.back()) / static_cast<double>(total_length);
        return static_cast<int>(std::lround(100.0 * similarity));
    }

    static int score_upper_bound_by_length(std::u16string_view query, std::u16string_view text) {
        const std::u16string lhs = remove_space(query);
        const std::u16string rhs = remove_space(text);
        if (lhs == rhs) {
            return 100;
        }

        const std::size_t total_length = lhs.size() + rhs.size();
        if (total_length == 0) {
            return 100;
        }

        const std::size_t min_distance = lhs.size() > rhs.size() ? lhs.size() - rhs.size() : rhs.size() - lhs.size();
        const double similarity = 1.0 - static_cast<double>(min_distance) / static_cast<double>(total_length);
        return static_cast<int>(std::lround(100.0 * similarity));
    }

    static std::vector<std::string> collect_workspace_text_uris(const std::vector<std::string>& workspace_folders) {
        std::vector<std::string> uris;

        for (const auto& workspace_folder_uri : workspace_folders) {
            std::error_code error;
            const std::filesystem::path workspace_path = bridge_documents::file_path_from_uri(workspace_folder_uri);
            if (!std::filesystem::exists(workspace_path, error) || error) {
                continue;
            }

            std::filesystem::recursive_directory_iterator iterator(
                workspace_path,
                std::filesystem::directory_options::skip_permission_denied,
                error);
            std::filesystem::recursive_directory_iterator end;
            std::size_t visited_entries = 0;

            while (!error && iterator != end) {
                const std::filesystem::directory_entry entry = *iterator;
                const std::filesystem::path current_path = entry.path();

                ++visited_entries;
                if ((visited_entries % 256U) == 0U) {
                    fprintf(stderr,
                        "[debug] crossref: scanning workspace root=%s visited_entries=%zu current=%s\n",
                        path_to_utf8(workspace_path).c_str(),
                        visited_entries,
                        path_to_utf8(current_path).c_str());
                }

                if (entry.is_directory(error)) {
                    if (!error && should_skip_directory(entry, current_path)) {
                        iterator.disable_recursion_pending();
                    }
                    iterator.increment(error);
                    continue;
                }

                if (!error && entry.is_regular_file(error) && !is_hidden_name(current_path) && is_text_file(current_path)) {
                    try {
                        uris.push_back(bridge_documents::file_uri_from_path(current_path));
                    } catch (const std::exception& ex) {
                        fprintf(stderr,
                            "[debug] crossref: skip path due to uri conversion failure path=%s error=%s\n",
                            path_to_utf8(current_path).c_str(),
                            ex.what());
                    } catch (...) {
                        fprintf(stderr,
                            "[debug] crossref: skip path due to unknown uri conversion failure path=%s\n",
                            path_to_utf8(current_path).c_str());
                    }
                }

                error.clear();
                iterator.increment(error);
            }

            if (error) {
                fprintf(stderr,
                    "[debug] crossref: workspace scan stopped root=%s visited_entries=%zu error=%s\n",
                    path_to_utf8(workspace_path).c_str(),
                    visited_entries,
                    error.message().c_str());
            }
        }

        std::sort(uris.begin(), uris.end());
        uris.erase(std::unique(uris.begin(), uris.end()), uris.end());
        return uris;
    }

    void reset_unlocked() {
        index_ = bridge_similarity::SimilarityIndex{};
        line_infos_.clear();
        file_line_ids_.clear();
        open_document_versions_.clear();
        disk_file_timestamps_.clear();
        workspace_root_paths_.clear();
        indexed_file_paths_.clear();
        applied_generation_ = rebuild_generation_;
        next_line_id_ = 1;
    }

    static bool path_is_within_root(const std::filesystem::path& file_path, const std::filesystem::path& root_path) {
        const std::filesystem::path normalized_file = file_path.lexically_normal();
        const std::filesystem::path normalized_root = root_path.lexically_normal();
        if (normalized_root.empty()) {
            return false;
        }

        auto file_it = normalized_file.begin();
        auto root_it = normalized_root.begin();
        for (; root_it != normalized_root.end(); ++root_it, ++file_it) {
            if (file_it == normalized_file.end() || *file_it != *root_it) {
                return false;
            }
        }

        return true;
    }

    bool is_in_workspace_unlocked(const std::filesystem::path& file_path) const {
        return std::any_of(workspace_root_paths_.begin(), workspace_root_paths_.end(), [&](const std::filesystem::path& root_path) {
            return path_is_within_root(file_path, root_path);
        });
    }

    void erase_file_unlocked(const std::string& file_path) {
        const auto ids_it = file_line_ids_.find(file_path);
        if (ids_it != file_line_ids_.end()) {
            for (bridge_similarity::LineId id : ids_it->second) {
                index_.erase(id);
                line_infos_.erase(id);
            }
            file_line_ids_.erase(ids_it);
        }

        open_document_versions_.erase(file_path);
        disk_file_timestamps_.erase(file_path);
        indexed_file_paths_.erase(file_path);
    }

    void replace_document_unlocked(
        const std::string& file_path,
        const std::string& file_base_name,
        const std::vector<bridge_text::ParsedPair>& pairs) {
        erase_file_unlocked(file_path);

        indexed_file_paths_.insert(file_path);

        if (pairs.empty()) {
            return;
        }

        auto& ids = file_line_ids_[file_path];
        ids.reserve(pairs.size());
        for (const auto& pair : pairs) {
            const bridge_similarity::LineId line_id = next_line_id_++;
            ids.push_back(line_id);
            line_infos_.emplace(line_id, LineInfo{file_path, file_base_name, pair.original_line_index, pair.original.text, pair.translated.text});
            index_.upsert(line_id, file_path, pair.original_line_index, pair.original.text);
        }
    }

    static void ingest_document(
        BuildState& state,
        const bridge_documents::TextDocument& document,
        const bridge_text::ITextParser& parser,
        bool is_open_document,
        const std::optional<std::filesystem::file_time_type>& timestamp = std::nullopt) {
        const std::string file_path = path_to_utf8(document.getFilePath());
        const std::string file_base_name = file_name_to_utf8(document.getFilePath());

        try {
            const auto pairs = parser.parse_paired_lines(document.getContent());
            state.indexed_file_paths.insert(file_path);
            if (!pairs.empty()) {
                auto& ids = state.file_line_ids[file_path];
                ids.reserve(pairs.size());
                for (const auto& pair : pairs) {
                    const bridge_similarity::LineId line_id = state.next_line_id++;
                    ids.push_back(line_id);
                    state.line_infos.emplace(line_id, LineInfo{file_path, file_base_name, pair.original_line_index, pair.original.text, pair.translated.text});
                    state.index.upsert(line_id, file_path, pair.original_line_index, pair.original.text);
                }
            }

            if (is_open_document) {
                state.open_document_versions[file_path] = document.getVersion();
            } else if (timestamp.has_value()) {
                state.disk_file_timestamps[file_path] = *timestamp;
            }
        } catch (...) {
            // Skip files that do not match the parser instead of failing the whole rebuild.
        }
    }

    static BuildState build_state_from_snapshot(
        const std::vector<std::string>& workspace_folders,
        const std::vector<bridge_documents::TextDocument>& open_documents,
        const ParserState& parser_state) {
        BuildState state;
        const auto parser = bridge_text::make_text_parser(parser_state.config);

        fprintf(stderr, "[debug] crossref: build_state_from_snapshot start workspace_folders=%zu open_documents=%zu\n", workspace_folders.size(), open_documents.size());

        state.workspace_root_paths.reserve(workspace_folders.size());
        for (const auto& workspace_folder_uri : workspace_folders) {
            state.workspace_root_paths.push_back(bridge_documents::file_path_from_uri(workspace_folder_uri));
        }

        fprintf(stderr, "[debug] crossref: converted workspace root paths count=%zu\n", state.workspace_root_paths.size());

        std::unordered_map<std::string, bridge_documents::TextDocument> open_documents_by_uri;
        open_documents_by_uri.reserve(open_documents.size());
        for (const auto& document : open_documents) {
            if (!is_supported_path(document.getFilePath())) {
                continue;
            }
            open_documents_by_uri.emplace(document.getUri(), document);
        }

        fprintf(stderr, "[debug] crossref: indexed open documents count=%zu\n", open_documents_by_uri.size());

        const std::vector<std::string> workspace_uris = collect_workspace_text_uris(workspace_folders);
        fprintf(stderr, "[debug] crossref: collected %zu text files in workspace\n", workspace_uris.size());
        for (const auto& uri : workspace_uris) {
            const auto open_document_it = open_documents_by_uri.find(uri);
            if (open_document_it != open_documents_by_uri.end()) {
                ingest_document(state, open_document_it->second, *parser, true);
                continue;
            }

            try {
                const auto document = bridge_batch::load_document_fast_from_uri(uri);
                std::error_code error;
                const auto timestamp = std::filesystem::last_write_time(document.getFilePath(), error);
                ingest_document(state, document, *parser, false, error ? std::nullopt : std::optional(timestamp));
            } catch (...) {
                // Ignore individual file load failures during background rebuilds.
            }
        }

        return state;
    }

    void apply_rebuild_unlocked(BuildState&& state) {
        index_ = std::move(state.index);
        line_infos_ = std::move(state.line_infos);
        file_line_ids_ = std::move(state.file_line_ids);
        open_document_versions_ = std::move(state.open_document_versions);
        disk_file_timestamps_ = std::move(state.disk_file_timestamps);
        workspace_root_paths_ = std::move(state.workspace_root_paths);
        indexed_file_paths_ = std::move(state.indexed_file_paths);
        next_line_id_ = state.next_line_id;
    }

    void schedule_rebuild(
        ParserState parser_state,
        std::uint64_t generation,
        std::vector<std::string> workspace_folders,
        std::vector<bridge_documents::TextDocument> open_documents,
        boost::asio::thread_pool& pool) {
        boost::asio::post(pool, [this, parser_state = std::move(parser_state), generation, workspace_folders = std::move(workspace_folders), open_documents = std::move(open_documents)]() mutable {
            try {
                fprintf(stderr,
                    "[debug] crossref: rebuild start generation=%llu workspace_folders=%zu open_documents=%zu\n",
                    static_cast<unsigned long long>(generation),
                    workspace_folders.size(),
                    open_documents.size());
                BuildState state = build_state_from_snapshot(workspace_folders, open_documents, parser_state);
                std::function<void()> index_ready_notifier;

                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (!parser_state_.has_value() || parser_state_->stamp != parser_state.stamp || generation != rebuild_generation_) {
                        fprintf(stderr,
                            "[debug] crossref: rebuild stale generation=%llu current_generation=%llu parser_config_present=%d indexed_lines_built=%zu\n",
                            static_cast<unsigned long long>(generation),
                            static_cast<unsigned long long>(rebuild_generation_),
                            parser_state_.has_value() ? 1 : 0,
                            state.line_infos.size());
                        return;
                    }

                    apply_rebuild_unlocked(std::move(state));
                    applied_generation_ = generation;
                    index_ready_notifier = index_ready_notifier_;
                    fprintf(stderr,
                        "[debug] crossref: rebuild applied generation=%llu indexed_files=%zu indexed_lines=%zu notifier=%d\n",
                        static_cast<unsigned long long>(generation),
                        indexed_file_paths_.size(),
                        line_infos_.size(),
                        index_ready_notifier ? 1 : 0);
                }

                rebuild_cv_.notify_all();

                if (index_ready_notifier) {
                    fprintf(stderr,
                        "[debug] crossref: notifying index ready generation=%llu\n",
                        static_cast<unsigned long long>(generation));
                    index_ready_notifier();
                }
            } catch (const std::exception& ex) {
                fprintf(stderr,
                    "[debug] crossref: rebuild failed generation=%llu error=%s\n",
                    static_cast<unsigned long long>(generation),
                    ex.what());
                rebuild_cv_.notify_all();
            } catch (...) {
                fprintf(stderr,
                    "[debug] crossref: rebuild failed generation=%llu error=unknown\n",
                    static_cast<unsigned long long>(generation));
                rebuild_cv_.notify_all();
            }
        });
    }

    static json line_search_result_to_json(const LineSearchResult& result) {
        const std::string jp_utf8 = bridge_documents::utf16_to_utf8(result.line_info.jp_line);
        const std::string tr_utf8 = bridge_documents::utf16_to_utf8(result.line_info.tr_line);

        return json{
            {"lineInfo", json{
                {"fileName", result.line_info.file_name},
                {"lineNumber", result.line_info.line_number},
                {"jpLine", jp_utf8},
                {"trLine", tr_utf8},
            }},
            {"score", result.score},
        };
    }

    std::vector<LineSearchResult> search_line(
        boost::asio::thread_pool& pool,
        std::u16string_view query,
        int threshold,
        std::size_t limit,
        const std::string& current_file_path,
        const std::string& current_base_name,
        std::size_t current_line_number,
        std::size_t* exact_count) const {
        const auto raw_candidates = [&]() {
            std::lock_guard<std::mutex> lock(mutex_);
            return index_.search(pool, query, 0.0, (std::max)(limit * 3, std::size_t{16}));
        }();

        std::vector<LineSearchResult> filtered;
        filtered.reserve(raw_candidates.size());

        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const auto& candidate : raw_candidates) {
                const auto info_it = line_infos_.find(candidate.line_id);
                if (info_it == line_infos_.end()) {
                    continue;
                }

                const LineInfo& line_info = info_it->second;
                if (score_upper_bound_by_length(query, line_info.jp_line) < threshold) {
                    continue;
                }

                const int score = compute_score(query, line_info.jp_line);
                if (score < threshold) {
                    continue;
                }

                if (line_info.file_base_name == current_base_name
                    && !(line_info.file_name == current_file_path && line_info.line_number != current_line_number)) {
                    continue;
                }

                filtered.push_back(LineSearchResult{line_info, score});
            }
        }

        std::sort(filtered.begin(), filtered.end(), [](const LineSearchResult& left, const LineSearchResult& right) {
            if (left.score != right.score) {
                return left.score > right.score;
            }
            if (left.line_info.file_name != right.line_info.file_name) {
                return left.line_info.file_name < right.line_info.file_name;
            }
            return left.line_info.line_number < right.line_info.line_number;
        });

        if (filtered.size() > limit) {
            filtered.resize(limit);
        }

        *exact_count = 0;
        for (const auto& result : filtered) {
            if (result.score == 100) {
                ++*exact_count;
            }
        }

        return filtered;
    }

public:
    void set_index_ready_notifier(std::function<void()> notifier) {
        std::lock_guard<std::mutex> lock(mutex_);
        index_ready_notifier_ = std::move(notifier);
    }

    void update_parser_config(
        const ParserState& parser_state,
        const std::vector<std::string>& workspace_folders,
        const std::vector<bridge_documents::TextDocument>& open_documents,
        boost::asio::thread_pool& pool) {

        std::uint64_t generation = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (parser_state_.has_value() && parser_state_->stamp == parser_state.stamp) {
                fprintf(stderr,
                    "crossref: update_parser_config skipped unchanged payload workspace_folders=%zu open_documents=%zu\n",
                    workspace_folders.size(),
                    open_documents.size());
                return;
            }

            parser_state_ = parser_state;
            generation = ++rebuild_generation_;
        }

        fprintf(stderr,
            "crossref: update_parser_config generation=%llu workspace_folders=%zu open_documents=%zu\n",
            static_cast<unsigned long long>(generation),
            workspace_folders.size(),
            open_documents.size());

        schedule_rebuild(std::move(parser_state), generation, workspace_folders, open_documents, pool);
    }

    void clear_parser_config() {
        std::lock_guard<std::mutex> lock(mutex_);
        fprintf(stderr,
            "crossref: clear_parser_config current_generation=%llu indexed_files=%zu indexed_lines=%zu\n",
            static_cast<unsigned long long>(rebuild_generation_),
            indexed_file_paths_.size(),
            line_infos_.size());
        parser_state_.reset();
        ++rebuild_generation_;
        reset_unlocked();
    }

    void schedule_open_document_update(bridge_documents::TextDocument document, boost::asio::thread_pool& pool) {
        if (!is_supported_path(document.getFilePath())) {
            return;
        }

        ParserState parser_state;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!parser_state_.has_value() || !is_in_workspace_unlocked(document.getFilePath())) {
                return;
            }

            const auto version_it = open_document_versions_.find(path_to_utf8(document.getFilePath()));
            if (version_it != open_document_versions_.end() && version_it->second == document.getVersion()) {
                return;
            }

            parser_state = *parser_state_;
        }

        boost::asio::post(pool, [this, document = std::move(document), parser_state = std::move(parser_state)]() mutable {
            const auto parser = bridge_text::make_text_parser(parser_state.config);
            const std::string file_path = path_to_utf8(document.getFilePath());
            const std::string file_base_name = file_name_to_utf8(document.getFilePath());

            try {
                const auto pairs = parser->parse_paired_lines(document.getContent());
                std::lock_guard<std::mutex> lock(mutex_);
                if (!parser_state_.has_value() || parser_state_->stamp != parser_state.stamp || !is_in_workspace_unlocked(document.getFilePath())) {
                    return;
                }

                replace_document_unlocked(file_path, file_base_name, pairs);
                open_document_versions_[file_path] = document.getVersion();
                disk_file_timestamps_.erase(file_path);
            } catch (...) {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!parser_state_.has_value() || parser_state_->stamp != parser_state.stamp || !is_in_workspace_unlocked(document.getFilePath())) {
                    return;
                }

                erase_file_unlocked(file_path);
                open_document_versions_[file_path] = document.getVersion();
                indexed_file_paths_.insert(file_path);
            }
        });
    }

    void schedule_closed_document_refresh(const std::string& uri, boost::asio::thread_pool& pool) {
        if (!is_supported_uri(uri)) {
            return;
        }

        const std::filesystem::path file_path = bridge_documents::file_path_from_uri(uri);
        ParserState parser_state;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!parser_state_.has_value()) {
                return;
            }

            if (!is_in_workspace_unlocked(file_path)) {
                erase_file_unlocked(path_to_utf8(file_path));
                return;
            }

            parser_state = *parser_state_;
        }

        boost::asio::post(pool, [this, file_path, parser_state = std::move(parser_state)]() mutable {
            const std::string file_path_string = path_to_utf8(file_path);
            std::error_code error;
            if (!std::filesystem::exists(file_path, error) || error) {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!parser_state_.has_value() || parser_state_->stamp != parser_state.stamp) {
                    return;
                }
                erase_file_unlocked(file_path_string);
                return;
            }

            try {
                const auto document = bridge_batch::load_document_fast(file_path);
                const auto parser = bridge_text::make_text_parser(parser_state.config);
                const auto pairs = parser->parse_paired_lines(document.getContent());
                std::error_code timestamp_error;
                const auto timestamp = std::filesystem::last_write_time(file_path, timestamp_error);

                std::lock_guard<std::mutex> lock(mutex_);
                if (!parser_state_.has_value() || parser_state_->stamp != parser_state.stamp || !is_in_workspace_unlocked(file_path)) {
                    return;
                }

                replace_document_unlocked(file_path_string, file_name_to_utf8(file_path), pairs);
                open_document_versions_.erase(file_path_string);
                if (!timestamp_error) {
                    disk_file_timestamps_[file_path_string] = timestamp;
                }
            } catch (...) {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!parser_state_.has_value() || parser_state_->stamp != parser_state.stamp) {
                    return;
                }
                erase_file_unlocked(file_path_string);
            }
        });
    }

    json search_json(
        const bridge_documents::TextDocument& current_document,
        int threshold,
        std::size_t limit) const {
        if (limit == 0 || !is_supported_path(current_document.getFilePath())) {
            return json{{"matches", json::array()}};
        }

        std::optional<ParserState> parser_state;
        std::uint64_t target_generation = 0;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            parser_state = parser_state_;
            target_generation = rebuild_generation_;

            if (parser_state.has_value() && applied_generation_ < target_generation) {
                fprintf(stderr,
                    "crossref: search waiting for rebuild target_generation=%llu applied_generation=%llu\n",
                    static_cast<unsigned long long>(target_generation),
                    static_cast<unsigned long long>(applied_generation_));

                rebuild_cv_.wait_for(lock, std::chrono::seconds(10), [&]() {
                    return !parser_state_.has_value()
                        || rebuild_generation_ != target_generation
                        || applied_generation_ >= target_generation;
                });

                parser_state = parser_state_;
                target_generation = rebuild_generation_;

                fprintf(stderr,
                    "crossref: search wait finished target_generation=%llu applied_generation=%llu parser_config_present=%d\n",
                    static_cast<unsigned long long>(target_generation),
                    static_cast<unsigned long long>(applied_generation_),
                    parser_state.has_value() ? 1 : 0);
            }
        }

        if (!parser_state.has_value()) {
            return json{{"matches", json::array()}};
        }

        const auto parser = bridge_text::make_text_parser(parser_state->config);

        std::vector<bridge_text::ParsedPair> current_pairs;
        try {
            current_pairs = parser->parse_paired_lines(current_document.getContent());
        } catch (...) {
            return json{{"matches", json::array()}};
        }

        const std::string current_file_path = path_to_utf8(current_document.getFilePath());
        const std::string current_base_name = file_name_to_utf8(current_document.getFilePath());
        json matches = json::array();
        // calculate execution time for each line, and sum them up
        std::chrono::duration<double> total_duration(0);
        for (const auto& pair : current_pairs) {
            auto start_time = std::chrono::high_resolution_clock::now();
            std::size_t exact_count = 0;
            auto refs = search_line(search_thread_pool(), pair.original.text, threshold, limit, current_file_path, current_base_name, pair.original_line_index, &exact_count);
            auto end_time = std::chrono::high_resolution_clock::now();
            fprintf(stderr, "Search time for line %zu: %.3f seconds\n", pair.original_line_index, std::chrono::duration<double>(end_time - start_time).count());
            total_duration += end_time - start_time;
            if (refs.empty()) {
                continue;
            }

            if (exact_count >= limit) {
                continue;
            }

            json refs_json = json::array();
            for (const auto& ref : refs) {
                refs_json.push_back(line_search_result_to_json(ref));
            }

            matches.push_back(json{
                {"lineNumber", pair.original_line_index},
                {"exactCount", exact_count},
                {"refs", std::move(refs_json)},
            });
        }

        fprintf(stderr, "Total search time: %.3f seconds\n", total_duration.count());

        return json{{"matches", std::move(matches)}};
    }
};

}  // namespace bridge_crossref
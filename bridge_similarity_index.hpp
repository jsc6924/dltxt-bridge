#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace bridge_similarity {

using LineId = std::uint64_t;

struct SearchResult {
    LineId line_id = 0;
    std::string file_path;
    std::size_t line_number = 0;
    double score = 0.0;
};

class SimilarityIndex {
    using Term = std::u16string;
    using TermFrequencyMap = std::unordered_map<Term, std::uint32_t>;

    struct IndexedLine {
        LineId line_id = 0;
        std::uint32_t file_id = 0;
        std::size_t line_number = 0;
        TermFrequencyMap term_frequency;
    };

    std::unordered_map<LineId, IndexedLine> lines_;
    std::unordered_map<Term, std::size_t> document_frequency_;
    std::unordered_map<Term, std::unordered_set<LineId>> postings_;
    std::unordered_map<std::string, std::uint32_t> file_ids_by_path_;
    std::vector<std::string> file_paths_by_id_;
    std::vector<std::size_t> file_ref_counts_;

    static TermFrequencyMap build_term_frequency(std::u16string_view text) {
        TermFrequencyMap frequencies;

        for (std::size_t gram_size : {std::size_t{2}, std::size_t{3}}) {
            if (text.size() < gram_size) {
                continue;
            }

            for (std::size_t index = 0; index + gram_size <= text.size(); ++index) {
                ++frequencies[Term{text.substr(index, gram_size)}];
            }
        }

        return frequencies;
    }

    std::uint32_t retain_file_id(const std::string& file_path) {
        const auto it = file_ids_by_path_.find(file_path);
        if (it != file_ids_by_path_.end()) {
            ++file_ref_counts_[it->second];
            return it->second;
        }

        const auto next_id = static_cast<std::uint32_t>(file_paths_by_id_.size());
        file_ids_by_path_.emplace(file_path, next_id);
        file_paths_by_id_.push_back(file_path);
        file_ref_counts_.push_back(1);
        return next_id;
    }

    void release_file_id(std::uint32_t file_id) {
        if (file_id < file_ref_counts_.size() && file_ref_counts_[file_id] > 0) {
            --file_ref_counts_[file_id];
        }
    }

    static double inverse_document_frequency(std::size_t total_documents, std::size_t document_frequency) {
        return std::log((static_cast<double>(total_documents) + 1.0) / (static_cast<double>(document_frequency) + 1.0)) + 1.0;
    }

    double current_idf(const Term& term) const {
        const auto it = document_frequency_.find(term);
        const std::size_t df = it != document_frequency_.end() ? it->second : 0;
        return inverse_document_frequency(lines_.size(), df);
    }

    double line_norm(const IndexedLine& line) const {
        double norm_squared = 0.0;
        for (const auto& [term, count] : line.term_frequency) {
            const double weight = static_cast<double>(count) * current_idf(term);
            norm_squared += weight * weight;
        }
        return std::sqrt(norm_squared);
    }

public:
    void upsert(LineId line_id, std::string file_path, std::size_t line_number, std::u16string text) {
        erase(line_id);

        IndexedLine indexed_line;
        indexed_line.line_id = line_id;
        indexed_line.file_id = retain_file_id(file_path);
        indexed_line.line_number = line_number;
        indexed_line.term_frequency = build_term_frequency(text);

        for (const auto& [term, count] : indexed_line.term_frequency) {
            (void)count;
            ++document_frequency_[term];
            postings_[term].insert(line_id);
        }

        lines_.emplace(line_id, std::move(indexed_line));
    }

    bool erase(LineId line_id) {
        const auto it = lines_.find(line_id);
        if (it == lines_.end()) {
            return false;
        }

        for (const auto& [term, count] : it->second.term_frequency) {
            (void)count;
            auto posting_it = postings_.find(term);
            if (posting_it != postings_.end()) {
                posting_it->second.erase(line_id);
                if (posting_it->second.empty()) {
                    postings_.erase(posting_it);
                }
            }

            auto df_it = document_frequency_.find(term);
            if (df_it != document_frequency_.end()) {
                if (--df_it->second == 0) {
                    document_frequency_.erase(df_it);
                }
            }
        }

        release_file_id(it->second.file_id);
        lines_.erase(it);
        return true;
    }

    std::vector<SearchResult> search(
        std::u16string_view query,
        double min_score = 0.0,
        std::size_t max_results = 10) const {
        if (lines_.empty() || max_results == 0) {
            return {};
        }

        const TermFrequencyMap query_term_frequency = build_term_frequency(query);
        if (query_term_frequency.empty()) {
            return {};
        }

        std::unordered_map<Term, double> query_weights;
        query_weights.reserve(query_term_frequency.size());
        std::unordered_set<LineId> candidate_ids;
        double query_norm_squared = 0.0;

        for (const auto& [term, count] : query_term_frequency) {
            const double weight = static_cast<double>(count) * current_idf(term);
            query_weights.emplace(term, weight);
            query_norm_squared += weight * weight;

            const auto posting_it = postings_.find(term);
            if (posting_it != postings_.end()) {
                candidate_ids.insert(posting_it->second.begin(), posting_it->second.end());
            }
        }

        if (candidate_ids.empty() || query_norm_squared <= 0.0) {
            return {};
        }

        const double query_norm = std::sqrt(query_norm_squared);
        std::vector<SearchResult> results;
        results.reserve(candidate_ids.size());

        for (LineId candidate_id : candidate_ids) {
            const auto line_it = lines_.find(candidate_id);
            if (line_it == lines_.end()) {
                continue;
            }

            const IndexedLine& line = line_it->second;
            const double document_norm = line_norm(line);
            if (document_norm <= 0.0) {
                continue;
            }

            double dot_product = 0.0;
            for (const auto& [term, query_weight] : query_weights) {
                const auto term_it = line.term_frequency.find(term);
                if (term_it == line.term_frequency.end()) {
                    continue;
                }

                dot_product += query_weight * (static_cast<double>(term_it->second) * current_idf(term));
            }

            if (dot_product <= 0.0) {
                continue;
            }

            const double score = dot_product / (query_norm * document_norm);
            if (score < min_score) {
                continue;
            }

            results.push_back(SearchResult{
                line.line_id,
                file_paths_by_id_[line.file_id],
                line.line_number,
                score,
            });
        }

        std::sort(results.begin(), results.end(), [](const SearchResult& left, const SearchResult& right) {
            if (left.score != right.score) {
                return left.score > right.score;
            }
            if (left.file_path != right.file_path) {
                return left.file_path < right.file_path;
            }
            if (left.line_number != right.line_number) {
                return left.line_number < right.line_number;
            }
            return left.line_id < right.line_id;
        });

        if (results.size() > max_results) {
            results.resize(max_results);
        }

        return results;
    }

    std::size_t document_count() const {
        return lines_.size();
    }

    std::size_t file_count() const {
        return std::count_if(file_ref_counts_.begin(), file_ref_counts_.end(), [](std::size_t ref_count) {
            return ref_count > 0;
        });
    }
};

}  // namespace bridge_similarity
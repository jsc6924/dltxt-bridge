#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <limits>
#include <optional>
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
    using TermId = std::uint32_t;
    using TermFrequencyMap = std::vector<std::pair<TermId, std::uint32_t>>; // Sorted vector of (term_id, frequency) pairs
    static constexpr std::size_t kMaxCandidateBudget = 1024;

    struct IndexedLine {
        LineId line_id = 0;
        std::uint32_t file_id = 0;
        std::size_t line_number = 0;
        TermFrequencyMap term_frequency;
    };

    std::unordered_map<LineId, IndexedLine> lines_;
    std::unordered_map<TermId, std::size_t> document_frequency_;
    std::unordered_map<TermId, std::vector<LineId>> postings_; // Inverted index: term_id -> sorted list of line_ids containing the term.
    std::unordered_map<Term, TermId> term_ids_by_value_;
    std::vector<Term> term_values_by_id_;
    std::vector<std::size_t> term_ref_counts_;
    std::unordered_map<std::string, std::uint32_t> file_ids_by_path_;
    std::vector<std::string> file_paths_by_id_;
    std::vector<std::size_t> file_ref_counts_;

    template <typename ResolveTermId>
    static TermFrequencyMap build_term_frequency(std::u16string_view text, ResolveTermId&& resolve_term_id) {
        TermFrequencyMap frequencies;
        std::vector<std::uint32_t> seen_term_ids;

        for (std::size_t gram_size : {std::size_t{2}}) {
            if (text.size() < gram_size) {
                continue;
            }

            for (std::size_t index = 0; index + gram_size <= text.size(); ++index) {
                const auto maybe_term_id = resolve_term_id(text.substr(index, gram_size));
                if (!maybe_term_id.has_value()) {
                    continue;
                }

                seen_term_ids.push_back(*maybe_term_id);
            }
        }
        std::sort(seen_term_ids.begin(), seen_term_ids.end());
        for (TermId term_id : seen_term_ids) {
            if (!frequencies.empty() && frequencies.back().first == term_id) {
                ++frequencies.back().second;
            } else {
                frequencies.emplace_back(term_id, 1);
            }
        }

        return frequencies;
    }

    std::optional<TermId> find_term_id(std::u16string_view term) const {
        const auto it = term_ids_by_value_.find(Term{term});
        if (it == term_ids_by_value_.end()) {
            return std::nullopt;
        }

        return it->second;
    }

    TermId retain_term_id(std::u16string_view term) {
        const auto it = term_ids_by_value_.find(Term{term});
        if (it != term_ids_by_value_.end()) {
            ++term_ref_counts_[it->second];
            return it->second;
        }

        const auto next_id = static_cast<TermId>(term_values_by_id_.size());
        Term owned_term{term};
        term_ids_by_value_.emplace(owned_term, next_id);
        term_values_by_id_.push_back(std::move(owned_term));
        term_ref_counts_.push_back(1);
        return next_id;
    }

    void release_term_id(TermId term_id) {
        if (term_id >= term_ref_counts_.size() || term_ref_counts_[term_id] == 0) {
            return;
        }

        if (--term_ref_counts_[term_id] == 0) {
            term_ids_by_value_.erase(term_values_by_id_[term_id]);
            term_values_by_id_[term_id].clear();
            term_values_by_id_[term_id].shrink_to_fit();
        }
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

    double current_idf(TermId term_id) const {
        const auto it = document_frequency_.find(term_id);
        const std::size_t df = it != document_frequency_.end() ? it->second : 0;
        return inverse_document_frequency(lines_.size(), df);
    }

    double line_norm(const IndexedLine& line) const {
        double norm_squared = 0.0;
        for (const auto& [term_id, count] : line.term_frequency) {
            const double weight = static_cast<double>(count) * current_idf(term_id);
            norm_squared += weight * weight;
        }
        return std::sqrt(norm_squared);
    }

    static void insert_candidates_up_to_budget(
        std::unordered_set<LineId>& candidate_ids,
        const std::vector<LineId>& posting,
        std::size_t candidate_budget) {
        for (LineId candidate_id : posting) {
            candidate_ids.insert(candidate_id);
            if (candidate_ids.size() >= candidate_budget) {
                break;
            }
        }
    }

public:
    void upsert(LineId line_id, std::string file_path, std::size_t line_number, std::u16string text) {
        erase(line_id);

        IndexedLine indexed_line;
        indexed_line.line_id = line_id;
        indexed_line.file_id = retain_file_id(file_path);
        indexed_line.line_number = line_number;
        indexed_line.term_frequency = build_term_frequency(text, [this](std::u16string_view term) {
            return std::optional<TermId>{retain_term_id(term)};
        });

        for (const auto& [term_id, count] : indexed_line.term_frequency) {
            (void)count;
            ++document_frequency_[term_id];
            auto& posting = postings_[term_id];
            if (posting.empty() || posting.back() < line_id) {
                posting.push_back(line_id);
                continue;
            }
            const auto insert_it = std::lower_bound(posting.begin(), posting.end(), line_id);
            if (insert_it == posting.end() || *insert_it != line_id) {
                posting.insert(insert_it, line_id);
            }
        }

        lines_.emplace(line_id, std::move(indexed_line));
    }

    bool erase(LineId line_id) {
        const auto it = lines_.find(line_id);
        if (it == lines_.end()) {
            return false;
        }

        for (const auto& [term_id, count] : it->second.term_frequency) {
            (void)count;
            auto posting_it = postings_.find(term_id);
            if (posting_it != postings_.end()) {
                auto& posting = posting_it->second;
                const auto erase_it = std::lower_bound(posting.begin(), posting.end(), line_id);
                if (erase_it != posting.end() && *erase_it == line_id) {
                    posting.erase(erase_it);
                }
                if (posting_it->second.empty()) {
                    postings_.erase(posting_it);
                }
            }

            auto df_it = document_frequency_.find(term_id);
            if (df_it != document_frequency_.end()) {
                if (--df_it->second == 0) {
                    document_frequency_.erase(df_it);
                }
            }

            release_term_id(term_id);
        }

        release_file_id(it->second.file_id);
        lines_.erase(it);
        return true;
    }

    std::vector<SearchResult> search(
        std::u16string_view query,
        double min_score = 0.0,
        std::size_t max_results = 10) const {
        using Clock = std::chrono::steady_clock;
        const auto search_started_at = Clock::now();
        if (lines_.empty() || max_results == 0) {
            return {};
        }

        const TermFrequencyMap query_term_frequency = build_term_frequency(query, [this](std::u16string_view term) {
            return find_term_id(term);
        });
        const auto term_frequency_built_at = Clock::now();
        if (query_term_frequency.empty()) {
            return {};
        }

        struct WeightedQueryTerm {
            TermId term_id = 0;
            double query_weight = 0.0;
            std::size_t posting_size = 0;
        };

        std::unordered_map<TermId, double> query_weights;
        query_weights.reserve(query_term_frequency.size());
        std::vector<WeightedQueryTerm> weighted_terms;
        weighted_terms.reserve(query_term_frequency.size());
        double query_norm_squared = 0.0;

        for (const auto& [term_id, count] : query_term_frequency) {
            const double weight = static_cast<double>(count) * current_idf(term_id);
            query_weights.emplace(term_id, weight);
            query_norm_squared += weight * weight;

            const auto posting_it = postings_.find(term_id);
            if (posting_it != postings_.end()) {
                weighted_terms.push_back(WeightedQueryTerm{
                    term_id,
                    weight,
                    posting_it->second.size(),
                });
            }
        }

        if (weighted_terms.empty() || query_norm_squared <= 0.0) {
            return {};
        }

        // Sort terms by increasing posting size and decreasing query weight to optimize candidate retrieval.
        // The intuition is that terms with smaller postings are more selective, and among terms with similar posting sizes, those with higher query weights contribute more to the score.
        std::sort(weighted_terms.begin(), weighted_terms.end(), [](const WeightedQueryTerm& left, const WeightedQueryTerm& right) {
            if (left.posting_size != right.posting_size) {
                return left.posting_size < right.posting_size;
            }
            return left.query_weight > right.query_weight;
        });

        // To avoid scoring too many candidates, we set a budget for how many candidate lines to consider based on the number of results requested. We then iterate through the query terms in order of increasing posting size, adding candidates until we reach the budget. We also skip terms with very large postings once we have some candidates, as they are less likely to contribute to a high score.
        const std::size_t candidate_budget = (std::min)((std::max)(max_results * 24, std::size_t{128}), kMaxCandidateBudget);
        const std::size_t common_posting_cutoff = (std::max)(candidate_budget * 4, std::size_t{512});
        std::unordered_set<LineId> candidate_ids;
        candidate_ids.reserve(candidate_budget);

        for (const auto& weighted_term : weighted_terms) {
            if (weighted_term.posting_size > common_posting_cutoff && !candidate_ids.empty()) {
                continue;
            }

            const auto posting_it = postings_.find(weighted_term.term_id);
            if (posting_it == postings_.end()) {
                continue;
            }

            insert_candidates_up_to_budget(candidate_ids, posting_it->second, candidate_budget);
            if (candidate_ids.size() >= candidate_budget) {
                break;
            }
        }

        if (candidate_ids.empty()) {
            const auto posting_it = postings_.find(weighted_terms.front().term_id);
            if (posting_it == postings_.end()) {
                return {};
            }

            insert_candidates_up_to_budget(candidate_ids, posting_it->second, candidate_budget);
        }

        if (candidate_ids.empty()) {
            return {};
        }

        const auto candidates_built_at = Clock::now();

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
            auto query_it = query_term_frequency.begin();
            auto line_term_it = line.term_frequency.begin();
            while (query_it != query_term_frequency.end() && line_term_it != line.term_frequency.end()) {
                if (query_it->first < line_term_it->first) {
                    ++query_it;
                    continue;
                }

                if (line_term_it->first < query_it->first) {
                    ++line_term_it;
                    continue;
                }

                const double query_weight = query_weights.at(query_it->first);
                dot_product += query_weight * (static_cast<double>(line_term_it->second) * current_idf(line_term_it->first));
                ++query_it;
                ++line_term_it;
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

        const auto scoring_finished_at = Clock::now();

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

        const auto search_finished_at = Clock::now();
        const auto to_milliseconds = [](Clock::duration duration) {
            return std::chrono::duration<double, std::milli>(duration).count();
        };
        fprintf(
            stderr,
            "similarity_index: search query_len=%zu query_terms=%zu weighted_terms=%zu candidates=%zu results=%zu tf_ms=%.3f candidate_ms=%.3f score_ms=%.3f sort_ms=%.3f total_ms=%.3f\n",
            query.size(),
            query_term_frequency.size(),
            weighted_terms.size(),
            candidate_ids.size(),
            results.size(),
            to_milliseconds(term_frequency_built_at - search_started_at),
            to_milliseconds(candidates_built_at - term_frequency_built_at),
            to_milliseconds(scoring_finished_at - candidates_built_at),
            to_milliseconds(search_finished_at - scoring_finished_at),
            to_milliseconds(search_finished_at - search_started_at));

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
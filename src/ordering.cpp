#include "cisda/ordering.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace cisda {
namespace {

struct VectorHash {
  std::size_t operator()(const std::vector<std::uint32_t> &values) const {
    std::size_t hash = 1469598103934665603ULL;
    for (const std::uint32_t value : values) {
      hash ^= value;
      hash *= 1099511628211ULL;
    }
    return hash;
  }
};

using SetCounts =
    std::unordered_map<std::vector<std::uint32_t>, std::uint64_t, VectorHash>;

std::uint64_t completed_gaussians(const std::vector<std::uint32_t> &candidate,
                                  const SetCounts &sets) {
  if (candidate.size() <= 12) {
    std::uint64_t count = 0;
    const std::uint64_t combinations = 1ULL << candidate.size();
    std::vector<std::uint32_t> subset;
    subset.reserve(candidate.size());
    for (std::uint64_t mask = 1; mask < combinations; ++mask) {
      subset.clear();
      for (std::size_t index = 0; index < candidate.size(); ++index) {
        if ((mask & (1ULL << index)) != 0) {
          subset.push_back(candidate.at(index));
        }
      }
      const auto found = sets.find(subset);
      if (found != sets.end()) {
        count += found->second;
      }
    }
    return count;
  }

  std::uint64_t count = 0;
  for (const auto &[sources, frequency] : sets) {
    if (sources.size() <= candidate.size() &&
        std::includes(candidate.begin(), candidate.end(), sources.begin(),
                      sources.end())) {
      count += frequency;
    }
  }
  return count;
}

} // namespace

OrderingResult gaussian_aware_order(const CaptureFacts &capture,
                                    std::uint64_t bytes_per_gaussian) {
  const auto &mapping = capture.mapping;
  const std::size_t tile_count = mapping.gaussians_by_tile.size();
  std::vector<bool> unissued(tile_count, true);
  OrderingResult result;
  result.tiles.reserve(tile_count);

  while (result.tiles.size() < tile_count) {
    SetCounts sets;
    sets.reserve(mapping.source_tiles_by_gaussian.size());
    std::vector<std::uint32_t> remaining;
    for (const auto &sources : mapping.source_tiles_by_gaussian) {
      remaining.clear();
      for (const std::uint32_t tile : sources) {
        if (unissued.at(tile)) {
          remaining.push_back(tile);
        }
      }
      if (!remaining.empty()) {
        ++sets[remaining];
      }
    }

    std::vector<std::uint32_t> selected;
    std::uint64_t selected_completions = 0;
    std::uint64_t selected_cost = 1;
    for (const auto &[candidate, frequency] : sets) {
      static_cast<void>(frequency);
      std::uint64_t cost = 0;
      for (const std::uint32_t tile : candidate) {
        cost += std::max<std::uint64_t>(1, mapping.relation_counts.at(tile));
      }
      const std::uint64_t completions = completed_gaussians(candidate, sets);
      const bool better_score =
          selected.empty() ||
          completions * selected_cost > selected_completions * cost;
      const bool equal_score =
          !selected.empty() &&
          completions * selected_cost == selected_completions * cost;
      if (better_score ||
          (equal_score &&
           (candidate.front() < selected.front() ||
            (candidate.front() == selected.front() && candidate < selected)))) {
        selected = candidate;
        selected_completions = completions;
        selected_cost = cost;
      }
    }

    if (selected.empty()) {
      const auto oldest = std::find(unissued.begin(), unissued.end(), true);
      if (oldest == unissued.end()) {
        throw std::runtime_error("tile ordering lost an unissued tile");
      }
      selected.push_back(static_cast<std::uint32_t>(oldest - unissued.begin()));
      ++result.fallback_selections;
    } else {
      result.gaussian_ready_groups += selected_completions;
    }

    std::sort(selected.begin(), selected.end());
    for (const std::uint32_t tile : selected) {
      if (!unissued.at(tile)) {
        throw std::runtime_error("tile ordering issued a tile twice");
      }
      std::unordered_set<std::uint32_t> unique(
          mapping.gaussians_by_tile.at(tile).begin(),
          mapping.gaussians_by_tile.at(tile).end());
      result.tiles.push_back(
          {tile, std::max<std::uint64_t>(1, mapping.relation_counts.at(tile)),
           mapping.relation_counts.at(tile), unique.size() * bytes_per_gaussian,
           static_cast<std::uint32_t>(result.tiles.size())});
      unissued.at(tile) = false;
    }
  }
  return result;
}

} // namespace cisda

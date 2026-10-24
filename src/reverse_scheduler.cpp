#include "cisda/reverse_scheduler.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <set>
#include <unordered_set>
#include <vector>

namespace cisda {

ReverseSchedule
reverse_dataflow_schedule(const std::vector<ReverseCandidate> &candidates,
                          std::uint64_t storage_credit_bytes) {
  std::vector<ReverseCandidate> eligible;
  std::vector<ReverseCandidate> fallback;
  ReverseSchedule result;
  std::unordered_set<std::uint64_t> shared_dependencies;
  for (const ReverseCandidate &candidate : candidates) {
    ReverseCandidate normalized = candidate;
    if (normalized.dependency_key != 0 &&
        !shared_dependencies.insert(normalized.dependency_key).second) {
      ++result.shared_task_merges;
      normalized.avoidable_bytes = 0;
    }
    if (normalized.longest_path <= normalized.remaining_window) {
      eligible.push_back(normalized);
      ++result.deadline_eligible;
    } else {
      fallback.push_back(normalized);
      ++result.rejected_by_window;
    }
  }
  const auto by_priority = [](const ReverseCandidate &left,
                              const ReverseCandidate &right) {
    const std::uint64_t left_path =
        std::max<std::uint64_t>(1, left.longest_path);
    const std::uint64_t right_path =
        std::max<std::uint64_t>(1, right.longest_path);
    const auto left_score = left.avoidable_bytes * right_path;
    const auto right_score = right.avoidable_bytes * left_path;
    if (left_score != right_score) {
      return left_score > right_score;
    }
    if (left.remaining_window != right.remaining_window) {
      return left.remaining_window < right.remaining_window;
    }
    if (left.waited_cycles != right.waited_cycles) {
      return left.waited_cycles > right.waited_cycles;
    }
    return left.tile_id < right.tile_id;
  };
  std::stable_sort(eligible.begin(), eligible.end(), by_priority);
  result.priority_order.reserve(eligible.size());
  result.fallback_order.reserve(fallback.size());
  result.reserved_tile_ids.reserve(eligible.size());
  std::set<std::pair<std::uint64_t, std::uint64_t>> priority_rows;
  std::unordered_set<std::uint64_t> reserved_dependencies;
  std::unordered_set<std::uint32_t> reserved_tiles;
  for (const ReverseCandidate &candidate : eligible) {
    const bool needs_attribute =
        candidate.dependency_key == 0 ||
        !reserved_dependencies.contains(candidate.dependency_key);
    const bool needs_tile = !reserved_tiles.contains(candidate.tile_id);
    const std::uint64_t attribute_bytes =
        needs_attribute ? candidate.attribute_bytes : 0;
    const std::uint64_t tile_bytes =
        needs_tile ? candidate.tile_contribution_bytes : 0;
    const bool request_overflows =
        attribute_bytes >
        std::numeric_limits<std::uint64_t>::max() - tile_bytes;
    const std::uint64_t requested_bytes =
        request_overflows ? std::numeric_limits<std::uint64_t>::max()
                          : attribute_bytes + tile_bytes;
    if (requested_bytes > storage_credit_bytes) {
      fallback.push_back(candidate);
      ++result.blocked_by_credits;
      continue;
    }
    storage_credit_bytes -= requested_bytes;
    if (candidate.dependency_key != 0) {
      reserved_dependencies.insert(candidate.dependency_key);
    }
    if (reserved_tiles.insert(candidate.tile_id).second) {
      result.reserved_tile_ids.push_back(candidate.tile_id);
    }
    result.reserved_attribute_bytes += attribute_bytes;
    result.reserved_tile_contribution_bytes += tile_bytes;
    result.deduplicated_bytes += candidate.avoidable_bytes;
    result.priority_order.push_back(candidate.tile_id);
    ++result.storage_reservations;
    ++result.asic_priority_propagations;
    ++result.pim_priority_propagations;
    const std::uint64_t path =
        std::max<std::uint64_t>(1, candidate.longest_path);
    const std::uint64_t priority = candidate.avoidable_bytes / path;
    priority_rows.emplace(priority, candidate.row_address);
  }
  result.near_memory_row_groups = priority_rows.size();
  std::stable_sort(
      fallback.begin(), fallback.end(),
      [](const ReverseCandidate &left, const ReverseCandidate &right) {
        return left.tile_id < right.tile_id;
      });
  for (const ReverseCandidate &candidate : fallback) {
    result.fallback_order.push_back(candidate.tile_id);
  }
  return result;
}

} // namespace cisda

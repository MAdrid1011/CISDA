#include "cisda/reverse_scheduler.hpp"

#include <algorithm>
#include <cstdint>
#include <set>
#include <unordered_set>
#include <vector>

namespace cisda {

ReverseSchedule
reverse_dataflow_schedule(const std::vector<ReverseCandidate> &candidates) {
  std::vector<ReverseCandidate> eligible;
  std::vector<ReverseCandidate> fallback;
  ReverseSchedule result;
  std::unordered_set<std::uint64_t> shared_dependencies;
  for (const ReverseCandidate &candidate : candidates) {
    ReverseCandidate normalized = candidate;
    if (!normalized.storage_credit) {
      fallback.push_back(normalized);
      ++result.blocked_by_credits;
      continue;
    }
    if (normalized.dependency_key != 0 &&
        !shared_dependencies.insert(normalized.dependency_key).second) {
      ++result.shared_task_merges;
      normalized.avoidable_bytes = 0;
    }
    if (normalized.longest_path <= normalized.remaining_window) {
      eligible.push_back(normalized);
      ++result.deadline_eligible;
      result.deduplicated_bytes += normalized.avoidable_bytes;
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
  std::stable_sort(
      fallback.begin(), fallback.end(),
      [](const ReverseCandidate &left, const ReverseCandidate &right) {
        return left.tile_id < right.tile_id;
      });
  result.priority_order.reserve(eligible.size());
  result.fallback_order.reserve(fallback.size());
  std::set<std::pair<std::uint64_t, std::uint64_t>> priority_rows;
  for (const ReverseCandidate &candidate : eligible) {
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
  for (const ReverseCandidate &candidate : fallback) {
    result.fallback_order.push_back(candidate.tile_id);
  }
  return result;
}

} // namespace cisda

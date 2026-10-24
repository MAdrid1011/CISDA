#pragma once

#include <cstdint>
#include <vector>

namespace cisda {

struct ReverseCandidate {
  std::uint32_t tile_id{};
  std::uint64_t remaining_window{};
  std::uint64_t longest_path{};
  std::uint64_t avoidable_bytes{};
  std::uint64_t waited_cycles{};
  std::uint64_t dependency_key{};
  std::uint64_t row_address{};
  bool storage_credit{true};
};

struct ReverseSchedule {
  std::vector<std::uint32_t> order;
  std::uint64_t deadline_eligible{};
  std::uint64_t rejected_by_window{};
  std::uint64_t deduplicated_bytes{};
  std::uint64_t blocked_by_credits{};
  std::uint64_t storage_reservations{};
  std::uint64_t shared_task_merges{};
  std::uint64_t asic_priority_propagations{};
  std::uint64_t pim_priority_propagations{};
  std::uint64_t near_memory_row_groups{};
};

ReverseSchedule
reverse_dataflow_schedule(const std::vector<ReverseCandidate> &candidates);

} // namespace cisda

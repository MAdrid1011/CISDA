#pragma once

#include "cisda/workload.hpp"

#include <cstdint>
#include <vector>

namespace cisda {

struct OrderedTile {
  std::uint32_t tile_id{};
  std::uint64_t cost_cycles{};
  std::uint64_t gaussian_relations{};
  std::uint64_t avoidable_bytes{};
  std::uint32_t arrival{};
};

struct OrderingResult {
  std::vector<OrderedTile> tiles;
  std::uint64_t gaussian_ready_groups{};
  std::uint64_t fallback_selections{};
};

OrderingResult gaussian_aware_order(const CaptureFacts &capture,
                                    std::uint64_t bytes_per_gaussian);

} // namespace cisda

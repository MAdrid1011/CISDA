#pragma once

#include "cisda/workload.hpp"

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace cisda {

struct ReadyGaussian {
  std::uint32_t gaussian{};
  std::uint64_t unblocked_consumers{};
};

class DependencyTables {
public:
  explicit DependencyTables(const MappingTopology &mapping);

  void set_unblocked_consumers(std::uint32_t gaussian, std::uint64_t consumers);
  std::vector<ReadyGaussian> complete_mapping_tile(std::uint32_t tile);

  void register_tracking_request(std::uint64_t request_key,
                                 const std::vector<std::uint32_t> &tiles,
                                 bool speculative);
  void resolve_tracking_request(std::uint64_t request_key);
  bool tile_validation_ready(std::uint32_t tile) const;

  bool reserve_spm(std::uint64_t bytes);
  void release_spm(std::uint64_t bytes);
  std::uint64_t spm_available_bytes() const;
  void account_credit_stalls(std::uint64_t stalls);
  bool reserve_frame_tile(std::uint64_t bytes);
  void release_frame_tile(std::uint64_t bytes);

  std::uint64_t input_entries() const;
  std::uint64_t validation_entries() const;
  std::uint64_t gradient_fragments() const;
  std::uint64_t gradient_reductions() const;
  std::uint64_t ready_gaussians() const;
  std::uint64_t single_source_xor_hits() const;
  std::uint64_t merged_requests() const;
  std::uint64_t input_waits() const;
  std::uint64_t priority_propagations() const;
  std::uint64_t spm_high_water_bytes() const;
  std::uint64_t frame_tile_high_water_bytes() const;
  std::uint64_t credit_stalls() const;

  static constexpr std::uint64_t spm_capacity_bytes() { return 128 * 1024; }
  static constexpr std::uint64_t frame_tile_capacity_bytes() {
    return 16 * 1024;
  }

private:
  struct InputEntry {
    std::uint64_t remaining{};
    std::uint64_t unfinished_xor{};
    std::uint64_t consumers{};
    bool completed{};
  };

  std::vector<InputEntry> inputs_;
  std::vector<std::vector<std::uint32_t>> gaussians_by_tile_;
  std::unordered_map<std::uint64_t, std::unordered_set<std::uint32_t>>
      request_tiles_;
  std::unordered_map<std::uint32_t, std::uint64_t> pending_by_tile_;
  std::uint64_t gradient_fragments_{};
  std::uint64_t gradient_reductions_{};
  std::uint64_t ready_gaussians_{};
  std::uint64_t single_source_xor_hits_{};
  std::uint64_t merged_requests_{};
  std::uint64_t input_waits_{};
  std::uint64_t priority_propagations_{};
  std::uint64_t spm_used_{};
  std::uint64_t spm_high_water_{};
  std::uint64_t frame_tile_used_{};
  std::uint64_t frame_tile_high_water_{};
  std::uint64_t credit_stalls_{};
};

} // namespace cisda

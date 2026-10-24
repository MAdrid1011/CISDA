#pragma once

#include <array>
#include <cstdint>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace cisda {

enum class ArrayMode { Gaussian, Ray, Tile };

struct ArrayIssue {
  std::uint64_t finish_cycle{};
  std::uint32_t cluster{};
};

struct SupplyResult {
  std::uint64_t ordered_entries{};
  std::uint64_t shared_rows{};
  std::uint64_t hits{};
  std::uint64_t fills{};
  std::uint64_t version_misses{};
  std::uint64_t fill_bytes{};
};

class SharedGsArray {
public:
  ArrayIssue issue(ArrayMode mode, std::uint64_t release_cycle,
                   std::uint64_t cycles, std::uint64_t residency_key);
  SupplyResult supply(std::uint64_t packet_key,
                      const std::vector<std::uint32_t> &depth_ordered_ids,
                      std::uint64_t map_version, std::uint64_t pose_context,
                      bool projection, std::uint64_t row_bytes);

  std::uint64_t tasks() const;
  std::uint64_t mode_switches() const;
  std::uint64_t makespan() const;
  std::uint64_t tasks(ArrayMode mode) const;
  std::uint64_t packet_reconfigurations() const;
  std::uint64_t residency_hits() const;
  std::uint64_t residency_fills() const;
  std::uint64_t residency_version_misses() const;
  std::uint64_t fill_bytes() const;
  std::uint64_t depth_ordered_entries() const;
  std::uint64_t shared_projection_rows() const;
  std::uint64_t gaussian_broadcast_packets() const;
  std::uint64_t transposed_exchange_packets() const;
  std::uint64_t ray_feedback_packets() const;
  std::uint64_t hierarchical_reduction_packets() const;

  static constexpr std::uint64_t cluster_count() { return 24; }
  static constexpr std::uint64_t fma_lanes_per_cluster() { return 16; }
  static constexpr std::uint64_t exp_log_lanes_per_cluster() { return 4; }
  static constexpr std::uint64_t reciprocal_sqrt_lanes_per_cluster() {
    return 2;
  }
  static constexpr std::uint64_t clusters_per_residency_group() { return 6; }
  static constexpr std::uint64_t residency_entries_per_group() { return 256; }
  static constexpr std::uint64_t data_sram_bytes_per_group() {
    return 32 * 1024;
  }

private:
  struct Cluster {
    std::uint64_t available{};
    ArrayMode mode{ArrayMode::Tile};
    bool initialized{};
  };

  struct ResidentKey {
    std::uint32_t gaussian{};
    std::uint64_t map_version{};
    std::uint64_t pose_context{};
    bool projection{};

    bool operator==(const ResidentKey &) const = default;
  };

  struct ResidentHash {
    std::size_t operator()(const ResidentKey &key) const;
  };

  struct ResidentRow {
    std::uint64_t bytes{};
    std::uint64_t last_use{};
    std::uint64_t retain_until{};
  };

  struct ResidencyGroup {
    std::unordered_map<ResidentKey, ResidentRow, ResidentHash> rows;
    std::uint64_t bytes{};
  };

  std::array<Cluster, 24> clusters_{};
  std::array<ResidencyGroup, 4> residency_{};
  std::array<std::uint64_t, 3> mode_tasks_{};
  std::uint64_t tasks_{};
  std::uint64_t mode_switches_{};
  std::uint64_t packet_sequence_{};
  std::uint64_t residency_hits_{};
  std::uint64_t residency_fills_{};
  std::uint64_t residency_version_misses_{};
  std::uint64_t fill_bytes_{};
  std::uint64_t depth_ordered_entries_{};
  std::uint64_t shared_projection_rows_{};
  std::uint64_t gaussian_broadcast_packets_{};
  std::uint64_t transposed_exchange_packets_{};
  std::uint64_t ray_feedback_packets_{};
  std::uint64_t hierarchical_reduction_packets_{};
};

std::string_view array_mode_name(ArrayMode mode);

} // namespace cisda

#include "cisda/shared_array.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <unordered_set>

namespace cisda {

std::size_t
SharedGsArray::ResidentHash::operator()(const ResidentKey &key) const {
  std::size_t value = std::hash<std::uint32_t>{}(key.gaussian);
  value ^= std::hash<std::uint64_t>{}(key.map_version) + 0x9e3779b9U +
           (value << 6U) + (value >> 2U);
  value ^= std::hash<std::uint64_t>{}(key.pose_context) + 0x9e3779b9U +
           (value << 6U) + (value >> 2U);
  value ^= std::hash<bool>{}(key.projection) + 0x9e3779b9U + (value << 6U) +
           (value >> 2U);
  return value;
}

ArrayIssue SharedGsArray::issue(ArrayMode mode, std::uint64_t release_cycle,
                                std::uint64_t cycles,
                                std::uint64_t residency_key) {
  const std::uint32_t group = static_cast<std::uint32_t>(residency_key % 4);
  const std::uint32_t first = group * 6;
  std::uint32_t selected = first;
  std::uint64_t best = std::numeric_limits<std::uint64_t>::max();
  for (std::uint32_t cluster = first; cluster < first + 6; ++cluster) {
    const std::uint64_t available = clusters_.at(cluster).available;
    if (available < best) {
      best = available;
      selected = cluster;
    }
  }
  Cluster &cluster = clusters_.at(selected);
  if (cluster.initialized && cluster.mode != mode) {
    ++mode_switches_;
  }
  cluster.initialized = true;
  cluster.mode = mode;
  cluster.available = std::max(cluster.available, release_cycle) +
                      std::max<std::uint64_t>(1, cycles);
  ++tasks_;
  ++mode_tasks_.at(static_cast<std::size_t>(mode));
  if (mode == ArrayMode::Gaussian) {
    ++gaussian_broadcast_packets_;
    ++transposed_exchange_packets_;
  } else if (mode == ArrayMode::Ray) {
    ++ray_feedback_packets_;
  } else {
    ++hierarchical_reduction_packets_;
  }
  return {cluster.available, selected};
}

SupplyResult
SharedGsArray::supply(std::uint64_t packet_key,
                      const std::vector<std::uint32_t> &depth_ordered_ids,
                      std::uint64_t map_version, std::uint64_t pose_context,
                      bool projection, std::uint64_t row_bytes) {
  ++packet_sequence_;
  SupplyResult result;
  result.ordered_entries = depth_ordered_ids.size();
  depth_ordered_entries_ += result.ordered_entries;
  ResidencyGroup &group = residency_.at(packet_key % residency_.size());
  std::unordered_set<std::uint32_t> packet_rows;
  for (const std::uint32_t gaussian : depth_ordered_ids) {
    if (!packet_rows.insert(gaussian).second) {
      continue;
    }
    ++result.shared_rows;
    const ResidentKey key{gaussian, map_version, pose_context, projection};
    const auto found = group.rows.find(key);
    if (found != group.rows.end()) {
      ++result.hits;
      found->second.last_use = packet_sequence_;
      found->second.retain_until =
          std::max(found->second.retain_until, packet_sequence_ + 1);
      continue;
    }
    const bool version_miss = std::any_of(
        group.rows.begin(), group.rows.end(), [&](const auto &entry) {
          return entry.first.gaussian == gaussian &&
                 entry.first.projection == projection;
        });
    result.version_misses += version_miss ? 1U : 0U;
    while (!group.rows.empty() &&
           (group.rows.size() >= residency_entries_per_group() ||
            row_bytes > data_sram_bytes_per_group() - group.bytes)) {
      auto victim = std::min_element(
          group.rows.begin(), group.rows.end(),
          [&](const auto &left, const auto &right) {
            const bool left_retained =
                left.second.retain_until > packet_sequence_;
            const bool right_retained =
                right.second.retain_until > packet_sequence_;
            if (left_retained != right_retained) {
              return !left_retained;
            }
            return left.second.last_use < right.second.last_use;
          });
      group.bytes -= victim->second.bytes;
      group.rows.erase(victim);
    }
    if (row_bytes > data_sram_bytes_per_group()) {
      throw std::runtime_error("Gaussian row exceeds the residency SRAM");
    }
    group.rows.emplace(
        key, ResidentRow{row_bytes, packet_sequence_, packet_sequence_ + 1});
    group.bytes += row_bytes;
    ++result.fills;
    result.fill_bytes += row_bytes;
  }
  residency_hits_ += result.hits;
  residency_fills_ += result.fills;
  residency_version_misses_ += result.version_misses;
  fill_bytes_ += result.fill_bytes;
  shared_projection_rows_ += result.shared_rows;
  return result;
}

std::uint64_t SharedGsArray::tasks() const { return tasks_; }
std::uint64_t SharedGsArray::mode_switches() const { return mode_switches_; }
std::uint64_t SharedGsArray::makespan() const {
  std::uint64_t result = 0;
  for (const Cluster &cluster : clusters_) {
    result = std::max(result, cluster.available);
  }
  return result;
}
std::uint64_t SharedGsArray::tasks(ArrayMode mode) const {
  return mode_tasks_.at(static_cast<std::size_t>(mode));
}
std::uint64_t SharedGsArray::packet_reconfigurations() const {
  return mode_switches_;
}
std::uint64_t SharedGsArray::residency_hits() const { return residency_hits_; }
std::uint64_t SharedGsArray::residency_fills() const {
  return residency_fills_;
}
std::uint64_t SharedGsArray::residency_version_misses() const {
  return residency_version_misses_;
}
std::uint64_t SharedGsArray::fill_bytes() const { return fill_bytes_; }
std::uint64_t SharedGsArray::depth_ordered_entries() const {
  return depth_ordered_entries_;
}
std::uint64_t SharedGsArray::shared_projection_rows() const {
  return shared_projection_rows_;
}
std::uint64_t SharedGsArray::gaussian_broadcast_packets() const {
  return gaussian_broadcast_packets_;
}
std::uint64_t SharedGsArray::transposed_exchange_packets() const {
  return transposed_exchange_packets_;
}
std::uint64_t SharedGsArray::ray_feedback_packets() const {
  return ray_feedback_packets_;
}
std::uint64_t SharedGsArray::hierarchical_reduction_packets() const {
  return hierarchical_reduction_packets_;
}

std::string_view array_mode_name(ArrayMode mode) {
  switch (mode) {
  case ArrayMode::Gaussian:
    return "gaussian";
  case ArrayMode::Ray:
    return "ray";
  case ArrayMode::Tile:
    return "tile";
  }
  return "unknown";
}

} // namespace cisda

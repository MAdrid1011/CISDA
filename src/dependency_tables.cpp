#include "cisda/dependency_tables.hpp"

#include <algorithm>
#include <cstdint>
#include <stdexcept>

namespace cisda {

DependencyTables::DependencyTables(const MappingTopology &mapping)
    : inputs_(mapping.source_tiles_by_gaussian.size()),
      gaussians_by_tile_(mapping.gaussians_by_tile) {
  for (std::size_t gaussian = 0; gaussian < inputs_.size(); ++gaussian) {
    const auto &sources = mapping.source_tiles_by_gaussian.at(gaussian);
    InputEntry &entry = inputs_.at(gaussian);
    entry.remaining = sources.size();
    for (const std::uint32_t tile : sources) {
      entry.unfinished_xor ^= static_cast<std::uint64_t>(tile) + 1;
    }
  }
}

void DependencyTables::set_unblocked_consumers(std::uint32_t gaussian,
                                               std::uint64_t consumers) {
  inputs_.at(gaussian).consumers = consumers;
}

std::vector<ReadyGaussian>
DependencyTables::complete_mapping_tile(std::uint32_t tile) {
  if (tile >= gaussians_by_tile_.size()) {
    throw std::runtime_error("Mapping tile is outside the dependency table");
  }
  std::vector<ReadyGaussian> ready;
  for (const std::uint32_t gaussian : gaussians_by_tile_.at(tile)) {
    InputEntry &entry = inputs_.at(gaussian);
    if (entry.completed || entry.remaining == 0) {
      throw std::runtime_error("gradient source completed more than once");
    }
    ++gradient_fragments_;
    if (entry.remaining > 1) {
      ++gradient_reductions_;
    }
    entry.unfinished_xor ^= static_cast<std::uint64_t>(tile) + 1;
    --entry.remaining;
    if (entry.remaining == 1) {
      ++single_source_xor_hits_;
    }
    if (entry.remaining == 0) {
      if (entry.unfinished_xor != 0) {
        throw std::runtime_error("unfinished-source XOR did not conserve IDs");
      }
      entry.completed = true;
      ++ready_gaussians_;
      ready.push_back({gaussian, entry.consumers});
    }
  }
  std::stable_sort(
      ready.begin(), ready.end(),
      [](const ReadyGaussian &left, const ReadyGaussian &right) {
        if (left.unblocked_consumers != right.unblocked_consumers) {
          return left.unblocked_consumers > right.unblocked_consumers;
        }
        return left.gaussian < right.gaussian;
      });
  priority_propagations_ += ready.size();
  return ready;
}

void DependencyTables::register_tracking_request(
    std::uint64_t request_key, const std::vector<std::uint32_t> &tiles,
    bool speculative) {
  auto [request, inserted] = request_tiles_.try_emplace(
      request_key, std::unordered_set<std::uint32_t>{});
  static_cast<void>(inserted);
  for (const std::uint32_t tile : tiles) {
    if (!request->second.insert(tile).second) {
      continue;
    }
    if (request->second.size() > 1) {
      ++merged_requests_;
    }
    if (speculative) {
      ++pending_by_tile_[tile];
    } else {
      ++input_waits_;
    }
  }
}

void DependencyTables::resolve_tracking_request(std::uint64_t request_key) {
  const auto request = request_tiles_.find(request_key);
  if (request == request_tiles_.end()) {
    throw std::runtime_error("resolved Tracking request was not registered");
  }
  for (const std::uint32_t tile : request->second) {
    const auto pending = pending_by_tile_.find(tile);
    if (pending != pending_by_tile_.end() && pending->second != 0) {
      --pending->second;
    }
  }
}

bool DependencyTables::tile_validation_ready(std::uint32_t tile) const {
  const auto pending = pending_by_tile_.find(tile);
  return pending == pending_by_tile_.end() || pending->second == 0;
}

bool DependencyTables::reserve_spm(std::uint64_t bytes) {
  if (bytes > spm_capacity_bytes() - spm_used_) {
    ++credit_stalls_;
    return false;
  }
  spm_used_ += bytes;
  spm_high_water_ = std::max(spm_high_water_, spm_used_);
  return true;
}

void DependencyTables::release_spm(std::uint64_t bytes) {
  if (bytes > spm_used_) {
    throw std::runtime_error("SPM release exceeds its reservation");
  }
  spm_used_ -= bytes;
}

bool DependencyTables::reserve_frame_tile(std::uint64_t bytes) {
  if (bytes > frame_tile_capacity_bytes() - frame_tile_used_) {
    ++credit_stalls_;
    return false;
  }
  frame_tile_used_ += bytes;
  frame_tile_high_water_ = std::max(frame_tile_high_water_, frame_tile_used_);
  return true;
}

void DependencyTables::release_frame_tile(std::uint64_t bytes) {
  if (bytes > frame_tile_used_) {
    throw std::runtime_error("Frame Tile Buffer release exceeds reservation");
  }
  frame_tile_used_ -= bytes;
}

std::uint64_t DependencyTables::input_entries() const { return inputs_.size(); }
std::uint64_t DependencyTables::validation_entries() const {
  return pending_by_tile_.size();
}
std::uint64_t DependencyTables::gradient_fragments() const {
  return gradient_fragments_;
}
std::uint64_t DependencyTables::gradient_reductions() const {
  return gradient_reductions_;
}
std::uint64_t DependencyTables::ready_gaussians() const {
  return ready_gaussians_;
}
std::uint64_t DependencyTables::single_source_xor_hits() const {
  return single_source_xor_hits_;
}
std::uint64_t DependencyTables::merged_requests() const {
  return merged_requests_;
}
std::uint64_t DependencyTables::input_waits() const { return input_waits_; }
std::uint64_t DependencyTables::priority_propagations() const {
  return priority_propagations_;
}
std::uint64_t DependencyTables::spm_high_water_bytes() const {
  return spm_high_water_;
}
std::uint64_t DependencyTables::frame_tile_high_water_bytes() const {
  return frame_tile_high_water_;
}
std::uint64_t DependencyTables::credit_stalls() const { return credit_stalls_; }

} // namespace cisda

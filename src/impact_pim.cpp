#include "cisda/impact_pim.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <utility>

namespace cisda {

ImpactPim::ImpactPim(std::string workload, std::array<double, 6> group_budget)
    : global_budget_(group_budget) {
  if (workload == "Gaussian-SLAM") {
    field_layout_ = {"position[3]", "rotation[4]", "scale[3]", "color[3]",
                     "opacity[1]"};
  } else if (workload == "VTGaussian-SLAM") {
    field_layout_ = {"color[3]", "opacity[1]", "radius[1]"};
  } else if (workload == "SplaTAM") {
    field_layout_ = {"position[3]", "rotation[4]", "scale[1]", "color[3]",
                     "opacity[1]"};
  } else {
    throw std::runtime_error("unsupported ImpactPIM field layout");
  }
}

void ImpactPim::allocate_group_budgets(
    const std::vector<std::uint64_t> &groups) {
  std::vector<std::uint64_t> unique = groups;
  std::sort(unique.begin(), unique.end());
  unique.erase(std::unique(unique.begin(), unique.end()), unique.end());
  if (unique.empty()) {
    throw std::runtime_error("ImpactPIM requires at least one budget group");
  }
  group_budgets_.clear();
  for (const std::uint64_t group : unique) {
    std::array<double, 6> local{};
    for (std::size_t component = 0; component < local.size(); ++component) {
      local.at(component) =
          global_budget_.at(component) / static_cast<double>(unique.size());
    }
    group_budgets_.emplace(group, local);
  }
}

void ImpactPim::bind_versions(std::uint64_t visible_version,
                              std::uint64_t primary_version) {
  if (primary_version <= visible_version) {
    throw std::runtime_error("primary version must follow visible version");
  }
  source_version_ = visible_version;
  target_version_ = primary_version;
  versions_bound_ = true;
}

MergeDecision
ImpactPim::validate_group(const ImpactDescription &description,
                          const std::vector<std::uint64_t> &gaussian_ids) {
  if (!versions_bound_) {
    throw std::runtime_error("ImpactPIM versions were not bound");
  }
  if (gaussian_ids.empty()) {
    throw std::runtime_error("ImpactPIM cannot validate an empty group");
  }
  const std::uint64_t owner = description.gaussian_id % owners_.size();
  const std::uint64_t die = owner / 2;
  const std::uint64_t unit = owner % 2;
  const std::uint64_t bank = (description.gaussian_id / owners_.size()) % 4;
  for (const std::uint64_t gaussian_id : gaussian_ids) {
    const std::uint64_t member_owner = gaussian_id % owners_.size();
    const std::uint64_t member_bank =
        (gaussian_id / owners_.size()) % bank_access_counts_.at(0).size();
    owners_.at(member_owner) = true;
    ++address_mappings_;
    ++bank_access_counts_.at(member_owner).at(member_bank);
  }
  ++validation_microprograms_;
  micro_operations_ += 8 + field_layout_.size() * 6;
  if (primary_updates_ != 0) {
    ++late_validation_entries_;
  }
  bool versions_valid = true;
  for (const std::uint64_t gaussian_id : gaussian_ids) {
    auto [state, inserted] = versions_.try_emplace(
        gaussian_id, VersionState{target_version_, source_version_});
    static_cast<void>(inserted);
    ++version_checks_;
    versions_valid = versions_valid &&
                     state->second.primary == target_version_ &&
                     state->second.visible == source_version_;
  }
  const bool valid = description.context_valid && description.version_match &&
                     versions_valid && description.coverage_stable &&
                     group_budgets_.contains(description.group) &&
                     description.contribution.has_value();
  bool keep = false;
  if (valid) {
    GroupState &group = groups_[description.group];
    if (group.records.empty() || group.generation != description.generation) {
      group = GroupState{description.generation, {}, {}};
    }
    std::array<double, 6> candidate = group.cumulative;
    const auto old = group.records.find(description.gaussian_id);
    keep = true;
    for (std::size_t component = 0; component < candidate.size(); ++component) {
      if (old != group.records.end()) {
        candidate.at(component) -= std::abs(old->second.at(component));
      }
      candidate.at(component) +=
          std::abs(description.contribution->at(component));
      keep = keep && candidate.at(component) <=
                         group_budgets_.at(description.group).at(component);
    }
    if (keep) {
      group.cumulative = candidate;
      group.records[description.gaussian_id] = *description.contribution;
    } else if (old != group.records.end()) {
      for (std::size_t component = 0; component < candidate.size();
           ++component) {
        group.cumulative.at(component) -= std::abs(old->second.at(component));
        group.cumulative.at(component) =
            std::max(0.0, group.cumulative.at(component));
      }
      group.records.erase(old);
    }
  } else {
    invalid_descriptions_ += gaussian_ids.size();
    const auto group = groups_.find(description.group);
    if (group != groups_.end() &&
        group->second.generation == description.generation) {
      const auto old = group->second.records.find(description.gaussian_id);
      if (old != group->second.records.end()) {
        for (std::size_t component = 0;
             component < group->second.cumulative.size(); ++component) {
          group->second.cumulative.at(component) =
              std::max(0.0, group->second.cumulative.at(component) -
                                std::abs(old->second.at(component)));
        }
        group->second.records.erase(old);
      }
    }
  }
  if (keep) {
    confirmations_ += gaussian_ids.size();
  } else {
    advances_ += gaussian_ids.size();
    const std::uint64_t full_mask =
        (std::uint64_t{1} << field_layout_.size()) - 1;
    const std::uint64_t mask =
        description.changed_field_mask.value_or(full_mask) & full_mask;
    selected_field_writes_ +=
        static_cast<std::uint64_t>(std::popcount(mask)) * gaussian_ids.size();
  }
  for (const std::uint64_t gaussian_id : gaussian_ids) {
    VersionState &state = versions_.at(gaussian_id);
    if (!keep) {
      state.visible = target_version_;
    }
    completion_queue_.push_back(
        {gaussian_id % owners_.size(), state.visible, description.group, keep});
    ++completion_records_;
  }
  completion_queue_high_water_ = std::max<std::uint64_t>(
      completion_queue_high_water_, completion_queue_.size());
  return {keep,
          valid,
          owner,
          die,
          unit,
          bank,
          keep ? 0U : static_cast<std::uint64_t>(field_layout_.size())};
}

void ImpactPim::account_primary_updates(std::uint64_t objects,
                                        std::uint64_t scalars,
                                        std::uint64_t bytes) {
  primary_updates_ += objects;
  primary_bytes_ += bytes;
  descriptors_ += objects;
  update_microprograms_ += objects;
  micro_operations_ += scalars * 13;
  address_mappings_ += objects;
  bank_accesses_ += objects * 2;
  for (std::uint64_t object = 0; object < objects; ++object) {
    const std::uint64_t owner = object % owners_.size();
    const std::uint64_t bank = (object / owners_.size()) % 4;
    owners_.at(owner) = true;
    bank_access_counts_.at(owner).at(bank) += 2;
  }
}

void ImpactPim::account_visible_bytes(std::uint64_t bytes) {
  visible_bytes_ += bytes;
}

void ImpactPim::account_commands(std::uint64_t commands) {
  commands_ += commands;
  row_groups_ += commands;
  if (commands != 0) {
    owners_.fill(true);
  }
}

void ImpactPim::service_refresh(std::uint64_t pim_cycles) {
  constexpr std::uint64_t refresh_interval_cycles = 98;
  const std::uint64_t intervals = pim_cycles / refresh_interval_cycles;
  const std::uint64_t active = active_owners();
  refresh_drains_ += intervals * active;
  refresh_precharges_ += intervals * active;
  refreshes_ += intervals * active;
  retained_state_resumes_ += intervals * active;
}

void ImpactPim::drain_completions() { completion_queue_.clear(); }

std::uint64_t ImpactPim::primary_updates() const { return primary_updates_; }
std::uint64_t ImpactPim::primary_bytes() const { return primary_bytes_; }
std::uint64_t ImpactPim::visible_bytes() const { return visible_bytes_; }
std::uint64_t ImpactPim::commands() const { return commands_; }
std::uint64_t ImpactPim::confirmations() const { return confirmations_; }
std::uint64_t ImpactPim::advances() const { return advances_; }
std::uint64_t ImpactPim::invalid_descriptions() const {
  return invalid_descriptions_;
}
std::uint64_t ImpactPim::active_owners() const {
  return std::count(owners_.begin(), owners_.end(), true);
}
std::uint64_t ImpactPim::descriptors() const { return descriptors_; }
std::uint64_t ImpactPim::descriptor_bytes() const { return descriptors_ * 32; }
std::uint64_t ImpactPim::update_microprograms() const {
  return update_microprograms_;
}
std::uint64_t ImpactPim::validation_microprograms() const {
  return validation_microprograms_;
}
std::uint64_t ImpactPim::micro_operations() const { return micro_operations_; }
std::uint64_t ImpactPim::address_mappings() const { return address_mappings_; }
std::uint64_t ImpactPim::interleaved_bank_accesses() const {
  return bank_accesses_;
}
std::uint64_t ImpactPim::selected_field_writes() const {
  return selected_field_writes_;
}
std::uint64_t ImpactPim::completion_records() const {
  return completion_records_;
}
std::uint64_t ImpactPim::completion_queue_high_water() const {
  return completion_queue_high_water_;
}
std::uint64_t ImpactPim::late_validation_entries() const {
  return late_validation_entries_;
}
std::uint64_t ImpactPim::same_priority_row_groups() const {
  return row_groups_;
}
std::uint64_t ImpactPim::refresh_drains() const { return refresh_drains_; }
std::uint64_t ImpactPim::refresh_precharges() const {
  return refresh_precharges_;
}
std::uint64_t ImpactPim::refreshes() const { return refreshes_; }
std::uint64_t ImpactPim::retained_state_resumes() const {
  return retained_state_resumes_;
}
std::uint64_t ImpactPim::version_state_records() const {
  return versions_.size();
}
std::uint64_t ImpactPim::version_checks() const { return version_checks_; }
std::uint64_t ImpactPim::active_bank_lanes() const {
  std::uint64_t active = 0;
  for (const auto &owner : bank_access_counts_) {
    active += static_cast<std::uint64_t>(
        std::count_if(owner.begin(), owner.end(),
                      [](std::uint64_t accesses) { return accesses != 0; }));
  }
  return active;
}
std::uint64_t ImpactPim::budget_groups() const { return group_budgets_.size(); }
const std::vector<std::string> &ImpactPim::field_layout() const {
  return field_layout_;
}

} // namespace cisda

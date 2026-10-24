#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace cisda {

struct ImpactDescription {
  bool context_valid{};
  bool version_match{};
  bool coverage_stable{};
  std::uint64_t group{};
  std::uint64_t generation{};
  std::uint64_t gaussian_id{};
  std::optional<std::array<double, 6>> contribution;
  std::optional<std::uint64_t> changed_field_mask;
};

struct MergeDecision {
  bool kept_visible{};
  bool description_valid{};
  std::uint64_t owner{};
  std::uint64_t die{};
  std::uint64_t unit{};
  std::uint64_t bank{};
  std::uint64_t changed_fields{};
};

class ImpactPim {
public:
  ImpactPim(std::string workload, std::array<double, 6> group_budget);

  void bind_versions(std::uint64_t visible_version,
                     std::uint64_t primary_version);
  void allocate_group_budgets(const std::vector<std::uint64_t> &groups);
  MergeDecision validate_group(const ImpactDescription &description,
                               const std::vector<std::uint64_t> &gaussian_ids);
  void account_primary_updates(std::uint64_t objects, std::uint64_t scalars,
                               std::uint64_t bytes);
  void account_visible_bytes(std::uint64_t bytes);
  void account_commands(std::uint64_t commands);
  void service_refresh(std::uint64_t pim_cycles);
  void drain_completions();

  std::uint64_t primary_updates() const;
  std::uint64_t primary_bytes() const;
  std::uint64_t visible_bytes() const;
  std::uint64_t commands() const;
  std::uint64_t confirmations() const;
  std::uint64_t advances() const;
  std::uint64_t invalid_descriptions() const;
  std::uint64_t active_owners() const;
  std::uint64_t descriptors() const;
  std::uint64_t descriptor_bytes() const;
  std::uint64_t update_microprograms() const;
  std::uint64_t validation_microprograms() const;
  std::uint64_t micro_operations() const;
  std::uint64_t address_mappings() const;
  std::uint64_t interleaved_bank_accesses() const;
  std::uint64_t selected_field_writes() const;
  std::uint64_t completion_records() const;
  std::uint64_t completion_queue_high_water() const;
  std::uint64_t late_validation_entries() const;
  std::uint64_t same_priority_row_groups() const;
  std::uint64_t refresh_drains() const;
  std::uint64_t refresh_precharges() const;
  std::uint64_t refreshes() const;
  std::uint64_t retained_state_resumes() const;
  std::uint64_t version_state_records() const;
  std::uint64_t version_checks() const;
  std::uint64_t active_bank_lanes() const;
  std::uint64_t budget_groups() const;
  const std::vector<std::string> &field_layout() const;

private:
  struct GroupState {
    std::uint64_t generation{};
    std::array<double, 6> cumulative{};
    std::unordered_map<std::uint64_t, std::array<double, 6>> records;
  };

  struct Completion {
    std::uint64_t owner{};
    std::uint64_t version{};
    std::uint64_t context{};
    bool confirmation{};
  };

  struct VersionState {
    std::uint64_t primary{};
    std::uint64_t visible{};
  };

  std::array<double, 6> global_budget_;
  std::unordered_map<std::uint64_t, std::array<double, 6>> group_budgets_;
  std::vector<std::string> field_layout_;
  std::unordered_map<std::uint64_t, GroupState> groups_;
  std::unordered_map<std::uint64_t, VersionState> versions_;
  std::array<std::array<std::uint64_t, 4>, 8> bank_access_counts_{};
  std::array<bool, 8> owners_{};
  std::uint64_t source_version_{};
  std::uint64_t target_version_{};
  bool versions_bound_{};
  std::uint64_t primary_updates_{};
  std::uint64_t primary_bytes_{};
  std::uint64_t visible_bytes_{};
  std::uint64_t commands_{};
  std::uint64_t confirmations_{};
  std::uint64_t advances_{};
  std::uint64_t invalid_descriptions_{};
  std::deque<Completion> completion_queue_;
  std::uint64_t descriptors_{};
  std::uint64_t update_microprograms_{};
  std::uint64_t validation_microprograms_{};
  std::uint64_t micro_operations_{};
  std::uint64_t address_mappings_{};
  std::uint64_t bank_accesses_{};
  std::uint64_t selected_field_writes_{};
  std::uint64_t completion_records_{};
  std::uint64_t completion_queue_high_water_{};
  std::uint64_t late_validation_entries_{};
  std::uint64_t row_groups_{};
  std::uint64_t refresh_drains_{};
  std::uint64_t refresh_precharges_{};
  std::uint64_t refreshes_{};
  std::uint64_t retained_state_resumes_{};
  std::uint64_t version_checks_{};
};

} // namespace cisda

#pragma once

#include <filesystem>

#include <nlohmann/json.hpp>

namespace cisda {

class Simulator {
public:
  nlohmann::json run(const std::filesystem::path &manifest_path) const;
};

} // namespace cisda

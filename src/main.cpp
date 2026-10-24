#include "cisda/simulator.hpp"

#include <exception>
#include <filesystem>
#include <iostream>

int main(int argc, char **argv) {
  if (argc != 2) {
    std::cerr << "usage: cisda-sim WORKLOAD.json\n";
    return 64;
  }
  try {
    std::cout << cisda::Simulator().run(std::filesystem::path(argv[1])).dump(2)
              << '\n';
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "cisda-sim: " << error.what() << '\n';
    return 2;
  }
}

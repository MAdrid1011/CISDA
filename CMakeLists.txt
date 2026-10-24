cmake_minimum_required(VERSION 3.22)

project(cisda_simulator VERSION 1.0.0 LANGUAGES CXX)

find_package(nlohmann_json 3.11 REQUIRED)
find_package(ZLIB REQUIRED)

add_library(cisda_simulator
  src/dependency_tables.cpp
  src/impact_pim.cpp
  src/ordering.cpp
  src/predictor.cpp
  src/reverse_scheduler.cpp
  src/shared_array.cpp
  src/simulator.cpp
  src/tile_maintenance.cpp
  src/workload.cpp
)
target_compile_features(cisda_simulator PUBLIC cxx_std_20)
target_include_directories(cisda_simulator PUBLIC include)
target_link_libraries(cisda_simulator
  PUBLIC nlohmann_json::nlohmann_json
  PRIVATE ZLIB::ZLIB
)
target_compile_options(cisda_simulator PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_executable(cisda-sim src/main.cpp)
target_link_libraries(cisda-sim PRIVATE cisda_simulator)

install(TARGETS cisda-sim RUNTIME DESTINATION bin)

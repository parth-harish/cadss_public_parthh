project(branche_simulator)
add_library(branche_simulator SHARED branch.c)
target_include_directories(branche_simulator PRIVATE ../common)

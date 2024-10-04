project(branch_simulator)
add_library(branch_simulator SHARED branch.c)
target_include_directories(branch_simulator PRIVATE ../common)
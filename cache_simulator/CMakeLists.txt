project(cache_simulator)
add_library(cache_simulator SHARED cache.c stree.c)
target_include_directories(cache_simulator PRIVATE ../common)

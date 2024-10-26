project(processor_simulator)
add_library(processor_simulator SHARED processor.c)
target_include_directories(processor_simulator PRIVATE ../common ../cache_simulator)

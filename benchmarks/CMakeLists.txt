# Cross-Cluster State Fabric - benchmarks.

add_executable(ccsf_benchmarks bench_main.cpp)
target_link_libraries(ccsf_benchmarks PRIVATE cross_cluster_state_fabric)
ccsf_apply_warnings(ccsf_benchmarks)

install(TARGETS ccsf_benchmarks RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

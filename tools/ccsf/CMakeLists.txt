# Cross-Cluster State Fabric - inspection CLI.

add_executable(ccsf_cli ccsf_cli.cpp)
target_link_libraries(ccsf_cli PRIVATE cross_cluster_state_fabric)
ccsf_apply_warnings(ccsf_cli)

install(TARGETS ccsf_cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

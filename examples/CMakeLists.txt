# Cross-Cluster State Fabric - runnable examples.
#
# Every example compiles against the public headers only.

set(CCSF_EXAMPLE_SOURCES
  ex_basic_replication.cpp
  ex_migration.cpp
  ex_transfer_vs_reconstruction.cpp
  ex_incompatible_destination.cpp
  ex_stale_replica.cpp
  ex_cluster_reincarnation.cpp
  ex_under_replicated_repair.cpp
  ex_coordinator_restart.cpp
  ex_deterministic_explanation.cpp
  ex_cuda_state.cpp
)

foreach(example_source ${CCSF_EXAMPLE_SOURCES})
  get_filename_component(example_name ${example_source} NAME_WE)
  add_executable(${example_name} ${example_source})
  target_include_directories(${example_name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(${example_name} PRIVATE cross_cluster_state_fabric)
  ccsf_apply_warnings(${example_name})
  if(CROSS_CLUSTER_STATE_ENABLE_CUDA AND example_name STREQUAL "ex_cuda_state")
    target_compile_definitions(${example_name} PRIVATE CCSF_EXAMPLE_CUDA=1)
    target_include_directories(${example_name} PRIVATE ${CMAKE_SOURCE_DIR}/cuda)
    target_link_libraries(${example_name} PRIVATE ccsf_cuda_backend)
  endif()
endforeach()

# Cross-Cluster State Fabric - test suites.
#
# Every suite exposes stable case identifiers ("<suite>::<case>") that can be
# selected individually with --case. No case runs under a timeout.

set(CCSF_TEST_SOURCES
  main.cpp
  framework.cpp
  test_foundation.cpp
  test_model.cpp
  test_fabric_state.cpp
  test_authority.cpp
  test_replication.cpp
  test_migration.cpp
  test_placement_economics.cpp
  test_persistence.cpp
  test_transport.cpp
  test_multiprocess.cpp
  test_splitbrain.cpp
  test_property.cpp
  test_adversarial.cpp
  test_concurrency.cpp
  test_cuda.cpp
)

add_executable(ccsf_tests ${CCSF_TEST_SOURCES})
target_link_libraries(ccsf_tests PRIVATE cross_cluster_state_fabric)
target_include_directories(ccsf_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
ccsf_apply_warnings(ccsf_tests)

# The multiprocess proof drives the reference deployment binaries.
add_dependencies(ccsf_tests ccsf_coordinator ccsf_cluster_agent)
target_compile_definitions(ccsf_tests PRIVATE
  CCSF_TEST_COORDINATOR_PATH="$<TARGET_FILE:ccsf_coordinator>"
  CCSF_TEST_AGENT_PATH="$<TARGET_FILE:ccsf_cluster_agent>")

if(CROSS_CLUSTER_STATE_ENABLE_CUDA)
  target_compile_definitions(ccsf_tests PRIVATE CCSF_TEST_CUDA=1)
  target_link_libraries(ccsf_tests PRIVATE ccsf_cuda_backend)
endif()

add_test(NAME ccsf_tests COMMAND ccsf_tests)

# The sanitizer self test is only meaningful when instrumentation is enabled: it
# must produce a sanitizer report rather than pass silently.
if(CROSS_CLUSTER_STATE_ENABLE_ASAN)
  add_executable(ccsf_sanitizer_self_test sanitizer_self_test.cpp)
  target_link_libraries(ccsf_sanitizer_self_test PRIVATE cross_cluster_state_fabric)
  ccsf_apply_warnings(ccsf_sanitizer_self_test)
endif()

cmake_minimum_required(VERSION 3.25)

project(CrossClusterStateFabric
        VERSION 1.0.0
        DESCRIPTION "Vendor-neutral C++20 runtime for authoritative cross-cluster state placement, replication, migration, reuse, durability and integrity"
        LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(CROSS_CLUSTER_STATE_BUILD_TESTS "Build the Cross-Cluster State Fabric test suites" ON)
option(CROSS_CLUSTER_STATE_BUILD_EXAMPLES "Build the Cross-Cluster State Fabric examples" ON)
option(CROSS_CLUSTER_STATE_BUILD_BENCHMARKS "Build the Cross-Cluster State Fabric benchmarks" ON)
option(CROSS_CLUSTER_STATE_BUILD_CLI "Build the Cross-Cluster State Fabric inspection CLI" ON)
option(CROSS_CLUSTER_STATE_ENABLE_CUDA "Build the CUDA-backed state proof" OFF)
option(CROSS_CLUSTER_STATE_WARNINGS_AS_ERRORS "Treat first-party warnings as errors" ON)
option(CROSS_CLUSTER_STATE_ENABLE_ASAN "Instrument first-party code with AddressSanitizer" OFF)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# ---------------------------------------------------------------------------
# First-party warning discipline. Applied to first-party targets only.
# ---------------------------------------------------------------------------
function(ccsf_apply_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE
      /W4 /permissive- /utf-8 /Zc:__cplusplus /EHsc /MP
      /w14242 /w14254 /w14263 /w14265 /w14287 /w14296 /w14311 /w14545 /w14546 /w14547
      /w14549 /w14555 /w14619 /w14640 /w14826 /w14905 /w14906 /w14928)
    if(CROSS_CLUSTER_STATE_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
    if(CROSS_CLUSTER_STATE_ENABLE_ASAN)
      target_compile_options(${target} PRIVATE /fsanitize=address /Zi)
      target_link_options(${target} PRIVATE /INCREMENTAL:NO)
    endif()
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    if(CROSS_CLUSTER_STATE_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
    if(CROSS_CLUSTER_STATE_ENABLE_ASAN)
      target_compile_options(${target} PRIVATE -fsanitize=address -fno-omit-frame-pointer)
      target_link_options(${target} PRIVATE -fsanitize=address)
    endif()
  endif()
endfunction()

# ---------------------------------------------------------------------------
# Core library
# ---------------------------------------------------------------------------
set(CCSF_CORE_SOURCES
  src/version.cpp
  src/error.cpp
  src/ids.cpp
  src/bytes.cpp
  src/clock.cpp
  src/digest.cpp
  src/crc32c.cpp
  src/taxonomy.cpp
  src/cost.cpp
  src/compatibility.cpp
  src/model.cpp
  src/placement.cpp
  src/economics.cpp
  src/authority.cpp
  src/detail/encoding.cpp
  src/detail/platform_io.cpp
  src/detail/control_plane.cpp
  src/fabric_lifecycle.cpp
  src/fabric_state.cpp
  src/fabric_replica.cpp
  src/fabric_ops.cpp
  src/protocol.cpp
  src/transport.cpp
  src/process.cpp
  src/cluster_agent.cpp
  src/coordinator.cpp
)

add_library(cross_cluster_state_fabric ${CCSF_CORE_SOURCES})
add_library(CrossClusterStateFabric::cross_cluster_state_fabric ALIAS cross_cluster_state_fabric)

target_include_directories(cross_cluster_state_fabric
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_definitions(cross_cluster_state_fabric PRIVATE CCSF_BUILDING_LIBRARY=1)

find_package(Threads REQUIRED)
target_link_libraries(cross_cluster_state_fabric PRIVATE Threads::Threads)
if(WIN32)
  target_link_libraries(cross_cluster_state_fabric PRIVATE ws2_32)
endif()

set_target_properties(cross_cluster_state_fabric PROPERTIES
  OUTPUT_NAME "cross_cluster_state_fabric"
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
  POSITION_INDEPENDENT_CODE ON)

ccsf_apply_warnings(cross_cluster_state_fabric)

# ---------------------------------------------------------------------------
# Installation and CMake package export
# ---------------------------------------------------------------------------
install(TARGETS cross_cluster_state_fabric
        EXPORT CrossClusterStateFabricTargets
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(DIRECTORY include/ccsf DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT CrossClusterStateFabricTargets
        FILE CrossClusterStateFabricTargets.cmake
        NAMESPACE CrossClusterStateFabric::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/CrossClusterStateFabric)

configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CrossClusterStateFabricConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/CrossClusterStateFabricConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/CrossClusterStateFabric)

write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/CrossClusterStateFabricConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)

install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/CrossClusterStateFabricConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/CrossClusterStateFabricConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/CrossClusterStateFabric)

# ---------------------------------------------------------------------------
# Optional CUDA-backed state example. Declared before the test target so that
# the CUDA test cases can link the example backend.
# ---------------------------------------------------------------------------
if(CROSS_CLUSTER_STATE_ENABLE_CUDA)
  include(CheckLanguage)
  check_language(CUDA)
  if(CMAKE_CUDA_COMPILER)
    enable_language(CUDA)
    add_subdirectory(cuda)
  else()
    message(FATAL_ERROR "CROSS_CLUSTER_STATE_ENABLE_CUDA was requested but no CUDA compiler was found")
  endif()
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(CROSS_CLUSTER_STATE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(CROSS_CLUSTER_STATE_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

if(CROSS_CLUSTER_STATE_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()

if(CROSS_CLUSTER_STATE_BUILD_CLI)
  add_subdirectory(tools/ccsf)
endif()

# Reference deployment executables are always built: the multiprocess proof
# depends on them and they are the documented reference deployment.
add_subdirectory(apps)

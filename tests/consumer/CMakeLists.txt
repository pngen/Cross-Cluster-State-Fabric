cmake_minimum_required(VERSION 3.25)
project(CcsfConsumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The consumer must find the installed package, never the source tree.
find_package(CrossClusterStateFabric CONFIG REQUIRED)

add_executable(ccsf_consumer main.cpp)
target_link_libraries(ccsf_consumer PRIVATE CrossClusterStateFabric::cross_cluster_state_fabric)

if(MSVC)
  target_compile_options(ccsf_consumer PRIVATE /W4 /WX /permissive- /utf-8)
endif()

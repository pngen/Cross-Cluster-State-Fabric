# Cross-Cluster State Fabric - CUDA-backed state example.
#
# The CUDA device code is compiled with the same strict host warnings as the
# rest of the project, expressed through nvcc's host-compiler passthrough. The
# generic helper is not used here because /MP is meaningless for a single CUDA
# translation unit and is rejected by nvcc.

add_library(ccsf_cuda_backend ccsf_cuda_backend.cu)
target_include_directories(ccsf_cuda_backend
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../include ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ccsf_cuda_backend PUBLIC cross_cluster_state_fabric)

if(MSVC)
  target_compile_options(ccsf_cuda_backend PRIVATE
    $<$<COMPILE_LANGUAGE:CUDA>:-Xcompiler=/W4,-permissive-,-utf-8,-WX>)
endif()

add_executable(ccsf_cuda_proof ccsf_cuda_proof_main.cpp)
target_include_directories(ccsf_cuda_proof PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ccsf_cuda_proof PRIVATE ccsf_cuda_backend)
ccsf_apply_warnings(ccsf_cuda_proof)

install(TARGETS ccsf_cuda_proof RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

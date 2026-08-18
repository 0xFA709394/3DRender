add_executable(perf_test
  main.cpp
  ${CMAKE_SOURCE_DIR}/tests/common/shader_code.cpp
  ${CMAKE_SOURCE_DIR}/tests/common/image.cpp
  ${CMAKE_SOURCE_DIR}/tests/common/skinned_gen.cpp
)
target_link_libraries(perf_test PRIVATE rd_core ktx)
target_include_directories(perf_test PRIVATE
  ${CMAKE_SOURCE_DIR}/tests
  ${CMAKE_SOURCE_DIR}
  ${CMAKE_BINARY_DIR}/generated
  ${stb_SOURCE_DIR}
  ${ktx_SOURCE_DIR}/include)
target_compile_definitions(perf_test PRIVATE
  RD_PERF_BASELINE_PATH="${CMAKE_SOURCE_DIR}/tests/perf/baseline.json"
  RD_PERF_ASSETS_DIR="${CMAKE_SOURCE_DIR}/tests/assets")
set_target_properties(perf_test PROPERTIES
  BUILD_RPATH "/opt/homebrew/lib;/usr/local/lib;$ENV{VULKAN_SDK}/lib")

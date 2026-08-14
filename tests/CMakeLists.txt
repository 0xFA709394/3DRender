include(GoogleTest)

add_executable(rd_tests
  foundation/smoke_test.cpp
  foundation/handle_test.cpp
  foundation/log_test.cpp
  foundation/math_test.cpp
  foundation/task_queue_test.cpp
  rhi/rhi_types_test.cpp
  rhi/factory_test.cpp
  rhi/caps_test.cpp
  rhi/buffer_memory_test.cpp
  common/image.cpp
  common/image_test.cpp
  common/shader_code.cpp
  rhi/cube_test.cpp
  rhi/swapchain_test.cpp
  rhi/texture_test.cpp
  rhi/texture_quad_test.cpp
  api/api_test.cpp
)
target_link_libraries(rd_tests PRIVATE rd_core GTest::gtest_main)
target_include_directories(rd_tests PRIVATE
  ${CMAKE_BINARY_DIR}/generated
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${stb_SOURCE_DIR})
target_compile_definitions(rd_tests PRIVATE RD_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(rd_tests PROPERTIES
  BUILD_RPATH "/opt/homebrew/lib;/usr/local/lib;$ENV{VULKAN_SDK}/lib")
gtest_discover_tests(rd_tests)

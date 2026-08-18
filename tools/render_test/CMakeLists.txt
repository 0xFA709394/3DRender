add_executable(render_test
  main.cpp
  scenes.cpp
  ${CMAKE_SOURCE_DIR}/tests/common/shader_code.cpp
  ${CMAKE_SOURCE_DIR}/tests/common/image.cpp
  ${CMAKE_SOURCE_DIR}/tests/common/skinned_gen.cpp
)
target_link_libraries(render_test PRIVATE rd_core ktx)
target_include_directories(render_test PRIVATE
  ${CMAKE_SOURCE_DIR}/tests
  ${CMAKE_SOURCE_DIR}
  ${CMAKE_BINARY_DIR}/generated
  ${stb_SOURCE_DIR}
  ${ktx_SOURCE_DIR}/include)
set_target_properties(render_test PROPERTIES
  BUILD_RPATH "/opt/homebrew/lib;/usr/local/lib;$ENV{VULKAN_SDK}/lib")

# --interactive:GLFW 窗口 + Metal swapchain(macOS host)
if(APPLE)
  target_sources(render_test PRIVATE interactive.mm)
  target_include_directories(render_test PRIVATE ${CMAKE_SOURCE_DIR})
  target_link_libraries(render_test PRIVATE glfw "-framework Cocoa" "-framework QuartzCore")
endif()

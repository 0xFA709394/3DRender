add_library(rd_core STATIC
  foundation/version.cpp
  foundation/log.cpp
  foundation/task_queue.cpp
  rhi/rhi_factory.cpp
  rhi/rhi_capability.cpp
  resource/image_codec.cpp
  resource/ktx2_codec.cpp
  resource/gltf_loader.cpp
  resource/mesh_render_resource.cpp
  resource/mesh_utils.cpp
  resource/primitives.cpp
  renderer/renderer.cpp
  renderer/mesh_renderable.cpp
  renderer/environment.cpp
  renderer/quality.cpp
  scene/cube_scene.cpp
  scene/node.cpp
  scene/orbit_controller.cpp
  scene/animator.cpp
  scene/picking.cpp
  scene/scene.cpp
  api/command_bus.cpp
  api/rd_api.cpp
)
target_include_directories(rd_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
  ${stb_SOURCE_DIR} ${cgltf_SOURCE_DIR})
target_link_libraries(rd_core PUBLIC glm::glm)
target_link_libraries(rd_core PRIVATE ktx)
target_include_directories(rd_core PRIVATE ${ktx_SOURCE_DIR}/include)
target_compile_definitions(rd_core PUBLIC GLM_FORCE_DEPTH_ZERO_TO_ONE)

# 内嵌 shader：host 构建 build 期生成；ANDROID/IOS configure 期生成
if(ANDROID)
  rd_embed_shaders_configure(${CMAKE_BINARY_DIR}/generated/embedded_shaders.cpp
    ${RD_HOST_SHADER_OUT} ${RD_HOST_SHADER_OUT}_ios)
  target_sources(rd_core PRIVATE ${CMAKE_BINARY_DIR}/generated/embedded_shaders.cpp)
elseif(IOS)
  rd_embed_shaders_configure(${CMAKE_BINARY_DIR}/generated/embedded_shaders.cpp
    ${RD_HOST_SHADER_OUT} ${RD_HOST_SHADER_OUT}_ios)
  if(CMAKE_OSX_SYSROOT MATCHES "[Ss]imulator") # iphonesimulator / iPhoneSimulator.sdk
    target_compile_definitions(rd_core PRIVATE RD_EMBED_IOS_SIMULATOR=1)
  else()
    target_compile_definitions(rd_core PRIVATE RD_EMBED_IOS_METAL=1)
  endif()
  target_sources(rd_core PRIVATE ${CMAKE_BINARY_DIR}/generated/embedded_shaders.cpp)
else()
  set(RD_EMBEDDED_CPP ${CMAKE_BINARY_DIR}/generated/embedded_shaders.cpp)
  add_custom_command(
    OUTPUT ${RD_EMBEDDED_CPP}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/generated
    COMMAND ${CMAKE_COMMAND} -DOUT=${RD_EMBEDDED_CPP} -DDIR=${RD_SHADER_OUT}
            -DDIR_IOS=${RD_SHADER_OUT}_ios -DDIR_IOSSIM=${RD_SHADER_OUT}_iossim
            -P ${CMAKE_SOURCE_DIR}/cmake/GenEmbedded.cmake
    DEPENDS shader_cube.vert shader_cube.frag shader_unlit.vert shader_unlit.frag
            shader_pbr_forward.vert shader_pbr_forward.frag shader_prefilter.vert
            shader_prefilter.frag shader_blit.vert shader_blit.frag
    COMMENT "生成 embedded_shaders.cpp")
  target_sources(rd_core PRIVATE ${RD_EMBEDDED_CPP})
endif()

# 声明式选项:options.json → options_generated(host=build 期;移动端 configure 期)
if(ANDROID OR IOS)
  set(RD_OPTIONS_CPP ${CMAKE_BINARY_DIR}/generated/options_generated.cpp)
  execute_process(
    COMMAND ${CMAKE_COMMAND} -DJSON=${CMAKE_CURRENT_SOURCE_DIR}/api/options.json
            -DOUT_H=${CMAKE_BINARY_DIR}/generated/options_generated.h
            -DOUT_CPP=${RD_OPTIONS_CPP}
            -P ${CMAKE_SOURCE_DIR}/cmake/GenOptions.cmake
    RESULT_VARIABLE res)
  if(NOT res EQUAL 0)
    message(FATAL_ERROR "options 生成失败")
  endif()
  target_sources(rd_core PRIVATE ${RD_OPTIONS_CPP})
  target_include_directories(rd_core PUBLIC ${CMAKE_BINARY_DIR}/generated)
endif()

# 声明式选项:options.json → options_generated(host=build 期;移动端 configure 期)
if(NOT ANDROID AND NOT IOS)
  set(RD_OPTIONS_CPP ${CMAKE_BINARY_DIR}/generated/options_generated.cpp)
  add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/generated/options_generated.h ${RD_OPTIONS_CPP}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/generated
    COMMAND ${CMAKE_COMMAND} -DJSON=${CMAKE_CURRENT_SOURCE_DIR}/api/options.json
            -DOUT_H=${CMAKE_BINARY_DIR}/generated/options_generated.h
            -DOUT_CPP=${RD_OPTIONS_CPP}
            -P ${CMAKE_SOURCE_DIR}/cmake/GenOptions.cmake
    DEPENDS api/options.json ${CMAKE_SOURCE_DIR}/cmake/GenOptions.cmake
    COMMENT "生成 options_generated")
  target_sources(rd_core PRIVATE ${RD_OPTIONS_CPP})
  target_include_directories(rd_core PUBLIC ${CMAKE_BINARY_DIR}/generated)
endif()

if(APPLE)
  target_sources(rd_core PRIVATE rhi/backends/metal/metal_device.mm)
  set_source_files_properties(rhi/backends/metal/metal_device.mm
    PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
  target_compile_definitions(rd_core PRIVATE RD_WITH_METAL=1)
  target_link_libraries(rd_core PRIVATE "-framework Metal" "-framework Foundation" "-framework QuartzCore")
endif()

find_library(RD_MOLTENVK_LIB MoltenVK
  PATHS /opt/homebrew/lib /usr/local/lib "$ENV{VULKAN_SDK}/lib")
if(RD_MOLTENVK_LIB AND NOT ANDROID AND NOT IOS)
  message(STATUS "MoltenVK found: ${RD_MOLTENVK_LIB}")
  target_sources(rd_core PRIVATE rhi/backends/vulkan/vulkan_device.cpp)
  target_compile_definitions(rd_core PUBLIC RD_WITH_VULKAN=1)
  target_link_libraries(rd_core PRIVATE Vulkan::Headers ${RD_MOLTENVK_LIB})
  set(RD_HAVE_VULKAN ON CACHE INTERNAL "vulkan 后端已编译(host MoltenVK)")
endif()

if(ANDROID)
  target_sources(rd_core PRIVATE rhi/backends/gles/gles_device.cpp)
  target_link_libraries(rd_core PRIVATE EGL GLESv3 android log vulkan)
  target_sources(rd_core PRIVATE rhi/backends/vulkan/vulkan_device.cpp)
  target_compile_definitions(rd_core PUBLIC RD_WITH_VULKAN=1)
  target_link_libraries(rd_core PRIVATE Vulkan::Headers)
endif()

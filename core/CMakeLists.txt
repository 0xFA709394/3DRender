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
  renderer/renderer.cpp
  renderer/mesh_renderable.cpp
  renderer/environment.cpp
  scene/cube_scene.cpp
  scene/node.cpp
  scene/scene.cpp
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
    DEPENDS shader_cube.vert shader_cube.frag
    COMMENT "生成 embedded_shaders.cpp")
  target_sources(rd_core PRIVATE ${RD_EMBEDDED_CPP})
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
endif()

if(ANDROID)
  target_sources(rd_core PRIVATE rhi/backends/gles/gles_device.cpp)
  target_link_libraries(rd_core PRIVATE EGL GLESv3 android log vulkan)
  target_sources(rd_core PRIVATE rhi/backends/vulkan/vulkan_device.cpp)
  target_compile_definitions(rd_core PUBLIC RD_WITH_VULKAN=1)
  target_link_libraries(rd_core PRIVATE Vulkan::Headers)
endif()

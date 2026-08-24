# ============================================================================
# 第三方依赖（全部经 FetchContent 按源码拉取，无系统包管理器依赖）
#
# - glm：全平台数学库（配合 rd_core 公开的 GLM_FORCE_DEPTH_ZERO_TO_ONE 宏）
# - VulkanHeaders：Android 与 host 需要（头文件 only；iOS 走 Metal 不需要）
# - 以下仅 host（非 ANDROID/IOS）：
#     googletest   单元测试框架
#     glslang      GLSL → SPIR-V（shader 离线管线第一步）
#     spirv-cross  SPIR-V → MSL/GLSL ES/反射 JSON（第二、三步）
#     stb          PNG 读写（测试/工具用）
# ============================================================================
include(FetchContent)

FetchContent_Declare(glm
  URL https://github.com/g-truc/glm/archive/refs/tags/1.0.1.tar.gz)
FetchContent_MakeAvailable(glm)

# stb/cgltf:纯 C 头文件库,全平台需要(image_codec/gltf_loader 编进 rd_core)
FetchContent_Declare(stb
  URL https://github.com/nothings/stb/archive/refs/heads/master.tar.gz)
FetchContent_MakeAvailable(stb)

FetchContent_Declare(cgltf
  URL https://github.com/jkuhlmann/cgltf/archive/refs/tags/v1.14.tar.gz)
FetchContent_MakeAvailable(cgltf)

# KTX-Software:KTX2/BasisU 解码与转码(裁剪:无 tools/tests/doc,静态库)
set(KTX_FEATURE_TOOLS OFF CACHE BOOL "" FORCE)
set(KTX_FEATURE_TESTS OFF CACHE BOOL "" FORCE)
set(KTX_FEATURE_DOC OFF CACHE BOOL "" FORCE)
set(KTX_FEATURE_LOADTEST_APPS OFF CACHE BOOL "" FORCE)
set(KTX_FEATURE_STATIC_LIBRARY ON CACHE BOOL "" FORCE)
# 弱网/CI 环境可用 $ENV{RD_DEPS_MIRROR}/ktx 指向本地 KTX-Software 源码副本跳过下载
if(EXISTS "$ENV{RD_DEPS_MIRROR}/ktx/CMakeLists.txt")
  FetchContent_Declare(ktx SOURCE_DIR $ENV{RD_DEPS_MIRROR}/ktx)
else()
  FetchContent_Declare(ktx
    URL https://github.com/KhronosGroup/KTX-Software/archive/refs/tags/v4.3.2.tar.gz)
endif()
FetchContent_MakeAvailable(ktx)
# ktx CMakeLists 会强设 CMAKE_XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET=11.0
# (CACHE 全局,污染所有目标;std::filesystem 需 13+)——iOS 下覆盖回 16.0
if(IOS)
  set(CMAKE_XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "16.0" CACHE STRING "" FORCE)
endif()
# astcenc 自带 -Werror 且 AppleClang 下同目标的 -ffp-model/-ffp-contract 冲突会致命,
# 关掉该告警名(目标名随 ISA 变,逐个存在性检查)
foreach(astcenc_tgt astcenc-neon-static astcenc-avx2-static astcenc-sse4.1-static
                    astcenc-sse2-static astcenc-none-static astcenc-static)
  if(TARGET ${astcenc_tgt})
    target_compile_options(${astcenc_tgt} PRIVATE -Wno-overriding-option)
  endif()
endforeach()

if(ANDROID)
  FetchContent_Declare(VulkanHeaders
    URL https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/vulkan-sdk-1.3.296.0.tar.gz)
  FetchContent_MakeAvailable(VulkanHeaders)
endif()

if(NOT ANDROID AND NOT IOS)
  # nlohmann/json:仅 glb_ktx2 工具用(单头;header-only)
  FetchContent_Declare(nlohmann_json
    URL https://github.com/nlohmann/json/archive/refs/tags/v3.11.3.tar.gz)
  FetchContent_MakeAvailable(nlohmann_json)

  FetchContent_Declare(googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz)
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  # glslang 裁剪：关掉安装/优化器（spirv-opt 依赖 SPIRV-Tools）/JS/HLSL 等无用组件
  set(SKIP_GLSLANG_INSTALL ON CACHE BOOL "" FORCE)
  set(ENABLE_OPT OFF CACHE BOOL "" FORCE) # 避免依赖 SPIRV-Tools
  set(ENABLE_SPVREMAPPER OFF CACHE BOOL "" FORCE)
  set(ENABLE_GLSLANG_JS OFF CACHE BOOL "" FORCE)
  set(ENABLE_HLSL OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(glslang
    URL https://github.com/KhronosGroup/glslang/archive/refs/tags/14.3.0.tar.gz)
  FetchContent_MakeAvailable(glslang)

  # spirv-cross 裁剪：只要 CLI + GLSL/MSL 后端 + 反射（CPP/UTIL 是 CLI 的依赖）
  set(SPIRV_CROSS_CLI ON CACHE BOOL "" FORCE)
  set(SPIRV_CROSS_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
  set(SPIRV_CROSS_ENABLE_GLSL ON CACHE BOOL "" FORCE)
  set(SPIRV_CROSS_ENABLE_MSL ON CACHE BOOL "" FORCE)
  set(SPIRV_CROSS_ENABLE_CPP ON CACHE BOOL "" FORCE) # CLI 依赖
  set(SPIRV_CROSS_ENABLE_REFLECT ON CACHE BOOL "" FORCE)
  set(SPIRV_CROSS_ENABLE_C_API OFF CACHE BOOL "" FORCE)
  set(SPIRV_CROSS_ENABLE_UTIL ON CACHE BOOL "" FORCE) # CLI 依赖
  FetchContent_Declare(spirv-cross
    URL https://github.com/KhronosGroup/SPIRV-Cross/archive/refs/tags/vulkan-sdk-1.3.296.0.tar.gz)
  FetchContent_MakeAvailable(spirv-cross)

  # GLFW:render_test --interactive 的窗口/事件(host only,NO_API 模式;镜像旁路同 ktx)
  set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  if(EXISTS "$ENV{RD_DEPS_MIRROR}/glfw/CMakeLists.txt")
    FetchContent_Declare(glfw SOURCE_DIR $ENV{RD_DEPS_MIRROR}/glfw)
  else()
    FetchContent_Declare(glfw
      URL https://github.com/glfw/glfw/archive/refs/tags/3.4.tar.gz)
  endif()
  FetchContent_MakeAvailable(glfw)

  FetchContent_Declare(VulkanHeaders
    URL https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/vulkan-sdk-1.3.296.0.tar.gz)
  FetchContent_MakeAvailable(VulkanHeaders)
endif()



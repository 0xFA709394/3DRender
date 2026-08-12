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

if(ANDROID)
  FetchContent_Declare(VulkanHeaders
    URL https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/vulkan-sdk-1.3.296.0.tar.gz)
  FetchContent_MakeAvailable(VulkanHeaders)
endif()

if(NOT ANDROID AND NOT IOS)
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

  FetchContent_Declare(stb
    URL https://github.com/nothings/stb/archive/refs/heads/master.tar.gz)
  FetchContent_MakeAvailable(stb)

  FetchContent_Declare(VulkanHeaders
    URL https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/vulkan-sdk-1.3.296.0.tar.gz)
  FetchContent_MakeAvailable(VulkanHeaders)
endif()



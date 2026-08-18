# ============================================================================
# 根构建脚本：移动端 3D 渲染器框架（C++17 跨平台内核 + Vulkan/Metal/GLES 三后端）
#
# 目标结构：
#   core/                 → rd_core 静态库（内核 + 按平台编译的后端）
#   platform/android/     → rd_jni 共享库（仅 ANDROID）
#   samples/ios/          → RdDemo app（仅 IOS toolchain）
#   shaders/ tests/ tools/→ 仅 host 构建（shader 离线产物 + 单元测试 + 手动工具）
#
# 常用配置：
#   host:        cmake -S . -B build
#   Android:     由 samples/android 的 Gradle externalNativeBuild 驱动
#   iOS 模拟器:  cmake -S . -B build-ios -G Xcode \
#                  -DCMAKE_TOOLCHAIN_FILE=cmake/ios.toolchain.cmake \
#                  -DRD_IOS_SDK=iphonesimulator
# ============================================================================
cmake_minimum_required(VERSION 3.22) # Android SDK 自带 cmake 为 3.22.1
project(rdrenderer LANGUAGES C CXX)

if(APPLE)
  enable_language(OBJC)   # GLFW 的 Cocoa 源是纯 Objective-C（.m）
  enable_language(OBJCXX) # Metal 后端是 Objective-C++（.mm）
endif()

# 全局 C++17（无编译器扩展）
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

include(cmake/Deps.cmake)
include(cmake/ShaderCompile.cmake) # 提供 RD_SHADER_OUT 与 rd_compile_shader

# 移动端交叉构建：shader 产物来自 host 构建目录（交叉编译期跑不了 glslang/spirv-cross），
# 内嵌 embedded_shaders.cpp 在 configure 期生成（见 cmake/EmbedShaders.cmake）
if(ANDROID OR IOS)
  include(cmake/EmbedShaders.cmake)
  if(NOT DEFINED RD_HOST_SHADER_OUT)
    set(RD_HOST_SHADER_OUT ${CMAKE_SOURCE_DIR}/build/shaders_out)
  endif()
endif()

enable_testing()
add_subdirectory(core)
if(ANDROID)
  add_subdirectory(platform/android)
elseif(IOS)
  add_subdirectory(samples/ios)
else()
  add_subdirectory(shaders)
  add_subdirectory(tests)
  add_subdirectory(tools/render_test)
  add_subdirectory(tools/img_check)
  add_subdirectory(tools/perf_test)
endif()

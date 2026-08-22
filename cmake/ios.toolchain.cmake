# ============================================================================
# iOS 交叉编译 toolchain（-DCMAKE_TOOLCHAIN_FILE=cmake/ios.toolchain.cmake）
#
# 用法（模拟器）：cmake -S . -B build-ios -G Xcode \
#   -DCMAKE_TOOLCHAIN_FILE=cmake/ios.toolchain.cmake -DRD_IOS_SDK=iphonesimulator
#
# RD_IOS_SDK：iphoneos（真机，默认）| iphonesimulator（模拟器）。
# 真机与模拟器的 metallib 指令集不同，core/CMakeLists.txt 会据
# CMAKE_OSX_SYSROOT 是否含 "Simulator" 选择 RD_EMBED_IOS_METAL /
# RD_EMBED_IOS_SIMULATOR，内嵌对应的 shader 产物。
# ============================================================================
set(CMAKE_SYSTEM_NAME iOS)
# 最低部署版本 iOS 16(std::filesystem 需 13+;须 CACHE FORCE——project() 的平台
# 初始化会用 SDK 默认值回填非 cache 变量,toolchain 普通 set 会被覆盖)
set(CMAKE_OSX_DEPLOYMENT_TARGET "16.0" CACHE STRING "iOS 最低部署版本" FORCE)
if(NOT DEFINED RD_IOS_SDK)
  set(RD_IOS_SDK iphoneos) # 默认真机；模拟器须在配置时显式传 -DRD_IOS_SDK=iphonesimulator
endif()
set(CMAKE_OSX_SYSROOT ${RD_IOS_SDK})
set(CMAKE_OSX_ARCHITECTURES arm64) # 仅 arm64（真机与 Apple Silicon 模拟器）

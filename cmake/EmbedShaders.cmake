# ============================================================================
# rd_embed_shaders_configure(<输出cpp> <spv/gles目录> <ios metallib目录>)
#
# 移动端（ANDROID/IOS）交叉构建专用的 embedded_shaders.cpp 生成入口：
# 交叉编译期无法运行 host 侧的 glslang/spirv-cross，shader 产物全部取自
# host 构建目录（RD_HOST_SHADER_OUT，见根 CMakeLists.txt），故在 configure 期
# 直接以 cmake -P 调 GenEmbedded.cmake 把产物十六进制内嵌成 cpp
# （host 构建则是 build 期由 add_custom_command 生成，见 core/CMakeLists.txt）。
#
# iOS 模拟器 metallib 目录按约定取 <spv/gles目录>_iossim（与 ShaderCompile.cmake
# 的输出组织一致）；生成失败直接 FATAL_ERROR 终止配置，避免静默编出无 shader 的库。
# ============================================================================
function(rd_embed_shaders_configure OUT DIR DIR_IOS)
  set(DIR_IOSSIM "${DIR}_iossim") # 模拟器 metallib 目录约定：与 host 产物目录平级
  execute_process(
    COMMAND ${CMAKE_COMMAND} -DOUT=${OUT} -DDIR=${DIR} -DDIR_IOS=${DIR_IOS}
            -DDIR_IOSSIM=${DIR_IOSSIM}
            -P ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/GenEmbedded.cmake
    RESULT_VARIABLE res)
  if(NOT res EQUAL 0)
    message(FATAL_ERROR "embedded shader 生成失败")
  endif()
endfunction()

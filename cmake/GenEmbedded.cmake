# ============================================================================
# embedded_shaders.cpp 生成脚本（cmake -P 模式运行，被两条路径复用）：
#   用法: cmake -DOUT=<输出cpp> -DDIR=<shaders_out> -DDIR_IOS=<shaders_out_ios>
#               [-DDIR_IOSSIM=<shaders_out_iossim>] -P GenEmbedded.cmake
#
# 把 cube shader 的各后端产物以十六进制数组内嵌进 C++ 源码；
# 不存在的产物生成占位空数组（sizeof==1），embeddedCubeShader 据此返回 false。
# 生成文件的接口见 core/api/embedded_shaders.h。
# ============================================================================
function(embed_file VAR_NAME FILE_PATH OUT_LINES)
  if(EXISTS ${FILE_PATH})
    file(READ ${FILE_PATH} hex HEX)
    string(REGEX MATCHALL ".." bytes "${hex}")
    set(body "")
    foreach(b ${bytes})
      string(APPEND body "0x${b},")
    endforeach()
    set(${OUT_LINES} "static const uint8_t ${VAR_NAME}[] = {${body}};\n" PARENT_SCOPE)
  else()
    set(${OUT_LINES} "static const uint8_t ${VAR_NAME}[] = {0};\n" PARENT_SCOPE)
  endif()
endfunction()

embed_file(kCubeVertSpv ${DIR}/cube.vert.spv L1)
embed_file(kCubeFragSpv ${DIR}/cube.frag.spv L2)
embed_file(kCubeVertGles ${DIR}/cube.vert.gles L3)
embed_file(kCubeFragGles ${DIR}/cube.frag.gles L4)
embed_file(kCubeVertMetal ${DIR}/cube.vert.metallib L5)
embed_file(kCubeFragMetal ${DIR}/cube.frag.metallib L6)
embed_file(kCubeVertMetalIos ${DIR_IOS}/cube.vert.metallib L7)
embed_file(kCubeFragMetalIos ${DIR_IOS}/cube.frag.metallib L8)
embed_file(kCubeVertMetalIosSim ${DIR_IOSSIM}/cube.vert.metallib L9)
embed_file(kCubeFragMetalIosSim ${DIR_IOSSIM}/cube.frag.metallib L10)

# 生成查询函数：按后端+阶段返回字节区间；Metal 的三份 metallib 由编译宏选择
# （RD_EMBED_IOS_METAL=真机，RD_EMBED_IOS_SIMULATOR=模拟器，否则=macOS host）。
file(WRITE ${OUT} "// GENERATED FILE - 勿手改
#include \"api/embedded_shaders.h\"
namespace {
${L1}${L2}${L3}${L4}${L5}${L6}${L7}${L8}${L9}${L10}
}
namespace rd {
bool embeddedCubeShader(Backend backend, ShaderStage stage, const uint8_t** data, size_t* size) {
  const uint8_t* d = nullptr; size_t n = 0;
  const bool vs = (stage == ShaderStage::Vertex);
  switch (backend) {
    case Backend::Vulkan:
      if (vs) { d = kCubeVertSpv; n = sizeof(kCubeVertSpv); } else { d = kCubeFragSpv; n = sizeof(kCubeFragSpv); }
      break;
    case Backend::GLES:
      if (vs) { d = kCubeVertGles; n = sizeof(kCubeVertGles); } else { d = kCubeFragGles; n = sizeof(kCubeFragGles); }
      break;
    case Backend::Metal:
#if defined(RD_EMBED_IOS_SIMULATOR)
      if (vs) { d = kCubeVertMetalIosSim; n = sizeof(kCubeVertMetalIosSim); } else { d = kCubeFragMetalIosSim; n = sizeof(kCubeFragMetalIosSim); }
#elif defined(RD_EMBED_IOS_METAL)
      if (vs) { d = kCubeVertMetalIos; n = sizeof(kCubeVertMetalIos); } else { d = kCubeFragMetalIos; n = sizeof(kCubeFragMetalIos); }
#else
      if (vs) { d = kCubeVertMetal; n = sizeof(kCubeVertMetal); } else { d = kCubeFragMetal; n = sizeof(kCubeFragMetal); }
#endif
      break;
  }
  if (!d || n <= 1) return false; // 占位空数组 sizeof==1
  *data = d; *size = n; return true;
}
} // namespace rd
")

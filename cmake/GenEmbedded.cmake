# ============================================================================
# embedded_shaders.cpp 生成脚本（cmake -P 模式运行，被两条路径复用）：
#   用法: cmake -DOUT=<输出cpp> -DDIR=<shaders_out> -DDIR_IOS=<shaders_out_ios>
#               [-DDIR_IOSSIM=<shaders_out_iossim>] -P GenEmbedded.cmake
#
# 把名表内 shader 的各后端产物以十六进制数组内嵌进 C++ 源码；
# 不存在的产物生成占位空数组（sizeof==1），embeddedShader 据此返回 false。
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

set(SHADERS cube unlit pbr_forward prefilter blit shadow_depth bloom_extract bloom_blur composite fxaa pbr_forward_skinned shadow_depth_skinned equirect_to_cube)
set(ALL_LINES "")
foreach(S ${SHADERS})
  embed_file(k_${S}_vert_spv     ${DIR}/${S}.vert.spv            L)
  string(APPEND ALL_LINES "${L}")
  embed_file(k_${S}_frag_spv     ${DIR}/${S}.frag.spv            L)
  string(APPEND ALL_LINES "${L}")
  embed_file(k_${S}_vert_gles    ${DIR}/${S}.vert.gles           L)
  string(APPEND ALL_LINES "${L}")
  embed_file(k_${S}_frag_gles    ${DIR}/${S}.frag.gles           L)
  string(APPEND ALL_LINES "${L}")
  embed_file(k_${S}_vert_metal   ${DIR}/${S}.vert.metallib       L)
  string(APPEND ALL_LINES "${L}")
  embed_file(k_${S}_frag_metal   ${DIR}/${S}.frag.metallib       L)
  string(APPEND ALL_LINES "${L}")
  embed_file(k_${S}_vert_metali  ${DIR_IOS}/${S}.vert.metallib   L)
  string(APPEND ALL_LINES "${L}")
  embed_file(k_${S}_frag_metali  ${DIR_IOS}/${S}.frag.metallib   L)
  string(APPEND ALL_LINES "${L}")
  embed_file(k_${S}_vert_metals  ${DIR_IOSSIM}/${S}.vert.metallib L)
  string(APPEND ALL_LINES "${L}")
  embed_file(k_${S}_frag_metals  ${DIR_IOSSIM}/${S}.frag.metallib L)
  string(APPEND ALL_LINES "${L}")
endforeach()

# 查询函数:名 → 各后端数组;Metal 的三份 metallib 由编译宏选择
# （RD_EMBED_IOS_METAL=真机，RD_EMBED_IOS_SIMULATOR=模拟器，否则=macOS host）。
set(LOOKUP "")
foreach(S ${SHADERS})
  string(APPEND LOOKUP "
  if (strcmp(name, \"${S}\") == 0) {
    switch (backend) {
      case Backend::Vulkan:
        if (vs) { d = k_${S}_vert_spv; n = sizeof(k_${S}_vert_spv); }
        else    { d = k_${S}_frag_spv; n = sizeof(k_${S}_frag_spv); }
        break;
      case Backend::GLES:
        if (vs) { d = k_${S}_vert_gles; n = sizeof(k_${S}_vert_gles); }
        else    { d = k_${S}_frag_gles; n = sizeof(k_${S}_frag_gles); }
        break;
      case Backend::Metal:
#if defined(RD_EMBED_IOS_SIMULATOR)
        if (vs) { d = k_${S}_vert_metals; n = sizeof(k_${S}_vert_metals); }
        else    { d = k_${S}_frag_metals; n = sizeof(k_${S}_frag_metals); }
#elif defined(RD_EMBED_IOS_METAL)
        if (vs) { d = k_${S}_vert_metali; n = sizeof(k_${S}_vert_metali); }
        else    { d = k_${S}_frag_metali; n = sizeof(k_${S}_frag_metali); }
#else
        if (vs) { d = k_${S}_vert_metal; n = sizeof(k_${S}_vert_metal); }
        else    { d = k_${S}_frag_metal; n = sizeof(k_${S}_frag_metal); }
#endif
        break;
    }
  }
")
endforeach()

file(WRITE ${OUT} "// GENERATED FILE - 勿手改
#include \"api/embedded_shaders.h\"
#include <cstring>
namespace {
${ALL_LINES}
}
namespace rd {
bool embeddedShader(Backend backend, const char* name, ShaderStage stage,
                    const uint8_t** data, size_t* size) {
  const uint8_t* d = nullptr; size_t n = 0;
  const bool vs = (stage == ShaderStage::Vertex);
${LOOKUP}
  if (!d || n <= 1) return false;  // 占位空数组 sizeof==1
  *data = d; *size = n; return true;
}
bool embeddedCubeShader(Backend backend, ShaderStage stage, const uint8_t** data,
                        size_t* size) {
  return embeddedShader(backend, \"cube\", stage, data, size);
}
} // namespace rd
")

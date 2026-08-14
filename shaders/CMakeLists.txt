# shaders 离线编译清单：rd_compile_shader 在 cmake/ShaderCompile.cmake 定义，
# 对每个 GLSL 源产出三后端产物（.spv / .metallib / .gles）到 RD_SHADER_OUT 目录。
rd_compile_shader(cube.vert)
rd_compile_shader(cube.frag)
rd_compile_shader(texquad.vert)
rd_compile_shader(texquad.frag)
rd_compile_shader(blend.vert)
rd_compile_shader(blend.frag)

# 生成路径头，供测试定位 shader 产物
file(WRITE ${CMAKE_BINARY_DIR}/generated/rd_shader_dir.h
  "#pragma once\n#define RD_SHADER_DIR \"${RD_SHADER_OUT}\"\n")

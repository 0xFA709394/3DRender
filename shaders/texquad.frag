// texquad.frag：纹理四边形测试的片段着色器——采样纹理直接输出。
// 绑定约定：texture slot 0 ↔ set0 binding 4（combined-image-sampler）；
// 经 spirv-cross 转 MSL 后映射为 texture/sampler(4)，GLES 为纹理单元 0
// （完整约定见 rhi_types.h 底部注释块）。
#version 450
layout(location = 0) in vec2 vUV;
layout(binding = 4) uniform sampler2D tex0; // texture slot 0 → binding 4
layout(location = 0) out vec4 outColor;
void main() {
  outColor = texture(tex0, vUV);
}

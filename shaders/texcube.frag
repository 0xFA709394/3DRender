// texcube.frag：按 uniform 方向 + lod 采样 cubemap 输出。
// UBO binding0 ↔ uniform slot 0;samplerCube binding4 ↔ texture slot 0
// （GLES 侧 sampler uniform 名 tex0，完整约定见 rhi_types.h 底部注释块）。
#version 450
layout(binding = 0) uniform UBO { vec3 dir; float lod; };
layout(binding = 4) uniform samplerCube tex0;
layout(location = 0) out vec4 outColor;
void main() { outColor = textureLod(tex0, dir, lod); }

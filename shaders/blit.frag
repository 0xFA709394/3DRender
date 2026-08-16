// blit.frag:采样 sceneColor(slot 0)直接输出(upscale pass;P2 后处理链挂载点)。
// 绑定约定:texture slot 0 ↔ set0 binding 4 ↔ Metal texture/sampler(4) ↔ GLES 单元 0。
#version 450
layout(location = 0) in vec2 vUV;
layout(binding = 4) uniform sampler2D tex0;  // texture slot 0 → binding 4
layout(location = 0) out vec4 outColor;
void main() { outColor = texture(tex0, vUV); }

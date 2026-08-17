// hdr_view.frag:直读纹理 ×0.5 输出(R16F roundtrip 契约测试用)。
#version 450
layout(location = 0) in vec2 vUV;
layout(binding = 4) uniform sampler2D tex0;
layout(location = 0) out vec4 outColor;
void main() { outColor = vec4(texture(tex0, vUV).rgb * 0.5, 1.0); }

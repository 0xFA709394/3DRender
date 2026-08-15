// unlit.frag：baseColor 纹理直出。texture slot 0 ↔ set0 binding4 / texN(GLES)。
#version 450
layout(location = 0) in vec2 vUV;
layout(binding = 4) uniform sampler2D tex0;
layout(location = 0) out vec4 outColor;
void main() { outColor = texture(tex0, vUV); }

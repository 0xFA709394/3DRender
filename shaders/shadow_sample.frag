// shadow_sample.frag:sampler2DShadow 比较采样,输出灰度(0=阴影,1=受光)。
#version 450
layout(location = 0) in vec2 vUV;
layout(binding = 4) uniform sampler2DShadow tex0;  // texture slot 0 → binding 4
layout(location = 0) out vec4 outColor;
void main() { outColor = vec4(vec3(texture(tex0, vec3(vUV, 0.5))), 1.0); }

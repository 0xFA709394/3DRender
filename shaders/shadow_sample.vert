// shadow_sample.vert:全屏三角形顶点直通(契约测试用;pos3@0|uv2@12 stride 20)。
#version 450
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 0) out vec2 vUV;
void main() { vUV = aUV; gl_Position = vec4(aPos, 1.0); }

// depth.frag：深度遮挡测试片段着色器，输出插值顶点色。
#version 450
layout(location = 0) in vec3 vColor;
layout(location = 0) out vec4 outColor;
void main() { outColor = vec4(vColor, 1.0); }

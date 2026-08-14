// inst.frag：实例化测试片段着色器，输出常量红。
#version 450
layout(location = 0) out vec4 outColor;
void main() { outColor = vec4(1.0, 0.0, 0.0, 1.0); }

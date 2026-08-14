// blend.frag：输出常量半透明绿，用于 blend 契约测试。
#version 450
layout(location = 0) out vec4 outColor;
void main() { outColor = vec4(0.0, 1.0, 0.0, 0.5); }

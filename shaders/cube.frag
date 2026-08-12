// cube.frag：顶点色立方体的片段着色器——直接输出插值后的顶点色（不透明）。
#version 450
layout(location = 0) in vec3 vColor;
layout(location = 0) out vec4 outColor;
void main() {
  outColor = vec4(vColor, 1.0);
}

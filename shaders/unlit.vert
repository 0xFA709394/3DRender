// unlit.vert：glTF 静态模型 unlit 顶点着色器。
// 布局：location0=pos(vec3)@0，location1=normal(vec3)@12（2b PBR 用，本版未采样），
//       location2=uv(vec2)@24；交错 stride 32。UBO binding0=mvp。
#version 450
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(binding = 0) uniform UBO { mat4 mvp; };
layout(location = 0) out vec2 vUV;
void main() {
  gl_Position = mvp * vec4(aPos, 1.0);
  vUV = aUV;
}

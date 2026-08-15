// unlit.vert：glTF 静态模型 unlit 顶点着色器。
// 布局：location0=pos(vec3)@0，location1=normal(vec3)@12，
//       location2=tangent(vec4)@24（2b 布局;本 shader 未用），
//       location3=uv(vec2)@40；交错 stride 48。ItemUBO(binding1) 前 64B=mvp。
#version 450
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec4 aTangent;
layout(location = 3) in vec2 aUV;
layout(binding = 1) uniform ItemUBO { mat4 mvp; };  // ItemUBO 前 64B 即 mvp
layout(location = 0) out vec2 vUV;
void main() {
  gl_Position = mvp * vec4(aPos, 1.0);
  vUV = aUV;
}

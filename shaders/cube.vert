// cube.vert：顶点色立方体的顶点着色器。
// 输入：location0=位置(vec3)，location1=顶点色(vec3)——与 CubeScene 的顶点布局一致
//       （binding0 stride24，pos@0/color@12）。
// uniform：binding0 的 UBO.mvp（uniform slot 0 ↔ 各后端映射，见 rhi_types.h 绑定约定）。
// 坐标系：mvp 由 GLM_FORCE_DEPTH_ZERO_TO_ONE 的 glm 构造，输出 NDC z∈[0,1]；
//         Vulkan 侧经负高度视口翻转转正（见 vulkan_device.cpp）。
#version 450
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aColor;
layout(binding = 0) uniform UBO { mat4 mvp; };
layout(location = 0) out vec3 vColor;
void main() {
  gl_Position = mvp * vec4(aPos, 1.0);
  vColor = aColor;  // 顶点色插值传给片段阶段
}

// blit.vert:全屏三角形(无顶点缓冲,gl_VertexIndex 生成)。
// vFlip:GLES 渲染到纹理时 NDC+Y 落在内存末行(与 Metal/Vulkan 相反),
// 由 BlitUBO.params.x 翻转 v 吸收;Metal/Vulkan 传 0。
// 绑定约定:uniform slot 0 ↔ set0 binding 0(GLES 块名 BlitUBO→0)。
#version 450
layout(location = 0) out vec2 vUV;
layout(binding = 0) uniform BlitUBO { vec4 params; } u;  // x = vFlip(0/1)
void main() {
  vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);  // (0,0),(2,0),(0,2)
  vUV = vec2(p.x, u.params.x > 0.5 ? p.y : 1.0 - p.y);
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}

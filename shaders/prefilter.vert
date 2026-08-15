// prefilter.vert：全屏三角形，输出 NDC 供 frag 重建 cubemap 采样方向。
#version 450
layout(location = 0) out vec2 vNdc;
void main() {
  vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
  vNdc = p * 2.0 - 1.0;
  gl_Position = vec4(vNdc, 0.0, 1.0);
}

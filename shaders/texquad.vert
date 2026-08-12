// texquad.vert：纹理四边形测试的顶点着色器。
// 顶点已在裁剪空间（全屏三角带，pos 范围 ±1），直接透传 gl_Position；
// UV 可超出 [0,1]（mip 测试用跨度 32 强制缩小采样）。
// 输入布局：location0=pos(vec3)@0，location1=uv(vec2)@12，binding0 stride20。
#version 450
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 0) out vec2 vUV;
void main() {
  vUV = aUV;
  gl_Position = vec4(aPos, 1.0);
}

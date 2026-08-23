// skybox.vert:全屏三角形(gl_VertexIndex 生位置)+ 每顶点视线方向(CPU 算,含 yaw)。
// z=1 远平面(NDC [0,1]),配合 LessEqual 深度测试贴远平面。
#version 450
layout(location = 0) in vec3 aDir;
layout(location = 0) out vec3 vDir;
void main() {
  vDir = aDir;
  vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2) * 2.0 - 1.0;
  gl_Position = vec4(p, 1.0, 1.0);
}

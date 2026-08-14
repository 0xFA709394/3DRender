// inst.vert：实例化测试——小三角形按 per-instance 偏移平铺。
// location0=pos(vec2，binding0 每顶点)；location1=offset(vec2，binding1 每实例)。
#version 450
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aOffset;
void main() { gl_Position = vec4(aPos + aOffset, 0.0, 1.0); }

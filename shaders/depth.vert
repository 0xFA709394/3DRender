// depth.vert：深度遮挡测试——pos 直接作为裁剪空间坐标（z 即 NDC 深度）。
// location0=pos(vec3)；location1=color(vec3) → vColor。
#version 450
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aColor;
layout(location = 0) out vec3 vColor;
void main() { gl_Position = vec4(aPos, 1.0); vColor = aColor; }

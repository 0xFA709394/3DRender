// water_step.frag:波动方程显式有限差分步进(全屏 pass,RGBA16F ping-pong)。
// R=当前高度 u,G=上一帧 u_prev → 输出 (u_new, u) 写入另一张。
// u_new = 2u - u_prev + k·(四邻均值-u) - damp·(u-u_prev) + 注入脉冲。
// 边界=clamp 寻址(池壁全反射);CFL: k≤0.5(常量 0.42)。
// 槽位:texWave=slot1(binding5);WaterStepUBO=slot0。
#version 450
layout(location = 0) in vec2 vUV;
layout(binding = 0) uniform WaterStepUBO {
  vec4 inject[8];  // xy=uv z=强度(世界高度) w=半径(texel)
  vec4 params;     // x=count y=texel z=damping w=k
} ws;
layout(binding = 5) uniform sampler2D texWave;
layout(location = 0) out vec4 outColor;
void main() {
  vec2 c = texture(texWave, vUV).rg;
  float n = textureOffset(texWave, vUV, ivec2(0, 1)).r;
  float s = textureOffset(texWave, vUV, ivec2(0, -1)).r;
  float e = textureOffset(texWave, vUV, ivec2(1, 0)).r;
  float w = textureOffset(texWave, vUV, ivec2(-1, 0)).r;
  float lap = (n + s + e + w) * 0.25 - c.r;
  float u = 2.0 * c.r - c.g + ws.params.w * lap;
  u -= (c.r - c.g) * ws.params.z;  // 阻尼(速度比例)
  for (int i = 0; i < 8; ++i) {
    if (float(i) >= ws.params.x) break;
    vec2 d = (vUV - ws.inject[i].xy) / max(ws.inject[i].w * ws.params.y, 1e-6);
    u += ws.inject[i].z * exp(-dot(d, d));  // 高斯脉冲
  }
  outColor = vec4(u, c.r, 0.0, 1.0);
}

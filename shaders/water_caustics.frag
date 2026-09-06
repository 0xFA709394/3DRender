// water_caustics.frag:Jacobian 汇聚近似焦散(全屏 pass,与波场同分辨率)。
// 逐 texel:高度梯度→法线→方向光折射→落点 p(uv);邻差分 2×2 Jacobian;
// 强度=clamp(1/|det J|)(汇聚亮/发散暗)。视差近似(焦散图按水面 UV 索引)。
// 槽位:texWave=slot1(binding5);WaterCausticsUBO=slot0。
#version 450
layout(location = 0) in vec2 vUV;
layout(binding = 0) uniform WaterCausticsUBO {
  vec4 c0;  // x=texel y=eta(1/1.33) z=depth(焦散衰减) w=unused
  vec4 c1;  // x=worldPerTexelX y=worldPerTexelZ z=unused w=unused
  vec4 c2;  // xyz=lightDir(指向光源) w=unused
} wc;
layout(binding = 5) uniform sampler2D texWave;
layout(location = 0) out vec4 outColor;
// 折射后水平落点(uv 域;视差近似:uv + worldOffset/poolSize)
vec2 floorPos(vec2 uv) {
  float t = wc.c0.x;
  float hx = texture(texWave, uv + vec2(t, 0.0)).r - texture(texWave, uv - vec2(t, 0.0)).r;
  float hz = texture(texWave, uv + vec2(0.0, t)).r - texture(texWave, uv - vec2(0.0, t)).r;
  vec3 nrm = normalize(vec3(-hx * wc.c1.x, 2.0, -hz * wc.c1.y));  // 世界尺度梯度
  vec3 d = -normalize(wc.c2.xyz);                                  // 入射(指向下)
  vec3 refr = refract(d, nrm, wc.c0.y);
  if (dot(refr, refr) < 1e-6) refr = d;
  return uv + refr.xz * wc.c0.z / vec2(wc.c1.x, wc.c1.y) / max(wc.c0.x, 1e-6);
}
void main() {
  float t = wc.c0.x;
  vec2 p = floorPos(vUV);
  vec2 px = floorPos(vUV + vec2(t, 0.0));
  vec2 py = floorPos(vUV + vec2(0.0, t));
  vec2 jx = (px - p);  // 已含 /t·t 抵消(同尺度差分)
  vec2 jy = (py - p);
  float det = jx.x * jy.y - jx.y * jy.x;
  float detUv = det / max(t * t, 1e-8);  // uv 域行列式(平态=1)
  float i = clamp(1.0 / max(abs(detUv), 0.05), 0.0, 6.0);
  outColor = vec4(i, 0.0, 0.0, 1.0);
}

// water_surface.vert:水面网格顶点位移(顶点纹理 fetch×4)+ 三点差分法线。
// 世界 xz→池 uv;高度覆盖 Y(planeY+波高);mvp 不适用位移后位置 → 用 FrameUBO.viewProj。
// 槽位:texWave=slot1(binding5,顶点阶段;三后端 vert 采样已支持);
// FrameUBO=slot0(含 water[3] 尾部);ItemUBO=slot1(块声明取前 192B 对齐)。
#version 450
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec4 aTangent;
layout(location = 3) in vec2 aUV;
layout(binding = 0) uniform FrameUBO {
  mat4 viewProj;
  vec4 cameraPos;
  vec4 lightDir;
  vec4 lightColor;
  vec4 sh[9];
  vec4 transmissionParams;
  vec4 water[3];  // 0=(sizeX,sizeZ,planeY,waveScale) 1=(depth,causticsI,on,simSize) 2=(texel,0,0,0)
};
layout(binding = 1) uniform ItemUBO {
  mat4 mvp;
  mat4 world;
  mat4 normalMatrix;
  vec4 baseColorFactor;
  vec4 emissiveOcclusion;
  vec4 metallicRoughness;
  vec4 uvTransform;
  vec4 ext0;
  vec4 ext1;
  vec4 ext2;
};
layout(binding = 5) uniform sampler2D texWave;
layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec2 vUV;
void main() {
  vec3 wp = (world * vec4(aPos, 1.0)).xyz;
  vec2 wuv = wp.xz / water[0].xy + 0.5;
  float t = water[2].x * 3.0;  // 着色法线平滑:3 texel 跨度低通(位移仍用原高度)
  float h0 = texture(texWave, wuv).r;
  float hx = texture(texWave, wuv + vec2(t, 0.0)).r - texture(texWave, wuv - vec2(t, 0.0)).r;
  float hz = texture(texWave, wuv + vec2(0.0, t)).r - texture(texWave, wuv - vec2(0.0, t)).r;
  float scale = water[0].w;
  wp.y = water[0].z + h0 * scale;
  // 世界导数:Δh·scale / (2·texel·worldPerTexel)
  float dx = hx * scale / max(2.0 * t * water[0].x, 1e-6);
  float dz = hz * scale / max(2.0 * t * water[0].y, 1e-6);
  vNormal = normalize(vec3(-dx, 1.0, -dz));
  vWorldPos = wp;
  vUV = wuv;
  gl_Position = viewProj * vec4(wp, 1.0);
}

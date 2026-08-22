// composite.frag:scene + 3 级 bloom 加权 → ACES(Narkowicz)→ gamma。
#version 450
layout(location = 0) in vec2 vUV;
layout(binding = 4) uniform sampler2D tex0;  // slot0=scene(HDR)
layout(binding = 5) uniform sampler2D tex1;  // slot1=bloom l1
layout(binding = 6) uniform sampler2D tex2;  // slot2=bloom l2
layout(binding = 7) uniform sampler2D tex3;  // slot3=bloom l3
layout(binding = 0) uniform BlitUBO { vec4 params; } u;  // x=vFlip, w=exposure
layout(location = 0) out vec4 outColor;
vec3 aces(vec3 x) {
  const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
  return clamp(x * (a * x + b) / (x * (c * x + d) + e), 0.0, 1.0);
}
void main() {
  vec3 c = texture(tex0, vUV).rgb;
  c += texture(tex1, vUV).rgb * 1.0 + texture(tex2, vUV).rgb * 0.6 +
       texture(tex3, vUV).rgb * 0.4;
  c *= u.params.w > 0.0 ? u.params.w : 1.0;  // 曝光
  c = aces(c);
  outColor = vec4(pow(c, vec3(1.0 / 2.2)), 1.0);
}

// bloom_extract.frag:阈值提取(>1.0 的 HDR 部分),半分辨率输出。
#version 450
layout(location = 0) in vec2 vUV;
layout(binding = 4) uniform sampler2D tex0;  // slot0=scene(HDR)
layout(location = 0) out vec4 outColor;
const float THRESHOLD = 1.0;
void main() {
  vec3 c = texture(tex0, vUV).rgb;
  float luma = dot(c, vec3(0.2126, 0.7152, 0.0722));
  float w = max(luma - THRESHOLD, 0.0) / max(luma, 1e-4);
  outColor = vec4(c * w, 1.0);
}

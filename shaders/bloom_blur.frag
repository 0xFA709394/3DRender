// bloom_blur.frag:9-tap tent 降采样(单 pass)。
// BlitUBO.params: x=vFlip, y=srcTexelW, z=srcTexelH
#version 450
layout(location = 0) in vec2 vUV;
layout(binding = 4) uniform sampler2D tex0;
layout(binding = 0) uniform BlitUBO { vec4 params; } u;
layout(location = 0) out vec4 outColor;
void main() {
  vec2 tx = vec2(u.params.y, 0.0);
  vec2 ty = vec2(0.0, u.params.z);
  vec3 c = texture(tex0, vUV).rgb * 4.0;
  c += texture(tex0, vUV + tx + ty).rgb * 2.0;
  c += texture(tex0, vUV - tx + ty).rgb * 2.0;
  c += texture(tex0, vUV + tx - ty).rgb * 2.0;
  c += texture(tex0, vUV - tx - ty).rgb * 2.0;
  c += texture(tex0, vUV + tx * 2.0).rgb;
  c += texture(tex0, vUV - tx * 2.0).rgb;
  c += texture(tex0, vUV + ty * 2.0).rgb;
  c += texture(tex0, vUV - ty * 2.0).rgb;
  outColor = vec4(c / 16.0, 1.0);
}

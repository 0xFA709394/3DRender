// fxaa.frag:FXAA 浓缩版(luma 边检测 + 方向模糊)。
// BlitUBO.params: x=vFlip, y=texelW, z=texelH
#version 450
layout(location = 0) in vec2 vUV;
layout(binding = 4) uniform sampler2D tex0;
layout(binding = 0) uniform BlitUBO { vec4 params; } u;
layout(location = 0) out vec4 outColor;
float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }
void main() {
  vec2 tx = vec2(u.params.y, 0.0);
  vec2 ty = vec2(0.0, u.params.z);
  vec3 rgbNW = texture(tex0, vUV - tx - ty).rgb;
  vec3 rgbNE = texture(tex0, vUV + tx - ty).rgb;
  vec3 rgbSW = texture(tex0, vUV - tx + ty).rgb;
  vec3 rgbSE = texture(tex0, vUV + tx + ty).rgb;
  vec3 rgbM = texture(tex0, vUV).rgb;
  float lNW = luma(rgbNW), lNE = luma(rgbNE), lSW = luma(rgbSW), lSE = luma(rgbSE),
        lM = luma(rgbM);
  float lMin = min(lM, min(min(lNW, lNE), min(lSW, lSE)));
  float lMax = max(lM, max(max(lNW, lNE), max(lSW, lSE)));
  if (lMax - lMin < 0.0312) { outColor = vec4(rgbM, 1.0); return; }
  vec2 dir = vec2(-((lNW + lNE) - (lSW + lSE)), ((lNW + lSW) - (lNE + lSE)));
  float dirReduce = max((lNW + lNE + lSW + lSE) * 0.03125, 0.0078125);
  float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);
  dir = clamp(dir * rcpDirMin, vec2(-8.0), vec2(8.0)) * u.params.yz;
  vec3 rgbA = 0.5 * (texture(tex0, vUV + dir * (1.0 / 3.0 - 0.5)).rgb +
                     texture(tex0, vUV + dir * (2.0 / 3.0 - 0.5)).rgb);
  vec3 rgbB = rgbA * 0.5 + 0.25 * (texture(tex0, vUV + dir * -0.5).rgb +
                                   texture(tex0, vUV + dir * 0.5).rgb);
  float lB = luma(rgbB);
  outColor = vec4(lB < lMin || lB > lMax ? rgbA : rgbB, 1.0);
}

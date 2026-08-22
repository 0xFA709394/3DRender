# HDR 环境 + 天空盒 实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 按 `docs/superpowers/specs/2026-08-23-hdr-env-skybox-design.md` 实现 .hdr 环境加载 + HDR IBL + 天空盒 + env.yaw。

**关键事实(代码侦察)**:
- Environment::build(environment.cpp:218)程序化路径:genEnvCubemap(CPU)→ envTex_ RGBA8 cube → projectToSH(env_.faces) → LUT → prefilterCube_ RGBA8 → kFaceBasis(211 行)逐面预滤波。
- pbr_forward.frag:slot5=prefilterCube samplerCube,slot6=brdfLut;LightUBO lightCount.y=hdrMode。
- 场景 pass:renderer.cpp ~682 `beginRenderPass(scene)` 后逐 item;天空盒插此处最前。
- `readbackTarget` 仅 RGBA8 → HDR 模式无磁盘缓存(spec 已修正)。
- stb 已在依赖(image_codec 用),`stbi_loadf`/`stbi_write_hdr` 可用。
- prefilter.vert 可复用(face basis UBO + 全屏三角形);新 equirect_to_cube.frag。

**通用约定:**
- 构建+测试:`export RD_DEPS_MIRROR=$HOME/.rd_deps_mirror && ./scripts/check.sh`
- 提交规范:每 Task 一个 commit
- golden 更新:`RD_UPDATE_GOLDENS=1 ctest --test-dir build -R <用例>` 后像素核对

---

### Task 1: hdr_env 加载 + 单测

**Files:**
- Create: `core/resource/hdr_env.{h,cpp}`
- Modify: `core/CMakeLists.txt`
- Test: `tests/resource/hdr_env_test.cpp`(新建)、`tests/CMakeLists.txt`(注册)

- [ ] **Step 1: 失败测试**

`tests/resource/hdr_env_test.cpp`:

```cpp
// HDR 环境加载单测:stbi_write_hdr 生成 → loadHdrEnv 解码(尺寸/浮点值域/错误路径)。
#include <gtest/gtest.h>
#include <cmath>
#include "resource/hdr_env.h"
// stb_image_write 实现宏在 image_codec.cpp;测试侧直接调库函数(链接 rd_core 已有 stb)
#include <stb_image_write.h>

TEST(HdrEnv, LoadWritten) {
  // 生成 8×4 渐变 hdr(值域 >1 验证 HDR)
  const int W = 8, H = 4;
  float px[W * H * 3];
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) {
      px[(y * W + x) * 3 + 0] = float(x) / W * 4.0f;   // 0..4
      px[(y * W + x) * 3 + 1] = float(y) / H * 2.0f;   // 0..2
      px[(y * W + x) * 3 + 2] = 1.5f;
    }
  const char* path = "/tmp/rd_hdr_test.hdr";
  ASSERT_EQ(stbi_write_hdr(path, W, H, 3, px), 1);
  rd::HdrEnv env;
  ASSERT_TRUE(rd::loadHdrEnv(path, env));
  EXPECT_EQ(env.width, uint32_t(W));
  EXPECT_EQ(env.height, uint32_t(H));
  ASSERT_EQ(env.pixels.size(), size_t(W) * H * 4);  // RGBA float
  // 浮点值域:>1 保留(HDR 区别于 LDR 的本质)
  float maxV = 0;
  for (float v : env.pixels) maxV = std::max(maxV, v);
  EXPECT_GT(maxV, 1.0f);
}

TEST(HdrEnv, BadPath) {
  rd::HdrEnv env;
  EXPECT_FALSE(rd::loadHdrEnv("/tmp/rd_no_such.hdr", env));
}
```

`tests/CMakeLists.txt` 追加 `resource/hdr_env_test.cpp`。

- [ ] **Step 2: 跑测试确认失败**(头文件不存在)

- [ ] **Step 3: 实现**

`core/resource/hdr_env.h`:

```cpp
/**
 * @file hdr_env.h
 * @brief .hdr(Radiance)equirect 环境图加载(stb_image float 解码)。
 */
#pragma once
#include <cstdint>
#include <vector>

namespace rd {

/// HDR equirect 环境:RGBA float 像素(A 通道无义,统一 1)。
struct HdrEnv {
  uint32_t width = 0, height = 0;
  std::vector<float> pixels;  ///< RGBA32F 紧凑
  bool valid() const { return width > 0 && height > 0 && !pixels.empty(); }
};

/// 加载 .hdr;失败(不存在/非 hdr)返回 false。
bool loadHdrEnv(const char* path, HdrEnv& out);

} // namespace rd
```

`core/resource/hdr_env.cpp`:

```cpp
#include "resource/hdr_env.h"
#include "foundation/log.h"
// stb_image 实现宏在 image_codec.cpp;此处只调 API(链接 rd_core 内 stb)
#include <stb_image.h>

namespace rd {

bool loadHdrEnv(const char* path, HdrEnv& out) {
  if (!path || !path[0]) return false;
  if (!stbi_is_hdr(path)) {
    RD_LOGW("resource.hdr", "非 HDR 文件: %s", path);
    return false;
  }
  int w = 0, h = 0, n = 0;
  float* data = stbi_loadf(path, &w, &h, &n, 4);  // 强制 RGBA
  if (!data) {
    RD_LOGW("resource.hdr", "解码失败: %s", path);
    return false;
  }
  out.width = uint32_t(w);
  out.height = uint32_t(h);
  out.pixels.assign(data, data + size_t(w) * h * 4);
  stbi_image_free(data);
  RD_LOGI("resource.hdr", "HDR 环境加载 %ux%u: %s", out.width, out.height, path);
  return true;
}

} // namespace rd
```

`core/CMakeLists.txt` 追加 `resource/hdr_env.cpp`。

- [ ] **Step 4: 跑测试**(全绿,HdrEnv.* 通过)

- [ ] **Step 5: Commit**

`git commit -m "feat(resource): .hdr equirect 环境加载(stb float 解码)"`

---

### Task 2: Environment HDR 模式 + engine API

**Files:**
- Create: `shaders/equirect_to_cube.frag`
- Modify: `core/renderer/environment.{h,cpp}`(HDR 模式)、`core/renderer/renderer.{h,cpp}`(透传)、`core/api/rd_api.{h,cpp}`(API)、`shaders/CMakeLists.txt`(注册新 shader)、embedded 列表
- Test: `tests/api/api_test.cpp`(环境切换冒烟);程序化 golden 全回归

- [ ] **Step 1: equirect_to_cube.frag**

```glsl
#version 450
// equirect → cube face:方向(face basis UBO,与 prefilter 同布局)→ equirect uv 采样
layout(location = 0) in vec2 vUV;
layout(binding = 0) uniform EquirectUBO {
  vec4 basis0;   // fwd xyz, x=roughness(此 shader 不用)
  vec4 basis1;   // right xyz
  vec4 basis2;   // upNdc xyz
} u;
layout(binding = 4) uniform sampler2D texEquirect;  // 纹理槽 0(宿主侧 bind slot0)
layout(location = 0) out vec4 outColor;
void main() {
  vec3 dir = normalize(u.basis0.xyz + (vUV.x * 2.0 - 1.0) * u.basis1.xyz +
                       (vUV.y * 2.0 - 1.0) * u.basis2.xyz);
  // 等距柱状:u = atan2(dir.z, dir.x)/(2π)+0.5,v = acos(dir.y)/π
  float uu = atan(dir.z, dir.x) * 0.1591549 + 0.5;
  float vv = acos(clamp(dir.y, -1.0, 1.0)) * 0.3183099;
  outColor = vec4(texture(texEquirect, vec2(uu, vv)).rgb, 1.0);
}
```

注意:prefilter.vert 的 vUV 输出与 face basis——核对 prefilter.frag 现有用法复刻。

- [ ] **Step 2: Environment HDR 模式**

`environment.h`:
```cpp
  /// HDR 模式:以 equirect float 像素建环境(build 前设置;nullptr=程序化)。
  void setHdrSource(const HdrEnv* env) { hdrSrc_ = env; }
```
成员:`const HdrEnv* hdrSrc_ = nullptr;`

`environment.cpp` build 内分支:
```cpp
  // 1. envTex_:HDR 模式 = equirect→cube GPU pass(RGBA16F);
  //    程序化模式 = CPU 像素直接上传(RGBA8,现状不动)
  Format envFormat = hdrSrc_ ? Format::RGBA16F : Format::RGBA8_UNORM;
  uint32_t envSize = hdrSrc_ ? 512 : env_.size;  // HDR cube 固定 512(质量/显存平衡)
  ...
  // HDR:上传 equirect 2D 纹理(RGBA32F)→ 逐面 pass 渲入 envTex_(16F cube)
  // SH9:HDR 从 equirect 源像素投影(projectToSHEquirect 新函数,CPU)
  // 程序化:沿用 projectToSH(env_.faces)
  // prefilterCube_:格式 = envFormat;预滤波管线 colorFormat 同步
  // 缓存:hdrSrc_ 非空跳过(日志注明 v1 不缓存 HDR)
```

- `projectToSHEquirect(const float* px, uint32_t w, uint32_t h)`:
  equirect 立体角加权(dω = sinθ dθ dφ)9 基投影——复用 projectToSH 的基函数,
  输入从 faces 换成 equirect 遍历。
- 预滤波循环:`td.colorFormat` / `colorFromTexture` 格式随 envFormat;
  预滤波管线按格式建(pd.colorFormat = envFormat)。

- [ ] **Step 3: renderer + api**

`renderer.h`:
```cpp
  /// HDR 环境(build 前设置;nullptr=程序化)。env 重建时生效。
  void setHdrEnvironment(const HdrEnv* env) { env_.setHdrSource(env); envDirty_... }
```
——Environment 重建时机:setQuality(ibl 尺寸变)/init。HDR 切换须强制重建:
`setHdrEnvironment` 里 `env_.destroy(dev); env_.build(...)` 需 dev——Renderer 持 dev_
指针(init 后有效),可以直接:
```cpp
  bool setHdrEnvironment(const rd::HdrEnv* env);  // renderer.cpp:destroy+build,失败回退程序化
```
`rd_api.cpp`:
```cpp
rd_result_t rd_engine_set_environment_hdri(rd_engine* e, const char* path) {
  if (!e || !path) return RD_ERROR_INVALID_ARG;
  if (!e->rendererReady) return RD_ERROR_SCENE;  // 需 surface 后(renderer 就绪)
  rd::HdrEnv env;
  if (!rd::loadHdrEnv(path, env)) { setError(e, "HDR 加载失败"); return RD_ERROR_ASSET; }
  e->hdrEnv = std::move(env);  // 引擎持有(生命周期)
  e->renderer.setHdrEnvironment(&e->hdrEnv);
  e->renderDirty = true;
  return RD_OK;
}
void rd_engine_set_environment_procedural(rd_engine* e) {
  if (!e) return;
  e->renderer.setHdrEnvironment(nullptr);
  e->renderDirty = true;
}
```
rd_api.h 声明两个 API。结构体加 `rd::HdrEnv hdrEnv;`。

- [ ] **Step 4: api 冒烟测试**

`tests/api/api_test.cpp` 追加:
```cpp
// HDR 环境 API:无 surface 拒绝;错误路径拒绝
TEST(Api, HdrEnvApi) {
  rd_engine* e = rd_engine_create(RD_BACKEND_METAL);
  ASSERT_NE(e, nullptr);
  EXPECT_EQ(rd_engine_set_environment_hdri(e, "/tmp/nope.hdr"), RD_ERROR_SCENE); // 无 surface
  rd_engine_set_environment_procedural(e);  // 不崩
  rd_engine_destroy(e);
}
```

- [ ] **Step 5: 全量回归(程序化 golden 零变化)+ Commit**

Run: `./scripts/check.sh 2>&1 | tail -4`(全绿;程序化路径 RGBA8 不动)
Commit: `feat(renderer): HDR 环境模式(equirect→cube 16F + SH9 equirect 投影)+ engine API`

---

### Task 3: 天空盒 pass + env.skybox/env.yaw 选项

**Files:**
- Create: `shaders/skybox.frag`
- Modify: `shaders/CMakeLists.txt`、`core/api/options.json`、`core/renderer/renderer.{h,cpp}`(skybox pipeline+draw)、`core/api/rd_api.cpp`(applyOptions 接线)
- Test: golden_hdr_env(新)+ 全量回归

- [ ] **Step 1: skybox.frag**

```glsl
#version 450
// 天空盒:视方向(顶点插值)→ cubemap 采样;LDR 模式 Reinhard(与 pbr 一致)
layout(location = 0) in vec3 vDir;
layout(binding = 4) uniform samplerCube texSky;      // 槽 4?——避开口:复用槽 5?texPrefilter
// ——槽位:天空盒采样 prefilterCube 的 mip0(=envTex 效果,省绑定);
//   用 slot5(texPrefilter)+ textureLod(...,0)
layout(binding = 2) uniform LightUBO { ... } lu;  // hdrMode 判断(与 pbr 同布局,只用 lightCount.y)
layout(location = 0) out vec4 outColor;
void main() {
  vec3 c = textureLod(texSky, normalize(vDir), 0.0).rgb;
  if (lu.lightCount.y < 0.5) c = c / (c + 1.0);  // LDR Reinhard(与 pbr.frag 一致)
  outColor = vec4(c, 1.0);
}
```
——bind 全布局太啰嗦;**简化:单独小 UBO?** 天空盒 frag 绑定 LightUBO 全 352B
没问题(块名匹配即可)。采用 slot5 prefilterCube mip0 采样。

skybox 顶点:复用 blit.vert?blit.vert 输出 vUV——需要 vDir。**新 skybox.vert**:
```glsl
#version 450
layout(location = 0) in vec3 aDir;  // 每顶点视线方向(CPU 计算,含 yaw)
layout(location = 0) out vec3 vDir;
void main() {
  vDir = aDir;
  // 全屏三角形位置由 gl_VertexIndex 生成(与 blit 同);z=1 远平面
  vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2) * 2.0 - 1.0;
  gl_Position = vec4(p, 1.0, 1.0);  // NDC z=1(远平面;LessEqual 通过)
}
```

- [ ] **Step 2: renderer skybox 接线**

`renderer.h` 成员:`PipelineHandle skyboxPipeline_; BufferHandle skyboxVb_;
bool skyboxEnabled_ = false; float envYawDeg_ = 0;`+`setSkyboxEnabled/setEnvYaw`。
- ensureScenePipelines 里建 skybox 管线(vertexBindings: 1 binding vec3 @0,
  stride 12;depthTest on(LessEqual 需 PipelineDesc compareOp——查现状默认 Less;
  PipelineDesc 有 compareOp 字段?)depthWrite off,cull off)。
  ——侦察点:PipelineDesc depthCompare 字段名按现状用。
- skyboxVb_:3×vec3 hostWrite 顶点缓冲;endScene 场景 pass 内、item 循环前:
```cpp
  if (skyboxEnabled_ && env_.prefilterCube().valid()) {
    // 每帧算 3 角视线方向:dir = normalize(invViewProj * vec4(ndc,1,1).xyz) 后 rotY(yaw)
    ... 写 skyboxVb_ ...
    cmd->bindPipeline(skyboxPipeline_);
    cmd->bindVertexBuffer(0, skyboxVb_, 0);
    cmd->bindTexture(5, env_.prefilterCube(), env_.cubeSampler());
    cmd->bindUniformBuffer(2, lightUbo_, 0, sizeof(LightUBOData));  // hdrMode
    cmd->draw(3, 0);
  }
```
  天空盒深度:z=1 + LessEqual + depthWrite off,先画(物体覆盖它)。

- [ ] **Step 3: options + engine 接线**

options.json 追加:
```json
"env.skybox": {"type": "bool", "default": false, "doc": "天空盒背景开关"},
"env.yaw_deg": {
  "type": "float", "default": 0.0,
  "domain": {"style": "range", "min": -180.0, "max": 180.0, "step": 1.0},
  "doc": "环境绕 Y 旋转(度)"
}
```
applyOptions 追加:`e->renderer.setSkyboxEnabled(o.env.skybox);`
`e->renderer.setEnvYaw(o.env.yaw_deg);`

**env.yaw 烘焙**:yaw 作用于 equirect 采样(u 偏移)与程序化 genEnvCubemap(方向旋转)——
Environment build 时烘进 cube(重建生效);Renderer.setEnvYaw 记录值,
值变化 → env 重建(envDirty)。v1:yaw 变化触发 Environment rebuild。
——简化 v1:yaw 只烘 equirect→cube pass(UBO 加 yaw float,采样 u 偏移);
程序化模式 yaw 无效(记文档)。

- [ ] **Step 4: golden_hdr_env 用例**

`tests/golden/` 新增(或现有文件追加):RD_GOLDEN_TEST 宏:
- 场景:头盔 + 运行时生成的渐变 hdr(stbi_write_hdr 到 tmp)+ env.skybox=true
- 双后端(metal/vulkan)golden 各一张
- 断言:SSIM < 0.05;golden 文件生成后像素核对提交

- [ ] **Step 5: 全量回归 + Commit**

Run: `./scripts/check.sh`(全绿;程序化 golden 零回归——skybox 默认关)
Commit: `feat(renderer): 天空盒 pass + env.skybox/env.yaw 选项 + golden_hdr_env`

---

### Task 4: AGENTS.md 收尾

- [ ] **Step 1: 更新 + 全量回归 + Commit**

```markdown
- HDR 环境+天空盒:.hdr equirect(stb float)→ equirect_to_cube pass(RGBA16F)
  → SH9(equirect 立体角加权投影)+ 16F 预滤波;程序化模式保持 RGBA8 零回归;
  HDR 模式 v1 无磁盘缓存(readbackTarget 仅 RGBA8,v2 待 readback16F);
  天空盒=场景 pass 内首画(z=1 LessEqual depthWrite off,slot5 prefilterCube mip0,
  LDR Reinhard 与 pbr 一致);选项 env.skybox(默认关)/env.yaw_deg(烘进 equirect pass)
```

---

## 附:已知风险与决策记录

| 风险/决策 | 处理 |
|---|---|
| 16F 预滤波格式切换 | 仅 HDR 模式;程序化 RGBA8 不动(零回归) |
| HDR 磁盘缓存 | v1 跳过(readbackTarget 仅 RGBA8);v2 readback16F |
| 天空盒 UBO | 复用 LightUBO(hdrMode);方向经顶点属性(CPU 算)免 invViewProj |
| env.yaw 实时性 | 烘进 equirect pass(重建级,非每帧);v2 可改 shader 旋转 |
| 纹理槽位 | 天空盒用 slot5 prefilterCube mip0(不新增槽位) |

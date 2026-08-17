# P2-1:阴影 + 多光源 实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 按 `docs/superpowers/specs/2026-08-17-p2-1-shadow-multilight-design.md` 实现:①RHI 可采样深度纹理 + 比较采样器 + depth-only 渲染目标;②glTF KHR_lights_punctual 解析;③renderer LightUBO + ShadowPass + PCF 3x3 阴影 + 多光源 PBR;④C API 灯光/阴影 + 画质联动。

**Architecture:** ShadowPass 在 endScene 内场景 pass 之前(渲染到 depth-only shadow map,尺寸/开关按画质档);pbr_forward 多光源循环 + 方向光阴影系数;灯光双通道(glTF 解析 / C API);阴影取景按模型包围球。

**Tech Stack:** C++17、既有三后端 RHI(2c 已具 MSAA/压缩纹理/目标可采样化)、libktx、离线 shader 管线。

**通用约定(全任务遵守):**
- 构建+测试:`./scripts/check.sh`(弱网先 `export RD_DEPS_MIRROR=$HOME/.rd_deps_mirror`);单跑:`ctest --test-dir build --output-on-failure -R <正则>`
- golden 更新:`RD_UPDATE_GOLDENS=1 ctest --test-dir build -R <用例>` 后**像素核对**再提交
- 提交规范:每 Task 一个 commit,`<type>(<scope>): 描述`
- 绑定约定:uniform slot N ↔ Metal buffer(N) ↔ Vulkan set0 binding N ↔ GLES binding N;texture slot N ↔ Metal texture/sampler(N+4) ↔ Vulkan set0 binding(N+4) ↔ GLES 单元 N(sampler 名 `texN`)
- UBO:slot0=FrameUBO(256B),slot1=ItemUBO(256B 步进),**slot2=LightUBO(352B,本阶段新增)**;GLES 块名表 UBO/FrameUBO/BlitUBO→0,ItemUBO→1,**LightUBO→2,ShadowUBO→0(shadow pass 内 slot0)**
- 纹理槽:0=baseColor,1=MR,2=normal,3=emissive,4=occlusion,5=prefilterCube,6=brdfLut,**7=shadowMap(比较采样器)**
- 顶点布局:pos3@0|normal3@12|tangent4@24|uv2@40 交错 stride 48
- 光源方向约定:`L` = 指向光源的方向(shader 内 dot(N,L) 直接用);glTF 灯方向=节点旋转×(0,0,-1) 后取反为 L
- 资源创建时机:**全部资源须在 acquireCommandBuffer 之前创建**(Vulkan 上传路径重置共享命令缓冲)
- NDC/v 方向:Metal/Vulkan 渲染 ndcY+1 → 纹理内存行 0(v=0);GLES → 末行(v=1);阴影 UV 的 v 翻转由 LightUBO.shadowParams.w 吸收(同 blit vFlip 手法)

---

### Task 1: 比较采样器 + D32 渲染目标纹理(三后端)

**Files:**
- Modify: `core/rhi/rhi_types.h`(SamplerDesc.compareEnable)
- Modify: `core/rhi/backends/metal/metal_device.mm`(compareFunction + D32 RT 存储)
- Modify: `core/rhi/backends/vulkan/vulkan_device.cpp`(compareEnable + D32 RT usage/aspect)
- Modify: `core/rhi/backends/gles/gles_device.cpp`(TEXTURE_COMPARE_MODE + D32 格式三元组)
- Test: `tests/rhi/depth_texture_test.cpp`(新建)、`tests/CMakeLists.txt`(注册)

- [ ] **Step 1: 写失败测试**

`tests/rhi/depth_texture_test.cpp`:

```cpp
// 深度纹理(可渲染目标+可采样)与比较采样器创建契约。
#include <gtest/gtest.h>
#include "rhi/rhi_device.h"

namespace {
void runCreate(rd::Backend b) {
  rd::DeviceDesc d;
  d.backend = b;
  auto dev = rd::createDevice(d);
  ASSERT_NE(dev, nullptr);

  // D32 渲染目标纹理(可采样)
  rd::TextureDesc td;
  td.format = rd::Format::D32_FLOAT;
  td.width = 256;
  td.height = 256;
  td.usage = rd::TextureUsage::Sampled | rd::TextureUsage::RenderTargetAttachment;
  auto tex = dev->createTexture(td);
  EXPECT_TRUE(tex.valid()) << "D32 RT 纹理创建失败";

  // 比较采样器
  rd::SamplerDesc sd;
  sd.compareEnable = true;
  sd.minFilter = rd::Filter::Linear;
  sd.magFilter = rd::Filter::Linear;
  auto cmp = dev->createSampler(sd);
  EXPECT_TRUE(cmp.valid()) << "比较采样器创建失败";

  // 默认(compareEnable=false)不受影响
  auto plain = dev->createSampler({});
  EXPECT_TRUE(plain.valid());

  dev->destroySampler(cmp);
  dev->destroySampler(plain);
  dev->destroyTexture(tex);
}
} // namespace

TEST(DepthTexture, Metal) {
#if defined(__APPLE__)
  runCreate(rd::Backend::Metal);
#endif
}
TEST(DepthTexture, Vulkan) {
#if defined(RD_WITH_VULKAN)
  runCreate(rd::Backend::Vulkan);
#endif
}
```

`tests/CMakeLists.txt` 追加 `rhi/depth_texture_test.cpp`。

- [ ] **Step 2: 跑测试确认失败**

Run: `./scripts/check.sh 2>&1 | tail -5`
Expected: 编译错误(`SamplerDesc` 无 `compareEnable`)

- [ ] **Step 3: rhi_types.h**

`SamplerDesc` 追加:

```cpp
  /// 深度比较模式(阴影采样):开启后采样返回比较结果(硬件 PCF 基元);
  /// 仅对 D32 等深度纹理有意义。Vulkan=compareEnable+LESS,Metal=compareFunction,
  /// GLES=TEXTURE_COMPARE_MODE/COMPARE_REF_TO_TEXTURE。
  bool compareEnable = false;
```

- [ ] **Step 4: Metal**

`metal_device.mm`:
1. createSampler 内(`MTLSamplerDescriptor* sd` 填充处)追加:

```objc
    if (desc.compareEnable) sd.compareFunction = MTLCompareFunctionLess;
```

2. createTexture:D32 + RenderTargetAttachment 时存储模式改 Private(采样合法):

```objc
    td.storageMode = desc.format == Format::D32_FLOAT ? MTLStorageModePrivate
                                                      : MTLStorageModeShared;
```

- [ ] **Step 5: Vulkan**

`vulkan_device.cpp` createTexture(约 1116 行附近):
1. usage 组装按格式分支:

```cpp
  const bool isDepth = desc.format == Format::D32_FLOAT;
  ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
              VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  if (hasFlag(desc.usage, TextureUsage::RenderTargetAttachment))
    ici.usage |= isDepth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                         : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
```

2. 视图 aspect 分支(两处:createTexture 的 vci 与纹理视图;搜 `VK_IMAGE_ASPECT_COLOR_BIT` 在 createTexture 内的出现):

```cpp
  vci.subresourceRange = {isDepth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT,
                          0, desc.mipLevels, 0, faces};
```

3. createSampler 追加:

```cpp
  if (desc.compareEnable) {
    sci.compareEnable = VK_TRUE;
    sci.compareOp = VK_COMPARE_OP_LESS;
  }
```

- [ ] **Step 6: GLES**

`gles_device.cpp`:
1. `toGLTexFormat` 追加:

```cpp
      case Format::D32_FLOAT:
        internal = GL_DEPTH_COMPONENT32F; upload = GL_DEPTH_COMPONENT; type = GL_FLOAT;
        break;
```

2. createSampler 追加:

```cpp
  if (desc.compareEnable) {
    glSamplerParameteri(s, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
    glSamplerParameteri(s, GL_TEXTURE_COMPARE_FUNC, GL_LESS);
  }
```

- [ ] **Step 7: 跑测试 + Android 编译验证**

Run: `./scripts/check.sh 2>&1 | tail -5`
Expected: 全绿(DepthTexture.Metal/Vulkan 通过)
Run: `source /tmp/rd_env.sh && export RD_DEPS_MIRROR=$HOME/.rd_deps_mirror && cd samples/android && ./gradlew :app:assembleDebug 2>&1 | tail -2`
Expected: BUILD SUCCESSFUL(GLES 编译验证)

- [ ] **Step 8: Commit**

```bash
git add core/rhi tests/rhi/depth_texture_test.cpp tests/CMakeLists.txt
git commit -m "feat(rhi): 比较采样器 + D32 渲染目标纹理(三后端)"
```

---

### Task 2: depth-only 渲染目标 + PipelineDesc.depthOnly + 阴影采样契约

**Files:**
- Modify: `core/rhi/rhi_types.h`(OffscreenTargetDesc.depthFromTexture + PipelineDesc.depthOnly)
- Modify: `core/rhi/backends/vulkan/vulkan_device.cpp`(depth-only pass 变体 + endRenderPass 分支)
- Modify: `core/rhi/backends/metal/metal_device.mm`(depth-only render pass)
- Modify: `core/rhi/backends/gles/gles_device.cpp`(depth-only FBO + glDrawBuffer NONE)
- Create: `shaders/shadow_depth.vert`、`shaders/shadow_depth.frag`
- Modify: `shaders/CMakeLists.txt`
- Test: `tests/rhi/shadow_target_test.cpp`(新建)、`tests/CMakeLists.txt`(注册)

- [ ] **Step 1: shadow_depth shader(先行,契约测试要用)**

`shaders/shadow_depth.vert`(纯深度写出,无顶点属性也可——用 location0 pos):

```glsl
// shadow_depth.vert:顶点 → 光源空间(纯深度写出用)。
// 契约测试用 binding0 直传 mvp;renderer 集成见 Task 4。
#version 450
layout(location = 0) in vec3 aPos;
layout(binding = 0) uniform ShadowUBO { mat4 mvp; } u;  // uniform slot 0 ↔ binding 0
void main() { gl_Position = u.mvp * vec4(aPos, 1.0); }
```

`shaders/shadow_depth.frag`:

```glsl
// shadow_depth.frag:depth-only pass 无颜色输出。
#version 450
void main() {}
```

`shaders/CMakeLists.txt` 追加 `rd_compile_shader(shadow_depth.vert)` / `rd_compile_shader(shadow_depth.frag)`。

契约测试采样用 shader(比较采样器 → 灰度输出)。新建 `shaders/shadow_sample.vert/frag`:

```glsl
// shadow_sample.vert:全屏三角形 + 阴影 UV(参考深度固定 0.5)。
#version 450
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 0) out vec2 vUV;
void main() { vUV = aUV; gl_Position = vec4(aPos, 1.0); }
```

```glsl
// shadow_sample.frag:sampler2DShadow 比较采样,输出灰度(0=阴影,1=受光)。
#version 450
layout(location = 0) in vec2 vUV;
layout(binding = 4) uniform sampler2DShadow tex0;  // texture slot 0 → binding 4
layout(location = 0) out vec4 outColor;
void main() { outColor = vec4(vec3(texture(tex0, vec3(vUV, 0.5))), 1.0); }
```

`shaders/CMakeLists.txt` 追加这两个。

- [ ] **Step 2: 写失败测试**

`tests/rhi/shadow_target_test.cpp`:

```cpp
// depth-only 渲染目标契约:渲三角形到深度纹理 → 比较采样器采样 → 前后景灰度正确。
#include <gtest/gtest.h>
#include "common/shader_code.h"
#include "rhi/rhi_device.h"
#include "rd_shader_dir.h"
#include <glm/glm.hpp>
#include <vector>

namespace {
constexpr uint32_t kW = 64, kH = 64;

void runShadowTarget(rd::Backend b) {
  rd::DeviceDesc d;
  d.backend = b;
  auto dev = rd::createDevice(d);
  ASSERT_NE(dev, nullptr);
  if (!dev->caps().supports(rd::Capability::depth_texture)) GTEST_SKIP() << "无深度纹理";

  // ---- 资源(全部在 acquireCommandBuffer 之前)----
  // 深度纹理 + depth-only 目标
  rd::TextureDesc tdd;
  tdd.format = rd::Format::D32_FLOAT;
  tdd.width = kW;
  tdd.height = kH;
  tdd.usage = rd::TextureUsage::Sampled | rd::TextureUsage::RenderTargetAttachment;
  auto depthTex = dev->createTexture(tdd);
  ASSERT_TRUE(depthTex.valid());
  rd::OffscreenTargetDesc od;
  od.width = kW;
  od.height = kH;
  od.depthFromTexture = depthTex;
  auto shadowTarget = dev->createOffscreenTarget(od);
  ASSERT_TRUE(shadowTarget.valid()) << "depth-only 目标创建失败";
  uint32_t w = 0, h = 0;
  dev->targetSize(shadowTarget, w, h);
  EXPECT_EQ(w, kW);
  EXPECT_EQ(h, kH);
  EXPECT_TRUE(dev->targetColorTexture(shadowTarget).valid());  // 返回深度纹理
  // readback 深度目标 → false
  std::vector<uint8_t> junk(16);
  EXPECT_FALSE(dev->readbackTarget(shadowTarget, junk.data(), junk.size()));

  auto load = [&](const char* n) { return rd::test::loadShaderCode(b, RD_SHADER_DIR, n); };
  auto sdVs = load("shadow_depth.vert"), sdFs = load("shadow_depth.frag");
  auto smpVs = load("shadow_sample.vert"), smpFs = load("shadow_sample.frag");
  auto sdVsM = dev->createShaderModule({rd::ShaderStage::Vertex, sdVs.code, sdVs.entry});
  auto sdFsM = dev->createShaderModule({rd::ShaderStage::Fragment, sdFs.code, sdFs.entry});
  auto smpVsM = dev->createShaderModule({rd::ShaderStage::Vertex, smpVs.code, smpVs.entry});
  auto smpFsM = dev->createShaderModule({rd::ShaderStage::Fragment, smpFs.code, smpFs.entry});
  ASSERT_TRUE(sdVsM.valid() && sdFsM.valid() && smpVsM.valid() && smpFsM.valid());

  // 深度写入管线(depthOnly)
  rd::PipelineDesc dpd;
  dpd.vertexShader = sdVsM;
  dpd.fragmentShader = sdFsM;
  dpd.vertexBindings = {{0, 12}};
  dpd.attributes = {{0, rd::Format::R32G32B32_FLOAT, 0, 0}};
  dpd.depthOnly = true;
  dpd.depthTest = true;
  dpd.depthWrite = true;
  auto depthPipe = dev->createPipeline(dpd);
  ASSERT_TRUE(depthPipe.valid()) << "depthOnly 管线创建失败";

  // 采样管线(普通离屏目标)
  rd::PipelineDesc spd;
  spd.vertexShader = smpVsM;
  spd.fragmentShader = smpFsM;
  spd.vertexBindings = {{0, 20}};
  spd.attributes = {{0, rd::Format::R32G32B32_FLOAT, 0, 0},
                    {1, rd::Format::R32G32_FLOAT, 12, 0}};
  auto samplePipe = dev->createPipeline(spd);
  ASSERT_TRUE(samplePipe.valid());

  // mvp:覆盖左半屏的三角形(z=0.4 < 参考 0.5 → 受光;其余区域远平面 → 阴影)
  glm::mat4 mvp(1.0f);
  auto ubo = dev->createBuffer({64, rd::BufferUsage::Uniform, true, false, nullptr});
  ASSERT_TRUE(ubo.valid());
  dev->updateBuffer(ubo, &mvp, 64, 0);
  const float tri[3 * 3] = {-1, -1, 0.4f, 0, -1, 0.4f, -1, 1, 0.4f};  // 左下三角
  auto vbo = dev->createBuffer({sizeof(tri), rd::BufferUsage::Vertex, false, false, tri});
  const float quad[6 * 5] = {-1, -1, 0, 0, 0, 1, -1, 0, 1, 0, -1, 1, 0, 0, 1,
                             1,  -1, 0, 1, 0, 1, 1,  0, 1, 1, -1, 1, 0, 0, 1};
  auto quadVbo = dev->createBuffer({sizeof(quad), rd::BufferUsage::Vertex, false, false, quad});
  ASSERT_TRUE(vbo.valid() && quadVbo.valid());
  rd::SamplerDesc csd;
  csd.compareEnable = true;
  auto cmpSampler = dev->createSampler(csd);
  ASSERT_TRUE(cmpSampler.valid());
  rd::OffscreenTargetDesc cod;
  cod.width = kW;
  cod.height = kH;
  auto colorTarget = dev->createOffscreenTarget(cod);
  ASSERT_TRUE(colorTarget.valid());

  // ---- pass1:三角形深度写入 shadowTarget ----
  auto* cmd = dev->acquireCommandBuffer();
  cmd->beginRenderPass(shadowTarget, {0, 0, 0, 1, 1.0f});  // clear.depth=1
  cmd->bindPipeline(depthPipe);
  cmd->bindVertexBuffer(0, vbo, 0);
  cmd->bindUniformBuffer(0, ubo, 0, 64);
  cmd->draw(3, 0);
  cmd->endRenderPass();
  // ---- pass2:比较采样到 colorTarget ----
  cmd->beginRenderPass(colorTarget, {0, 0, 0, 1});
  cmd->bindPipeline(samplePipe);
  cmd->bindVertexBuffer(0, quadVbo, 0);
  cmd->bindTexture(0, depthTex, cmpSampler);
  cmd->draw(6, 0);
  cmd->endRenderPass();
  dev->submit(cmd);
  dev->waitIdle();

  std::vector<uint8_t> px(size_t(kW) * kH * 4);
  ASSERT_TRUE(dev->readbackTarget(colorTarget, px.data(), px.size()));
  // 三角形覆盖区(左下):深度 0.4 < 0.5 → 受光(≈255);右上(未覆盖,远平面 1.0)→ 阴影(0)
  const auto at = [&](uint32_t x, uint32_t y) {
    return px[(size_t(y) * kW + x) * 4];
  };
  EXPECT_GT(at(kW / 8, kH * 7 / 8), 200u) << "三角形内应受光";  // 左下区域(顶向下坐标)
  EXPECT_LT(at(kW * 7 / 8, kH / 8), 60u) << "未覆盖区应阴影";

  dev->destroySampler(cmpSampler);
  dev->destroyBuffer(ubo);
  dev->destroyBuffer(vbo);
  dev->destroyBuffer(quadVbo);
  dev->destroyTarget(colorTarget);
  dev->destroyTarget(shadowTarget);
  dev->destroyTexture(depthTex);
  dev->destroyPipeline(depthPipe);
  dev->destroyPipeline(samplePipe);
  dev->destroyShaderModule(sdVsM);
  dev->destroyShaderModule(sdFsM);
  dev->destroyShaderModule(smpVsM);
  dev->destroyShaderModule(smpFsM);
}
} // namespace

TEST(ShadowTarget, Metal) {
#if defined(__APPLE__)
  runShadowTarget(rd::Backend::Metal);
#endif
}
TEST(ShadowTarget, Vulkan) {
#if defined(RD_WITH_VULKAN)
  runShadowTarget(rd::Backend::Vulkan);
#endif
}
```

`tests/CMakeLists.txt` 追加 `rhi/shadow_target_test.cpp`。

注意:ClearColor 结构若无 depth 成员,先查 `rhi_types.h` 的 ClearColor 定义
(现有 depth 附件清屏用 clear.depth——若 ClearColor 无 depth 字段,给它加
`float depth = 1.0f` 并核对现有调用点)。

- [ ] **Step 3: 跑测试确认失败**

Run: `./scripts/check.sh 2>&1 | tail -5`
Expected: 编译错误(`depthFromTexture`/`depthOnly` 未定义)

- [ ] **Step 4: rhi_types.h**

1. `OffscreenTargetDesc` 追加:

```cpp
  /// 非空:挂载该 D32 纹理为深度附件、无颜色附件(depth-only 目标,阴影贴图用);
  /// 与 colorFromTexture 互斥;sampleCount 须为 1。纹理须以
  /// Format::D32_FLOAT + RenderTargetAttachment 创建。
  TextureHandle depthFromTexture;
```

2. `PipelineDesc` 追加:

```cpp
  /// 纯深度管线(depth-only 渲染目标用):无颜色附件状态。
  bool depthOnly = false;
```

3. 核对 `ClearColor` 定义有 `float depth`(现有 beginRenderPass 已用 clear.depth,
   无则补 `float depth = 1.0f`)。

- [ ] **Step 5: Vulkan 实现**

`vulkan_device.cpp`:
1. `findOrCreateRenderPass` 的 depth-only 变体:当 `format == VK_FORMAT_UNDEFINED`
   时生成纯深度 pass(单深度附件,storeOp=STORE,finalLayout=SHADER_READ_ONLY;
   无颜色/resolve;samples 恒 1):

```cpp
  if (format == VK_FORMAT_UNDEFINED) {  // depth-only(shadow map)
    VkAttachmentDescription depth{};
    depth.format = VK_FORMAT_D32_SFLOAT;
    depth.samples = VK_SAMPLE_COUNT_1_BIT;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;  // 内容要采样,必须 STORE
    depth.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depth.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkAttachmentReference depthRef{0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 0;
    subpass.pDepthStencilAttachment = &depthRef;
    VkSubpassDependency deps[2]{};
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass = 0;
    deps[0].srcStageMask = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    deps[0].dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    deps[0].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    deps[1].srcSubpass = 0;
    deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    deps[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[1].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpci.attachmentCount = 1;
    rpci.pAttachments = &depth;
    rpci.subpassCount = 1;
    rpci.pSubpasses = &subpass;
    rpci.dependencyCount = 2;
    rpci.pDependencies = deps;
    VkRenderPass rp = VK_NULL_HANDLE;
    if (vkCreateRenderPass(device_, &rpci, nullptr, &rp) != VK_SUCCESS)
      return VK_NULL_HANDLE;
    renderPasses_.emplace(key, rp);
    return rp;
  }
```

(放在函数体开头 key 计算与缓存查找之后)
2. `createOffscreenTarget` depthFromTexture 分支(texture-backed 分支之前):

```cpp
  if (desc.depthFromTexture.valid()) {
    if (desc.sampleCount != 1) {
      RD_LOGE("rhi.vk", "depth-only 目标不支持 MSAA");
      return {};
    }
    auto it = textures_.find(desc.depthFromTexture);
    if (it == textures_.end() || it->second.format != Format::D32_FLOAT) return {};
    TargetRec rec{};
    rec.width = desc.width;
    rec.height = desc.height;
    rec.textureBacked = true;
    rec.depthOnly = true;   // TargetRec 加此字段
    rec.srcTexture = desc.depthFromTexture;
    rec.format = Format::D32_FLOAT;
    rec.hasDepth = true;
    // framebuffer 直接挂深度纹理的视图
    VkFramebufferCreateInfo fbci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fbci.renderPass = findOrCreateRenderPass(VK_FORMAT_UNDEFINED, true, 1);
    fbci.attachmentCount = 1;
    fbci.pAttachments = &it->second.view;
    fbci.width = desc.width;
    fbci.height = desc.height;
    fbci.layers = 1;
    if (vkCreateFramebuffer(device_, &fbci, nullptr, &rec.fb) != VK_SUCCESS) return {};
    TargetHandle h(nextId_++);
    targets_.emplace(h, rec);
    return h;
  }
```

3. TargetRec 加 `bool depthOnly = false;`。
4. `beginRenderPass`:depthOnly 时 clearValueCount=1(clears[0]=depth);
   pass 查询用 `renderPassAt(VK_FORMAT_UNDEFINED, true, 1)`:

```cpp
  if (t.depthOnly) {
    clears[0].depthStencil = {clear.depth, 0};
    rp.renderPass = device_->renderPassAt(VK_FORMAT_UNDEFINED, true, 1);
    rp.clearValueCount = 1;
  } else {
    // 现有逻辑
  }
```

5. `endRenderPass`:depthOnly 分支(在 textureBacked 分支之前):

```cpp
  if (t.depthOnly) {
    // 深度内容转 SHADER_READ 供采样(与 render pass finalLayout 一致,空操作亦可;
    // 显式 transition 保证 subLayouts 追踪正确)
    device_->recordTextureTransition(
        cmd_, t.srcTexture, 0, 0, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    return;
  }
```

(若 recordTextureTransition 对 from==to 直接更新追踪即可;或者更简单:render pass
finalLayout 已是 SHADER_READ,只需把 textures_ 里该纹理的 subLayouts[0] 同步——
加 `VulkanDevice::markTextureLayout(tex, layout)` 辅助直接写追踪表。选后者,零 GPU 开销)
6. `readbackTarget`:`t.depthOnly` 返回 false。
7. `targetColorTexture`:depthOnly → 返回 srcTexture。
8. `createPipeline`:depthOnly 时 `gpci.renderPass = findOrCreateRenderPass(VK_FORMAT_UNDEFINED, true, 1)`,
   且 `cb.attachmentCount = 0; gpci.pColorBlendState = &cb`(attachmentCount 0);
   depthStencil 强制 depthTest/Write 开。
9. `destroyTarget`:depthOnly 只销毁 fb(纹理归调用方)——与 textureBacked 同路径
   (检查现 retire 闭包对 depthOnly 的 fb/view:depthOnly 没有独立 view,只销毁 fb)。

- [ ] **Step 6: Metal 实现**

`metal_device.mm`:
1. TargetRec 加 `bool depthOnly = false;`。
2. `createOffscreenTarget` 入口(depthFromTexture 分支):

```objc
    if (desc.depthFromTexture.valid()) {
      if (desc.sampleCount != 1) return {};
      auto it = textures_.find(desc.depthFromTexture);
      if (it == textures_.end() || it->second.format != Format::D32_FLOAT) return {};
      TargetHandle h(nextId_++);
      TargetRec rec;
      rec.width = desc.width;
      rec.height = desc.height;
      rec.depthOnly = true;
      rec.hasDepth = true;
      rec.depth = it->second.texture;  // 共享底层纹理(ARC)
      rec.srcTexture = desc.depthFromTexture;
      targets_.emplace(h, rec);
      return h;
    }
```

3. `beginRenderPass`:depthOnly 时只挂深度:

```objc
  if (t.depthOnly) {
    rp.depthAttachment.texture = t.depth;
    rp.depthAttachment.loadAction = MTLLoadActionClear;
    rp.depthAttachment.clearDepth = clear.depth;
    rp.depthAttachment.storeAction = MTLStoreActionStore;  // 阴影图要保留
  } else { /* 现有颜色/深度逻辑 */ }
```

4. `readbackTarget`:depthOnly 返回 false。
5. `targetColorTexture`:depthOnly → srcTexture。
6. `createPipeline`:depthOnly 时跳过 `pd.colorAttachments[0].pixelFormat` 设置,
   `pd.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float` 强制设置。

- [ ] **Step 7: GLES 实现**

`gles_device.cpp`:
1. TargetRec 加 `bool depthOnly = false;`。
2. `createOffscreenTarget` depthFromTexture 分支(texture-backed 之前):

```cpp
  if (desc.depthFromTexture.valid()) {
    if (desc.sampleCount != 1) return {};
    auto it = textures_.find(desc.depthFromTexture);
    if (it == textures_.end() || it->second.format != Format::D32_FLOAT) return {};
    TargetRec rec;
    rec.width = desc.width;
    rec.height = desc.height;
    rec.depthOnly = true;
    rec.hasDepth = true;
    rec.srcTexture = desc.depthFromTexture;
    glGenFramebuffers(1, &rec.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, rec.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D,
                           it->second.tex, 0);
    const bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (!ok) {
      glDeleteFramebuffers(1, &rec.fbo);
      return {};
    }
    TargetHandle h(nextId_++);
    targets_.emplace(h, rec);
    return h;
  }
```

3. `beginRenderPass` 回放闭包:depthOnly 时

```cpp
    if (t.depthOnly) {
      glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
      glViewport(0, 0, GLsizei(t.width), GLsizei(t.height));
      glDrawBuffer(GL_NONE);
      glReadBuffer(GL_NONE);
      glDepthMask(GL_TRUE);
      glClearDepthf(clear.depth);
      glClear(GL_DEPTH_BUFFER_BIT);
      return;
    }
```

(beginRenderPass 现有闭包内加该分支)
4. `readbackTarget`:depthOnly 返回 false。`targetColorTexture`:depthOnly → srcTexture。
5. `createPipeline`:GLES 无颜色附件状态概念,depthOnly 仅接受即可(删除拒绝逻辑无——
   本就没拒绝;确保 depthOnly 管线创建不被颜色格式影响,无操作)。

- [ ] **Step 8: 跑测试 + Android 编译**

Run: `./scripts/check.sh 2>&1 | tail -6`
Expected: 全绿(ShadowTarget.Metal/Vulkan 通过;既有用例不回归)
Run: `source /tmp/rd_env.sh && export RD_DEPS_MIRROR=$HOME/.rd_deps_mirror && cd samples/android && ./gradlew :app:assembleDebug 2>&1 | tail -2`
Expected: BUILD SUCCESSFUL

- [ ] **Step 9: Commit**

```bash
git add core/rhi shaders tests/rhi/shadow_target_test.cpp tests/CMakeLists.txt
git commit -m "feat(rhi): depth-only 渲染目标 + PipelineDesc.depthOnly + 阴影采样契约(三后端)"
```

---

### Task 3: glTF KHR_lights_punctual 解析

**Files:**
- Modify: `core/resource/gltf_loader.h`(LightData/LightType + ModelAsset.lights)
- Modify: `core/resource/gltf_loader.cpp`(解析实现)
- Test: `tests/resource/gltf_test.cpp`(追加)、`tests/CMakeLists.txt`(不变)

- [ ] **Step 1: 写失败测试**

`tests/resource/gltf_test.cpp` 追加:

```cpp
// KHR_lights_punctual:运行时生成带灯 gltf(节点旋转定方向),断言解析结果。
TEST(Gltf, PunctualLights) {
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() / "rd_gltf_lights";
  fs::create_directories(dir);
  // 三角形几何(pos only)
  const float pos[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
  const uint16_t idx[3] = {0, 1, 2};
  const std::string binPath = (dir / "tri.bin").string();
  {
    FILE* f = fopen(binPath.c_str(), "wb");
    ASSERT_NE(f, nullptr);
    fwrite(pos, 4, 9, f);
    fwrite(idx, 2, 3, f);
    fclose(f);
  }
  // 灯光:node1=方向光(绕 X 轴旋转定方向)、node2=点光(平移 1,2,3)
  // 绕 X 轴 -90° 旋转:glTF -Z 灯向 → 世界 -Y;L=-dir → +Y(0,1,0)
  const char* json = R"({
    "asset": {"version": "2.0"},
    "extensionsUsed": ["KHR_lights_punctual"],
    "extensions": {
      "KHR_lights_punctual": {
        "lights": [
          {"type": "directional", "color": [1, 0.5, 0.25], "intensity": 2.0},
          {"type": "point", "color": [1, 1, 1], "intensity": 4.0, "range": 10.0},
          {"type": "spot", "intensity": 1.0, "range": 5.0,
           "spot": {"innerConeAngle": 0.2, "outerConeAngle": 0.5}}
        ]
      }
    },
    "scenes": [{"nodes": [0, 1, 2, 3]}], "scene": 0,
    "nodes": [
      {"mesh": 0},
      {"light": 0, "rotation": [-0.7071068, 0, 0, 0.7071068]},
      {"light": 1, "translation": [1, 2, 3]},
      {"light": 2}
    ],
    "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, "indices": 1}]}],
    "buffers": [{"uri": "tri.bin", "byteLength": 42}],
    "bufferViews": [
      {"buffer": 0, "byteOffset": 0, "byteLength": 36},
      {"buffer": 0, "byteOffset": 36, "byteLength": 6}],
    "accessors": [
      {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"},
      {"bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR"}]
  })";
  const std::string gltfPath = (dir / "lit.gltf").string();
  {
    FILE* f = fopen(gltfPath.c_str(), "wb");
    ASSERT_NE(f, nullptr);
    fwrite(json, 1, strlen(json), f);
    fclose(f);
  }
  auto model = rd::loadGltf(gltfPath.c_str());
  ASSERT_TRUE(model.valid());
  ASSERT_EQ(model.lights.size(), 3u);

  // 方向光:L = 节点旋转×(0,0,-1) 取反 → (0,1,0);颜色×intensity
  const auto& d0 = model.lights[0];
  EXPECT_EQ(d0.type, rd::LightType::Directional);
  EXPECT_NEAR(d0.direction[0], 0.0f, 1e-3f);
  EXPECT_NEAR(d0.direction[1], 1.0f, 1e-3f);
  EXPECT_NEAR(d0.direction[2], 0.0f, 1e-3f);
  EXPECT_NEAR(d0.color[0], 2.0f, 1e-3f);    // 1.0 × 2.0
  EXPECT_NEAR(d0.color[1], 1.0f, 1e-3f);    // 0.5 × 2.0

  const auto& p1 = model.lights[1];
  EXPECT_EQ(p1.type, rd::LightType::Point);
  EXPECT_NEAR(p1.position[0], 1.0f, 1e-3f);
  EXPECT_NEAR(p1.position[1], 2.0f, 1e-3f);
  EXPECT_NEAR(p1.position[2], 3.0f, 1e-3f);
  EXPECT_NEAR(p1.range, 10.0f, 1e-3f);

  const auto& s2 = model.lights[2];
  EXPECT_EQ(s2.type, rd::LightType::Spot);
  EXPECT_NEAR(s2.innerCone, 0.2f, 1e-3f);
  EXPECT_NEAR(s2.outerCone, 0.5f, 1e-3f);
  EXPECT_NEAR(s2.range, 5.0f, 1e-3f);

  // 无灯模型:lights 为空(BoxTextured 无 KHR_lights_punctual)
  auto box = rd::loadGltf((std::string(kAssets) + "/BoxTextured.glb").c_str());
  ASSERT_TRUE(box.valid());
  EXPECT_TRUE(box.lights.empty());
}
```

- [ ] **Step 2: 跑测试确认失败**

Run: `./scripts/check.sh 2>&1 | tail -5`
Expected: 编译错误(`LightType`/`lights` 未定义)

- [ ] **Step 3: gltf_loader.h**

追加(放在 MaterialData 之前):

```cpp
/// 光源类型(KHR_lights_punctual 全类型)。
enum class LightType : uint32_t { Directional = 0, Point = 1, Spot = 2 };

/// 光源数据(渲染语义):方向为"指向光源的方向"(shader 内 dot(N,L) 直接用);
/// color 已乘 intensity。
struct LightData {
  LightType type = LightType::Directional;
  float direction[3] = {0, 1, 0};   ///< 指向光源(dir/spot 用)
  float position[3] = {0, 0, 0};    ///< point/spot 用
  float color[3] = {1, 1, 1};       ///< rgb × intensity
  float range = 0.0f;               ///< 0=无限
  float innerCone = 0.0f;           ///< spot 内锥角(弧度)
  float outerCone = 0.0f;           ///< spot 外锥角(弧度)
};
```

`ModelAsset` 追加成员:

```cpp
  std::vector<LightData> lights;    // KHR_lights_punctual(无则空;渲染层默认 1 方向光)
```

- [ ] **Step 4: gltf_loader.cpp 解析**

`loadGltf(path, pref)` 内,cgltf_free 之前追加:

```cpp
  // KHR_lights_punctual:遍历节点取世界变换(方向=旋转×(0,0,-1) 取反=+Z 列)
  for (cgltf_size ni = 0; ni < data->nodes_count; ++ni) {
    const cgltf_node* node = &data->nodes[ni];
    if (!node->light) continue;
    if (model.lights.size() >= 4) {
      RD_LOGW("resource.gltf", "灯光超过 4 盏,截断");
      break;
    }
    const cgltf_light* l = node->light;
    cgltf_float m[16];
    cgltf_node_transform_world(node, m);  // 列主序世界矩阵
    LightData out;
    out.direction[0] = m[8];   // +Z 列 = glTF 灯向(0,0,-1) 的反向 = 指向光源
    out.direction[1] = m[9];
    out.direction[2] = m[10];
    out.position[0] = m[12];
    out.position[1] = m[13];
    out.position[2] = m[14];
    const float intensity = l->intensity;
    out.color[0] = l->color[0] * intensity;
    out.color[1] = l->color[1] * intensity;
    out.color[2] = l->color[2] * intensity;
    out.range = l->range;
    switch (l->type) {
      case cgltf_light_type_directional: out.type = LightType::Directional; break;
      case cgltf_light_type_point: out.type = LightType::Point; break;
      default: out.type = LightType::Spot; break;
    }
    out.innerCone = l->spot_inner_cone_angle;
    out.outerCone = l->spot_outer_cone_angle;
    model.lights.push_back(out);
  }
```

注意:方向光 direction 需归一化(节点可能带缩放):push 前
`normalize` 处理(零向量回退 (0,1,0))。`cgltf_node_transform_world` 需要
`CGLTF_IMPLEMENTATION` 已在本文件 ✓。

- [ ] **Step 5: 跑测试**

Run: `./scripts/check.sh 2>&1 | tail -6`
Expected: 全绿(Gltf.PunctualLights 通过)

- [ ] **Step 6: Commit**

```bash
git add core/resource/gltf_loader.h core/resource/gltf_loader.cpp tests/resource/gltf_test.cpp
git commit -m "feat(resource): KHR_lights_punctual 解析(节点世界变换→方向/位置)"
```

---

### Task 4: renderer LightUBO + ShadowPass + 多光源 PBR + 阴影 golden

**Files:**
- Create: `core/renderer/light_ubo.h`(布局与填充辅助)
- Modify: `core/renderer/renderer.h`、`core/renderer/renderer.cpp`
- Modify: `core/renderer/renderable.h`(RenderContext 扩展)
- Modify: `core/renderer/mesh_renderable.cpp`(shadowPass 路径 + lightUbo/shadowMap 绑定)
- Modify: `core/renderer/quality.h`、`core/renderer/quality.cpp`(shadowMapSize)
- Modify: `shaders/pbr_forward.frag`(多光源+阴影)、`shaders/shadow_depth.vert`(双 UBO 化)
- Modify: `core/rhi/backends/gles/gles_device.cpp`(块名表加 ShadowUBO→0)
- Test: `tests/renderer/shadow_test.cpp`(新建)、`tests/renderer/quality_test.cpp`(PresetTable 追加断言)、`tests/CMakeLists.txt`(注册)
- Golden: 新增 `tests/golden/helmet_shadow_metal.png`、`helmet_shadow_vulkan.png`
- Modify(调用点): `tools/render_test/main.cpp`、`tests/renderer/pbr_test.cpp`、`box_render_test.cpp`、`renderer_test.cpp`、`quality_test.cpp`、`ktx2_render_test.cpp`(RendererShaderDesc 增员)

- [ ] **Step 1: 写失败测试**

`tests/renderer/shadow_test.cpp`:

```cpp
// 阴影 golden:helmet + 程序化地面 quad + 方向光(包围球取景)。
// LightUBO 布局 static_assert 也在此。
#include <gtest/gtest.h>
#include "common/image.h"
#include "common/shader_code.h"
#include "renderer/light_ubo.h"
#include "renderer/renderer.h"
#include "resource/gltf_loader.h"
#include "resource/mesh_render_resource.h"
#include "scene/camera.h"
#include "scene/scene.h"
#include "rhi/rhi_device.h"
#include "rd_shader_dir.h"
#include <cstdlib>
#include <glm/glm.hpp>

static_assert(sizeof(rd::LightUBOData) == 352, "LightUBO 必须 352B");

namespace {
constexpr uint32_t kW = 512, kH = 512;

/// 程序化地面(y 平面,2 三角形;48B 交错顶点)
rd::ModelAsset makeGround(float y, float half) {
  rd::ModelAsset m;
  rd::MeshData mesh;
  mesh.name = "ground";
  const float v[4][12] = {
      // pos          normal      tangent         uv
      {-half, y, -half, 0, 1, 0, 1, 0, 0, 1, 0, 0},
      { half, y, -half, 0, 1, 0, 1, 0, 0, 1, 1, 0},
      { half, y,  half, 0, 1, 0, 1, 0, 0, 1, 1, 1},
      {-half, y,  half, 0, 1, 0, 1, 0, 0, 1, 0, 1},
  };
  mesh.vertices.assign(&v[0][0], &v[0][0] + 4 * 12);
  const uint16_t idx[6] = {0, 1, 2, 0, 2, 3};
  mesh.indices.resize(sizeof(idx));
  memcpy(mesh.indices.data(), idx, sizeof(idx));
  mesh.indexType = rd::IndexType::UInt16;
  mesh.indexCount = 6;
  mesh.material.baseColorFactor[0] = 0.8f;
  mesh.material.baseColorFactor[1] = 0.8f;
  mesh.material.baseColorFactor[2] = 0.8f;
  mesh.material.baseColorFactor[3] = 1.0f;
  mesh.material.roughnessFactor = 0.9f;
  mesh.material.metallicFactor = 0.0f;
  m.meshes.push_back(std::move(mesh));
  m.boundingRadius = half * 1.5f;
  return m;
}

void runShadowGolden(rd::Backend b) {
  rd::DeviceDesc d;
  d.backend = b;
  auto device = rd::createDevice(d);
  ASSERT_NE(device, nullptr);
  auto load = [&](const char* n) { return rd::test::loadShaderCode(b, RD_SHADER_DIR, n); };
  auto unlitVs = load("unlit.vert"), unlitFs = load("unlit.frag");
  auto pbrVs = load("pbr_forward.vert"), pbrFs = load("pbr_forward.frag");
  auto pfVs = load("prefilter.vert"), pfFs = load("prefilter.frag");
  auto blitVs = load("blit.vert"), blitFs = load("blit.frag");
  auto sdVs = load("shadow_depth.vert"), sdFs = load("shadow_depth.frag");
  rd::Renderer renderer;
  rd::RendererShaderDesc sd{unlitVs.code, unlitFs.code, pbrVs.code, pbrFs.code,
                            pfVs.code,   pfFs.code,   blitVs.code, blitFs.code,
                            sdVs.code,   sdFs.code,   unlitVs.entry,
                            rd::Format::RGBA8_UNORM};
  rd::OffscreenTargetDesc td;
  td.width = kW;
  td.height = kH;
  td.depth = true;
  auto target = device->createOffscreenTarget(td);
  auto helmet = rd::loadGltf(RD_TEST_DATA_DIR "/assets/DamagedHelmet.glb");
  auto ground = makeGround(-0.55f, 1.6f);
  ASSERT_TRUE(target.valid() && helmet.valid());
  ASSERT_TRUE(renderer.init(*device, sd));
  auto helmetRes = rd::MeshRenderResource::upload(*device, helmet);
  auto groundRes = rd::MeshRenderResource::upload(*device, ground);
  ASSERT_NE(helmetRes, nullptr);
  ASSERT_NE(groundRes, nullptr);
  rd::scene::Scene scene;
  auto n1 = std::make_unique<rd::scene::MeshNode>();
  n1->mesh = helmetRes;
  scene.root().addChild(std::move(n1));
  auto n2 = std::make_unique<rd::scene::MeshNode>();
  n2->mesh = groundRes;
  scene.root().addChild(std::move(n2));

  // 方向光(斜上方)+ 阴影开(High 档 2048)
  rd::LightData light;
  light.type = rd::LightType::Directional;
  float dl = std::sqrt(0.5f * 0.5f + 0.8f * 0.8f + 0.3f * 0.3f);
  light.direction[0] = 0.5f / dl;
  light.direction[1] = 0.8f / dl;
  light.direction[2] = 0.3f / dl;
  light.color[0] = light.color[1] = light.color[2] = 3.0f;
  renderer.setLights({light});
  renderer.setQuality(rd::qualityPreset(rd::QualityTier::High));
  renderer.setLightFraming(helmet.boundingCenter, helmet.boundingRadius);

  rd::scene::Camera cam;
  const float dist = helmet.boundingRadius * 2.5f;
  glm::vec3 center(helmet.boundingCenter[0], helmet.boundingCenter[1],
                   helmet.boundingCenter[2]);
  glm::vec3 eye = center + glm::vec3(dist * 0.65f, dist * 0.35f, dist * 0.65f);
  cam.lookAt({eye.x, eye.y, eye.z}, {center.x, center.y, center.z}, {0, 1, 0});
  cam.setPerspective(0.78539816f, 1.0f, dist * 0.1f, dist * 10.0f);

  device->beginFrame();
  renderer.beginScene(cam, {0.05f, 0.05f, 0.06f, 1.0f});
  scene.collect(renderer);
  auto* cmd = device->acquireCommandBuffer();
  renderer.endScene(cmd, target);
  device->submit(cmd);
  device->waitIdle();
  device->endFrame();

  std::vector<uint8_t> px(size_t(kW) * kH * 4);
  ASSERT_TRUE(device->readbackTarget(target, px.data(), px.size()));
  helmetRes->destroy(*device);
  groundRes->destroy(*device);
  renderer.shutdown();
  const std::string name =
      b == rd::Backend::Metal ? "helmet_shadow_metal.png" : "helmet_shadow_vulkan.png";
  const std::string path = std::string(RD_TEST_DATA_DIR) + "/golden/" + name;
  if (std::getenv("RD_UPDATE_GOLDENS")) {
    ASSERT_TRUE(rd::test::savePNG(path, kW, kH, px.data()));
    return;
  }
  auto golden = rd::test::loadPNG(path);
  ASSERT_EQ(golden.pixels.size(), px.size()) << "golden 缺失: " << path;
  auto cmp = rd::test::compareRGBA8(px.data(), golden.pixels.data(), kW, kH, 3, 0.02);
  EXPECT_TRUE(cmp.pass) << "diffRatio=" << cmp.diffRatio;
}
} // namespace

TEST(Shadow, MetalGolden) {
#if defined(__APPLE__)
  runShadowGolden(rd::Backend::Metal);
#endif
}
TEST(Shadow, VulkanGolden) {
#if defined(RD_WITH_VULKAN)
  runShadowGolden(rd::Backend::Vulkan);
#endif
}
```

`tests/renderer/quality_test.cpp` 的 PresetTable 追加断言:

```cpp
  EXPECT_EQ(hi.shadowMapSize, 2048u);
  // Mid:
  EXPECT_EQ(mid.shadowMapSize, 1024u);
  // Low:
  EXPECT_EQ(low.shadowMapSize, 0u);
```

`tests/CMakeLists.txt` 追加 `renderer/shadow_test.cpp`。

- [ ] **Step 2: 跑测试确认失败**

Run: `./scripts/check.sh 2>&1 | tail -5`
Expected: 编译错误(`renderer/light_ubo.h` 不存在 / RendererShaderDesc 字段不匹配)

- [ ] **Step 3: light_ubo.h(布局 + 填充)**

`core/renderer/light_ubo.h`:

```cpp
/**
 * @file light_ubo.h
 * @brief LightUBO(slot2,352B)布局与填充:多光源数组 + 阴影参数 + 光源变换。
 * UBO 步进/对齐遵循三后端最小公倍 256B 约定之外的独立块(单独 slot)。
 */
#pragma once
#include "resource/gltf_loader.h"  // LightData
#include "foundation/math.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace rd {

/// LightUBO 布局(352B):lightViewProj|shadowParams|lightCount|lights[4×64B]
struct LightUBOData {
  math::Mat4 lightViewProj;
  float shadowParams[4];   // x=bias, y=1/shadowMapSize, z=shadowOn, w=vFlip(GLES=1)
  float lightCount[4];     // x=count
  struct LightSlot {
    float dirType[4];      // xyz=指向光源方向, w=type(0=dir,1=point,2=spot)
    float posRange[4];     // xyz=位置, w=range(0=无限)
    float color[4];        // rgb=color×intensity
    float spot[4];         // x=innerCos, y=outerCos
  } lights[4];
};
static_assert(sizeof(LightUBOData) == 352, "LightUBO 必须 352B");

/// 填充 LightUBO:lights 超 4 截断;首盏方向光作为阴影投射者。
inline void fillLightUBO(LightUBOData& out, const std::vector<LightData>& lights,
                         const math::Mat4& lightViewProj, float shadowTexel,
                         bool shadowOn, bool vFlip) {
  out.lightViewProj = lightViewProj;
  out.shadowParams[0] = 0.0015f;
  out.shadowParams[1] = shadowTexel;
  out.shadowParams[2] = shadowOn ? 1.0f : 0.0f;
  out.shadowParams[3] = vFlip ? 1.0f : 0.0f;
  const uint32_t n = std::min<uint32_t>(uint32_t(lights.size()), 4);
  out.lightCount[0] = float(n);
  out.lightCount[1] = out.lightCount[2] = out.lightCount[3] = 0.0f;
  for (uint32_t i = 0; i < 4; ++i) {
    auto& s = out.lights[i];
    memset(&s, 0, sizeof(s));
    if (i >= n) continue;
    const LightData& l = lights[i];
    s.dirType[0] = l.direction[0];
    s.dirType[1] = l.direction[1];
    s.dirType[2] = l.direction[2];
    s.dirType[3] = float(l.type == LightType::Directional ? 0
                     : l.type == LightType::Point       ? 1
                                                        : 2);
    s.posRange[0] = l.position[0];
    s.posRange[1] = l.position[1];
    s.posRange[2] = l.position[2];
    s.posRange[3] = l.range;
    s.color[0] = l.color[0];
    s.color[1] = l.color[1];
    s.color[2] = l.color[2];
    s.spot[0] = std::cos(l.innerCone);
    s.spot[1] = std::cos(l.outerCone);
  }
}

/// 包围球 + 首盏方向光 → lightViewProj(正交,DEPTH_ZERO_TO_ONE)。
inline math::Mat4 makeLightViewProj(const LightData& dirLight, const float center[3],
                                    float radius) {
  math::Vec3 L(dirLight.direction[0], dirLight.direction[1], dirLight.direction[2]);
  if (glm::dot(L, L) < 1e-6f) L = math::Vec3(0, 1, 0);
  L = glm::normalize(L);
  const math::Vec3 c(center[0], center[1], center[2]);
  const math::Vec3 eye = c + L * (radius * 2.0f);
  const math::Mat4 view = math::lookAt(eye, c, math::Vec3(0, 1, 0));
  const float r = radius * 1.2f;
  const math::Mat4 proj =
      glm::ortho(-r, r, -r, r, radius * 0.1f, radius * 4.0f);
  return proj * view;
}

} // namespace rd
```

(light_ubo.h 需 `#include <cstring>` 供 memset;`glm::ortho` 经 foundation/math.h
引入 glm 且全局 GLM_FORCE_DEPTH_ZERO_TO_ONE ✓)

- [ ] **Step 4: quality.h/cpp 加 shadowMapSize**

`quality.h` QualityPreset 追加字段:`uint32_t shadowMapSize;  ///< 0=关阴影`
`quality.cpp` 预设:High `{1.0f, 4, 256, 6, 4096, 2048}`、Mid `{0.75f, 2, 128, 5, 2048, 1024}`、
Low `{0.5f, 1, 64, 4, 1024, 0}`;默认(现状等价)`{1.0f, 1, 64, 5, 4096, 0}`。

- [ ] **Step 5: shadow_depth.vert 双 UBO 化**

`shaders/shadow_depth.vert` 改为:

```glsl
// shadow_depth.vert:顶点 → 光源空间(ShadowUBO=lightViewProj,ItemUBO=world)。
#version 450
layout(location = 0) in vec3 aPos;
layout(binding = 0) uniform ShadowUBO { mat4 lightViewProj; } u;
layout(binding = 1) uniform ItemUBO {
  mat4 mvp;
  mat4 world;
  mat4 normalMatrix;
  vec4 baseColorFactor;
  vec4 emissiveOcclusion;
  vec4 metallicRoughness;
  vec4 uvTransform;
} item;
void main() { gl_Position = u.lightViewProj * item.world * vec4(aPos, 1.0); }
```

`tests/rhi/shadow_target_test.cpp` 同步:pass1 增绑 slot1(单位矩阵 ItemUBO 布局
256B 缓冲:mvp/world/normalMatrix 单位 + factors):

```cpp
  // ItemUBO(identity):256B——mvp|world|normalMatrix 单位阵,因子区填 1
  float itemData[64] = {};
  for (int m = 0; m < 3; ++m)
    for (int k = 0; k < 16; ++k) itemData[m * 16 + k] = (k % 5 == 0) ? 1.0f : 0.0f;
  itemData[48] = itemData[49] = itemData[50] = itemData[51] = 1.0f;  // baseColorFactor
  auto itemUbo = dev->createBuffer({256, rd::BufferUsage::Uniform, true, false, nullptr});
  dev->updateBuffer(itemUbo, itemData, 256, 0);
```

pass1 draw 前:`cmd->bindUniformBuffer(1, itemUbo, 0, 256);`(slot0 的 mvp 绑定义务
改为传 lightViewProj=单位——shadow_depth.vert 现在读 u.lightViewProj×world,
两个单位阵等价)。注意 shadow_depth.vert 的 ShadowUBO 只剩 lightViewProj:
**测试里 slot0 绑 64B 单位矩阵即可**(shader 只读前 64B)。

- [ ] **Step 6: RenderContext 扩展 + MeshRenderable**

`core/renderer/renderable.h` 的 RenderContext 追加:

```cpp
  BufferHandle lightUbo;       ///< slot2:LightUBO(352B,多光源+阴影参数)
  TextureHandle shadowMap;     ///< slot7:阴影深度图(无效=无阴影路径)
  SamplerHandle shadowSampler; ///< 比较采样器
  PipelineHandle shadowPipe;   ///< shadowPass 时使用的深度管线
  bool shadowPass = false;     ///< true=只写深度(shadow_depth.vert)
```

`core/renderer/mesh_renderable.cpp` 的 record 开头加 shadowPass 分支:

```cpp
void MeshRenderable::record(CommandBuffer* cmd, const RenderContext& ctx) {
  if (ctx.shadowPass) {  // 阴影:只写深度(位置语义)
    cmd->bindPipeline(ctx.shadowPipe);
    cmd->bindUniformBuffer(0, ctx.lightUbo, 0, 64);          // lightViewProj
    cmd->bindUniformBuffer(1, ctx.itemUbo, ctx.itemOffset, 256);
    for (const auto& g : mesh_->meshes()) {
      cmd->bindVertexBuffer(0, g.vbo, 0);
      cmd->bindIndexBuffer(g.ibo, 0, g.indexType);
      cmd->drawIndexed(g.indexCount, 0, 0);
    }
    return;
  }
  // 现有路径;并追加:
  //   cmd->bindUniformBuffer(2, ctx.lightUbo, 0, 352);
  //   if (ctx.shadowMap.valid()) cmd->bindTexture(7, ctx.shadowMap, ctx.shadowSampler);
  //   无阴影时绑 fallback 深度纹理(由 Renderer 保证 shadowMap 恒有效)
  ...
}
```

(pbr 分支绑定 slot2/slot7;unlit 分支不绑。注意 Vulkan 描述符按绑定状态缓存,
绑定的集合差异会正确物化。)

- [ ] **Step 7: pbr_forward.frag 多光源 + 阴影**

替换"1 方向光"段(line 99-103 区域)与 UBO/采样器声明:

```glsl
layout(binding = 2) uniform LightUBO {
  mat4 lightViewProj;
  vec4 shadowParams;   // x=bias, y=1/size, z=shadowOn, w=vFlip
  vec4 lightCount;     // x=count
  vec4 lights[16];     // 4 盏 × 4 vec4(dirType|posRange|color|spot)
};
layout(binding = 11) uniform sampler2DShadow texShadow;  // texture slot 7 → binding 11
```

main 内 direct 计算替换为:

```glsl
  // 多光源 direct(首盏方向光投影阴影)
  vec3 direct = vec3(0.0);
  const int nLights = min(int(lightCount.x + 0.5), 4);
  for (int i = 0; i < nLights; ++i) {
    vec4 dirType = lights[i * 4 + 0];
    vec4 posRange = lights[i * 4 + 1];
    vec3 lcolor = lights[i * 4 + 2].rgb;
    vec4 spot = lights[i * 4 + 3];
    int type = int(dirType.w + 0.5);
    vec3 L;
    float att = 1.0;
    if (type == 0) {
      L = normalize(dirType.xyz);
    } else {
      vec3 toL = posRange.xyz - vWorldPos;
      float dist = length(toL);
      L = toL / max(dist, 1e-4);
      if (posRange.w > 0.0) {
        float t = clamp(1.0 - dist / posRange.w, 0.0, 1.0);
        att = t * t;
      }
      if (type == 2) {
        float cd = dot(-L, normalize(dirType.xyz));
        float t = clamp((cd - spot.y) / max(spot.x - spot.y, 1e-4), 0.0, 1.0);
        att *= t * t;
      }
    }
    float ndl = clamp(dot(n, L), 0.0, 1.0);
    vec3 term = lcolor * att * ndl *
                (baseColor.rgb * (1.0 - metallic) / PI + ggxSpec(n, L, v, roughness, f0));
    if (i == 0 && type == 0) {
      // 阴影:PCF 3x3(bias 随 ndl 坡度放大)
      float shadow = 1.0;
      if (shadowParams.z > 0.5) {
        vec4 lp = lightViewProj * vec4(vWorldPos, 1.0);
        vec3 ndc = lp.xyz / lp.w;
        vec2 suv;
        suv.x = ndc.x * 0.5 + 0.5;
        suv.y = shadowParams.w > 0.5 ? ndc.y * 0.5 + 0.5 : 0.5 - ndc.y * 0.5;
        float bias = max(shadowParams.x * (1.0 - ndl), shadowParams.x * 0.2);
        float refZ = ndc.z - bias;
        if (suv.x >= 0.0 && suv.x <= 1.0 && suv.y >= 0.0 && suv.y <= 1.0) {
          float sum = 0.0;
          for (int x = -1; x <= 1; ++x)
            for (int y = -1; y <= 1; ++y)
              sum += texture(texShadow,
                             vec3(suv + vec2(float(x), float(y)) * shadowParams.y, refZ));
          shadow = sum / 9.0;
        }
      }
      term *= shadow;
    }
    direct += term;
  }
```

(删除原 `vec3 l = normalize(lightDir.xyz); ... vec3 direct = ...` 段;FrameUBO 的
lightDir/lightColor 字段保留不动——多光源后由 LightUBO 接管,FrameUBO 字段留作占位。)

- [ ] **Step 8: Renderer 集成**

`renderer.h`:
1. RendererShaderDesc 追加:`std::vector<uint8_t> shadowVs, shadowFs;`(blitFs 后)。
2. 公开方法:

```cpp
  /// 设置光源(≤4;空 → 默认 1 方向光);下一帧生效。
  void setLights(const std::vector<LightData>& lights);
  /// 阴影取景(模型包围球);光源方向取首盏方向光。
  void setLightFraming(const float center[3], float radius);
  /// 手动开关(画质档 shadowMapSize=0 时强制关)。
  void setShadowEnabled(bool on) { shadowManual_ = on; }
```

3. 成员:`BufferHandle lightUbo_; PipelineHandle shadowPipeline_; TextureHandle shadowDepthTex_; TargetHandle shadowTarget_; SamplerHandle shadowSampler_; TextureHandle shadowFallbackTex_; std::vector<LightData> lights_; float framingCenter_[3] = {}; float framingRadius_ = 1.0f; bool shadowManual_ = true; uint32_t shadowMapSize_ = 0; uint32_t shadowTargetSize_ = 0;`

`renderer.cpp`:
1. init:创建 lightUbo_(352B,hostWrite)、shadowPipeline_(depthOnly + depthTest/Write +
   48B 顶点布局)、shadowSampler_(compareEnable + Clamp)、shadowFallbackTex_(1x1 D32,
   data=1.0f);RendererShaderDesc 增员接线。
2. setQuality:追加 `shadowMapSize_ = q.shadowMapSize;`(重建在 ensureShadowTarget)。
3. setLights/setLightFraming 存状态。
4. ensureShadowTarget():shadowMapSize_>0 且尺寸变化时创建 shadowDepthTex_(D32 RT)
   + shadowTarget_(depthFromTexture)。
5. endScene:ItemUBO 填充后,填 LightUBO 并 updateBuffer;阴影激活
   (shadowManual_ && shadowMapSize_>0 && 有方向光 && ensureShadowTarget 成功)时:

```cpp
  cmd->beginRenderPass(shadowTarget_, {0, 0, 0, 1, 1.0f});
  RenderContext sctx;
  sctx.shadowPass = true;
  sctx.lightUbo = lightUbo_;
  sctx.itemUbo = itemUbo_;
  sctx.shadowPipe = shadowPipeline_;
  for (uint32_t i = 0; i < count; ++i) {
    sctx.itemOffset = uint64_t(i) * kUboStride;
    queue_[i]->record(cmd, sctx);
  }
  cmd->endRenderPass();
```

   场景 pass 的 ctx 追加 `ctx.lightUbo = lightUbo_; ctx.shadowMap = shadowMap 或 fallback; ctx.shadowSampler = shadowSampler_;`。
6. LightUBO 填充:

```cpp
  LightUBOData lu{};
  const LightData* dirLight = nullptr;
  for (const auto& l : lights_)
    if (l.type == LightType::Directional) { dirLight = &l; break; }
  math::Mat4 lvp{1.0f};
  if (dirLight) lvp = makeLightViewProj(*dirLight, framingCenter_, framingRadius_);
  fillLightUBO(lu, lights_, lvp,
               shadowMapSize_ ? 1.0f / float(shadowMapSize_) : 0.0f, shadowActive,
               dev_->backend() == Backend::GLES);
  dev_->updateBuffer(lightUbo_, &lu, sizeof(lu), 0);
```

   默认无灯时(lights_ 空)填 1 盏方向光(与 FrameUBO 默认同向 (-0.5,0.8,0.3),
   color=(0.8,0.75,0.7)——保持现状光照)再 fillLightUBO。
7. shutdown:新增资源全部销毁。

- [ ] **Step 9: 调用点适配(6 处)**

`render_test/main.cpp`、`tests/renderer/{pbr_test,box_render_test,renderer_test,quality_test,ktx2_render_test}.cpp`:
加载 `shadow_depth.vert/frag` 并插入 RendererShaderDesc 聚合(blitFs 之后):
`sdVs.code, sdFs.code,`。`core/api/rd_api.cpp` 的 get 链追加
`get("shadow_depth", Vertex/Fragment, sd.shadowVs/shadowFs)`(Task 5 用到,现在先加)。

- [ ] **Step 10: 跑测试 + 生成阴影 golden**

Run: `./scripts/check.sh 2>&1 | tail -8`
Expected: 编译通过;Shadow.*Golden 报 golden 缺失;**PbrHelmet/BoxPbr 等既有 golden 注意
可能微差**(多光源循环后默认 1 方向光应与原直接光一致——若 diffRatio 超容差,
检查默认灯参数与原 FrameUBO 光照是否一致)
Run: `RD_UPDATE_GOLDENS=1 ctest --test-dir build --output-on-failure -R "Shadow.*Golden"`
**像素核对** `tests/golden/helmet_shadow_{metal,vulkan}.png`:地面有阴影、helmet 受光正常、
无双后端结构差。再 `./scripts/check.sh 2>&1 | tail -6` 全绿。

- [ ] **Step 11: Commit**

```bash
git add core shaders tests tools/render_test
git commit -m "feat(renderer): LightUBO 多光源 + ShadowPass(PCF 3x3)+ 阴影 golden 双后端"
```

---

### Task 5: C API 灯光/阴影 + engine 集成 + 画质联动验证

**Files:**
- Modify: `core/api/rd_api.h`、`core/api/rd_api.cpp`
- Test: `tests/api/api_test.cpp`(追加)

- [ ] **Step 1: 写失败测试**

`tests/api/api_test.cpp` 追加:

```cpp
// 灯光/阴影 API:增删与开关安全;空引擎不崩
TEST(Api, LightsAndShadowSafe) {
  rd_engine* e = rd_engine_create(RD_BACKEND_METAL);
  ASSERT_NE(e, nullptr);
  rd_engine_clear_lights(e);
  rd_engine_add_dir_light(e, 0.5f, 0.8f, 0.3f, 1, 1, 1, 2.0f);
  rd_engine_add_point_light(e, 1, 2, 3, 10.0f, 1, 0.5f, 0.25f, 4.0f);
  rd_engine_add_spot_light(e, 0, 1, 0, 0, -1, 0, 20.0f, 40.0f, 5.0f, 1, 1, 1, 1.0f);
  rd_engine_set_shadow_enabled(e, 1);
  rd_engine_render_frame(e, 0.016f);  // 无 surface 安全
  rd_engine_clear_lights(e);
  rd_engine_set_shadow_enabled(e, 0);
  rd_engine_destroy(e);
  rd_engine_add_dir_light(nullptr, 0, 1, 0, 1, 1, 1, 1);  // 不崩
  rd_engine_set_shadow_enabled(nullptr, 1);
}
```

- [ ] **Step 2: 跑测试确认失败**

Run: `./scripts/check.sh 2>&1 | tail -5`
Expected: 编译错误(API 未声明)

- [ ] **Step 3: rd_api.h**

追加:

```c
/// 清空全部手动灯光(清空后回落 glTF 灯/默认灯)。
void rd_engine_clear_lights(rd_engine* engine);
/// 加方向光(dir=指向光源方向,无需归一化;color×intensity)。
void rd_engine_add_dir_light(rd_engine* engine, float dx, float dy, float dz,
                             float r, float g, float b, float intensity);
/// 加点光(range≤0 视为无限)。
void rd_engine_add_point_light(rd_engine* engine, float px, float py, float pz,
                               float range, float r, float g, float b, float intensity);
/// 加聚光灯(内外锥角单位:度)。
void rd_engine_add_spot_light(rd_engine* engine, float px, float py, float pz,
                              float dx, float dy, float dz, float innerDeg,
                              float outerDeg, float range, float r, float g, float b,
                              float intensity);
/// 阴影总开关(默认 1;Low 画质档自动关,与本开关为与关系)。
void rd_engine_set_shadow_enabled(rd_engine* engine, int enabled);
```

- [ ] **Step 4: rd_api.cpp**

1. 结构体追加:

```cpp
  std::vector<rd::LightData> manualLights;  ///< C API 灯(非空则覆盖 glTF 灯)
  bool shadowEnabled = true;
  bool lightsDirty = false;
```

2. API 实现(空引擎安全;每次增删置 lightsDirty):

```cpp
void rd_engine_clear_lights(rd_engine* e) {
  if (!e) return;
  e->manualLights.clear();
  e->lightsDirty = true;
}
void rd_engine_add_dir_light(rd_engine* e, float dx, float dy, float dz, float r,
                             float g, float b, float intensity) {
  if (!e) return;
  rd::LightData l;
  l.type = rd::LightType::Directional;
  l.direction[0] = dx; l.direction[1] = dy; l.direction[2] = dz;
  l.color[0] = r * intensity; l.color[1] = g * intensity; l.color[2] = b * intensity;
  e->manualLights.push_back(l);
  e->lightsDirty = true;
}
// point/spot 同理(point: position/range;spot: 加 direction/innerCone/outerCone 弧度转换:
//   l.innerCone = innerDeg * 0.0174532925f;)
void rd_engine_set_shadow_enabled(rd_engine* e, int en) {
  if (!e) return;
  e->shadowEnabled = en != 0;
  if (e->rendererReady) e->renderer.setShadowEnabled(e->shadowEnabled);
}
```

3. render_frame:beginScene 前,`if (e->lightsDirty) { e->renderer.setLights(e->activeLights()); e->lightsDirty=false; }`;
   辅助 `activeLights()`:`manualLights 非空 ? manualLights : gltfLights`(gltfLights 成员,
   load_gltf 时存 model.lights;两者皆空 → 空 vector,Renderer 默认灯兜底)。
4. load_gltf 成功路径追加:

```cpp
  e->gltfLights = model.lights;
  e->lightsDirty = true;
  e->renderer.setLightFraming(model.boundingCenter, model.boundingRadius);
```

   (rendererReady 与否都调——setLightFraming/setLights 只存 CPU 状态,安全)
5. set_shadow_enabled 的初始应用:applyQuality 后同步一次
   `e->renderer.setShadowEnabled(e->shadowEnabled)`。

- [ ] **Step 5: 跑测试**

Run: `./scripts/check.sh 2>&1 | tail -6`
Expected: 全绿(Api.LightsAndShadowSafe 通过)

- [ ] **Step 6: Commit**

```bash
git add core/api tests/api
git commit -m "feat(api): 灯光 C API(dir/point/spot)+ 阴影开关 + engine 集成(手动/glTF 双通道)"
```

---

### Task 6: AGENTS.md 收尾 + 全量回归

**Files:**
- Modify: `AGENTS.md`

- [ ] **Step 1: AGENTS.md 更新**

- 「当前状态」追加:

```markdown
- P2-1 完成:单方向光阴影(depth-only 目标 + 硬件比较 + PCF 3x3,包围球自动取景)
  + KHR_lights_punctual 多光源(dir/point/spot×4,glTF 解析/C API 双通道)
  + 画质档 shadowMapSize(2048/1024/0)
- 下一步:P2 余下(后处理链/骨骼动画/拾取/性能基准)
```

- 「代码约定」追加:

```markdown
- LightUBO=slot2(352B:lightViewProj|shadowParams|lightCount|lights[4×64B]);
  GLES 块名 LightUBO→2、ShadowUBO→0;阴影纹理=slot 7(比较采样器 sampler2DShadow)
- 阴影:ShadowPass 在场景 pass 前(endScene 内);depth-only 目标
  (OffscreenTargetDesc.depthFromTexture + PipelineDesc.depthOnly);
  bias 走 shader(常量+slope);GLES 阴影 UV 的 v 翻转由 shadowParams.w 吸收
- 灯光约定:direction=指向光源(dot(N,L) 直接用);color 已乘 intensity;
  手动灯(C API)非空覆盖 glTF 灯,皆空则默认 1 方向光
- 画质:QualityPreset.shadowMapSize(0=关);rd_engine_set_shadow_enabled 与画质档为与关系
```

- [ ] **Step 2: 全量回归**

Run: `./scripts/check.sh 2>&1 | tail -6`
Expected: 全绿
Run: `source /tmp/rd_env.sh && export RD_DEPS_MIRROR=$HOME/.rd_deps_mirror && cd samples/android && ./gradlew :app:assembleDebug 2>&1 | tail -2`
Expected: BUILD SUCCESSFUL
Run: `cmake --build build-ios --config Debug 2>&1 | tail -2`
Expected: BUILD SUCCEEDED
手动:`./build/tools/render_test/render_test --interactive --model tests/assets/DamagedHelmet.glb`
确认阴影与旋转/缩放交互正常。

- [ ] **Step 3: Commit**

```bash
git add AGENTS.md
git commit -m "docs: P2-1 收尾(AGENTS.md 新约定)"
```

---

## 附:已知风险与决策记录

| 风险/决策 | 处理 |
|---|---|
| 多光源只首盏方向光投影 | v1 简化;点/聚光阴影归 P2 后续(需 cube/多 target) |
| FrameUBO 的 lightDir/lightColor 字段 | 保留占位(256B 布局不动);多光源后由 LightUBO 接管 |
| 默认灯(无 glTF/手动灯) | 与 2b/2c 现状一致((-0.5,0.8,0.3)/(0.8,0.75,0.7)),golden 不回归 |
| GLES 无 host 覆盖 | shadow 采样契约仅 Metal/Vulkan;GLES 随 Android demo/模拟器验证 |
| 阴影 acne/条纹 | bias=0.0015 常量+slope;golden 容差 3/0.02;视觉核对为准 |
| LightUBO 352B 超 GLES 块保证 | ES3 保证 16KB UBO,安全 |

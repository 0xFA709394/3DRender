# 阶段二 a:框架骨架 + 深度 + glTF unlit 渲染打通 实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 按 `docs/superpowers/specs/2026-08-13-renderer-framework-skeleton-design.md` 建 renderer/scene/resource 三层骨架，RHI 落地离屏深度附件，打通 glTF 静态模型 unlit 渲染（BoxTextured golden 双后端）。

**Architecture:** GGUI 五元组移动版：Renderer（立即模式 render queue + pass 序列）/ scene（保留模式 Node 树 + Camera)/ resource（持久持有 GPU 重资产）。MeshRenderable 只持引用；帧级配置对象由阶段一退休队列兜底。

**Tech Stack:** C++17、cgltf v1.14(FetchContent)、stb、googletest、glm(GLM_FORCE_DEPTH_ZERO_TO_ONE)、阶段一强化后的 RHI(Vulkan/Metal/GLES)。

**通用约定：**
- 构建+测试：`./scripts/check.sh`；只跑新增测试 `ctest --test-dir build --output-on-failure -R <正则>`
- 更新 golden:`RD_UPDATE_GOLDENS=1 ctest --test-dir build -R <用例>` 后目视核对 `tests/golden/*.png` 再提交
- host 可测后端：Metal + Vulkan(MoltenVK);GLES 经 Android 构建验证：`source /tmp/rd_env.sh && cd samples/android && ./gradlew :app:assembleDebug`
- commit 规范：`<type>(<scope>): 描述`
- 测试资产已在库内：`tests/assets/BoxTextured.glb`(1 mesh)、`tests/assets/DamagedHelmet.glb`(多 mesh/32 位索引）
- 顶点布局约定：**pos(3f)@0 | normal(3f)@12 | uv(2f)@24，交错 stride 32**(normal 为 2b 备）

---

### Task 1: RHI 离屏深度附件三后端 + 深度契约测试

**Files:**
- Modify: `core/rhi/rhi_types.h`(DepthCompareOp + ClearColor.depth + PipelineDesc.depthCompare)
- Modify: `core/rhi/backends/vulkan/vulkan_device.cpp`（双 render pass + 深度附件 + 管线深度状态）
- Modify: `core/rhi/backends/metal/metal_device.mm`（深度纹理附件 + MTLDepthStencilState)
- Modify: `core/rhi/backends/gles/gles_device.cpp`（深度 renderbuffer + GL 深度状态）
- Create: `shaders/depth.vert`、`shaders/depth.frag`
- Modify: `shaders/CMakeLists.txt`
- Test: `tests/rhi/depth_test.cpp`（新）
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: 新 shader + 失败测试**

`shaders/depth.vert`（顶点色直通，z 由顶点数据直接给 NDC 深度）:

```glsl
// depth.vert:深度遮挡测试——pos 直接作为裁剪空间坐标(z 即 NDC 深度)。
// location0=pos(vec3);location1=color(vec3) → vColor。
#version 450
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aColor;
layout(location = 0) out vec3 vColor;
void main() { gl_Position = vec4(aPos, 1.0); vColor = aColor; }
```

`shaders/depth.frag`:

```glsl
#version 450
layout(location = 0) in vec3 vColor;
layout(location = 0) out vec4 outColor;
void main() { outColor = vec4(vColor, 1.0); }
```

`shaders/CMakeLists.txt` 追加两行：`rd_compile_shader(depth.vert)` / `rd_compile_shader(depth.frag)`。

`tests/rhi/depth_test.cpp`:

```cpp
// 深度附件契约测试:两个同位置交叠四边形,绿(z=0.2)先画、红(z=0.5)后画。
// depthTest 开(Less,清屏深度 1.0)→ 中心为绿(近者胜);
// depthTest 关 → 中心为红(后画覆盖)。Metal/Vulkan 双后端。
#include <gtest/gtest.h>
#include "common/image.h"
#include "common/shader_code.h"
#include "rhi/rhi_device.h"
#include "rd_shader_dir.h"

namespace {
constexpr uint32_t kW = 128, kH = 128;

// 两个覆盖全屏的四边形(各 2 三角形):pos xyz + color rgb,stride 24
const float kGreenQuad[] = {  // z=0.2,绿色
    -0.5f, -0.5f, 0.2f, 0, 1, 0,   0.5f, -0.5f, 0.2f, 0, 1, 0,   0.5f, 0.5f, 0.2f, 0, 1, 0,
    -0.5f, -0.5f, 0.2f, 0, 1, 0,   0.5f, 0.5f, 0.2f, 0, 1, 0,   -0.5f, 0.5f, 0.2f, 0, 1, 0,
};
const float kRedQuad[] = {    // z=0.5,红色
    -0.5f, -0.5f, 0.5f, 1, 0, 0,   0.5f, -0.5f, 0.5f, 1, 0, 0,   0.5f, 0.5f, 0.5f, 1, 0, 0,
    -0.5f, -0.5f, 0.5f, 1, 0, 0,   0.5f, 0.5f, 0.5f, 1, 0, 0,   -0.5f, 0.5f, 0.5f, 1, 0, 0,
};

rd::test::Image render(rd::Backend b, bool depthTest) {
  rd::DeviceDesc d;
  d.backend = b;
  auto device = rd::createDevice(d);
  if (!device) return {};
  auto vs = rd::test::loadShaderCode(b, RD_SHADER_DIR, "depth.vert");
  auto fs = rd::test::loadShaderCode(b, RD_SHADER_DIR, "depth.frag");
  auto vsm = device->createShaderModule({rd::ShaderStage::Vertex, vs.code, vs.entry});
  auto fsm = device->createShaderModule({rd::ShaderStage::Fragment, fs.code, fs.entry});
  auto vboG = device->createBuffer({sizeof(kGreenQuad), rd::BufferUsage::Vertex, false, false, kGreenQuad});
  auto vboR = device->createBuffer({sizeof(kRedQuad), rd::BufferUsage::Vertex, false, false, kRedQuad});

  rd::PipelineDesc pd;
  pd.vertexShader = vsm;
  pd.fragmentShader = fsm;
  pd.vertexBindings = {{0, 24}};
  pd.attributes = {{0, rd::Format::R32G32B32_FLOAT, 0, 0},
                   {1, rd::Format::R32G32B32_FLOAT, 12, 0}};
  pd.depthTest = depthTest;
  pd.depthWrite = depthTest;
  auto pipeline = device->createPipeline(pd);

  rd::OffscreenTargetDesc td;
  td.width = kW;
  td.height = kH;
  td.depth = true;
  auto target = device->createOffscreenTarget(td);
  if (!pipeline.valid() || !target.valid()) return {};

  auto* cmd = device->acquireCommandBuffer();
  rd::ClearColor clear{0.0f, 0.0f, 0.0f, 1.0f};  // depth 默认 1.0
  cmd->beginRenderPass(target, clear);
  cmd->bindPipeline(pipeline);
  cmd->bindVertexBuffer(0, vboG, 0);
  cmd->draw(6, 0);  // 绿先画(近,z=0.2)
  cmd->bindVertexBuffer(0, vboR, 0);
  cmd->draw(6, 0);  // 红后画(远,z=0.5)
  cmd->endRenderPass();
  device->submit(cmd);
  device->waitIdle();

  rd::test::Image img;
  img.width = kW;
  img.height = kH;
  img.pixels.resize(kW * kH * 4);
  if (!device->readbackTarget(target, img.pixels.data(), img.pixels.size())) return {};
  return img;
}

// 中心像素是否绿色为主
bool isGreen(const rd::test::Image& img) {
  size_t i = ((kH / 2) * kW + kW / 2) * 4;
  return img.pixels[i + 1] > 150 && img.pixels[i] < 60;
}
bool isRed(const rd::test::Image& img) {
  size_t i = ((kH / 2) * kW + kW / 2) * 4;
  return img.pixels[i] > 150 && img.pixels[i + 1] < 60;
}

void runCase(rd::Backend b) {
  auto onImg = render(b, true);
  ASSERT_EQ(onImg.pixels.size(), kW * kH * 4);
  EXPECT_TRUE(isGreen(onImg));   // 深度开:近者(绿)胜
  auto offImg = render(b, false);
  ASSERT_EQ(offImg.pixels.size(), kW * kH * 4);
  EXPECT_TRUE(isRed(offImg));    // 深度关:后画(红)覆盖
}
} // namespace

TEST(Depth, MetalOcclusion) {
#if defined(__APPLE__)
  runCase(rd::Backend::Metal);
#endif
}

TEST(Depth, VulkanOcclusion) {
#if defined(RD_WITH_VULKAN)
  runCase(rd::Backend::Vulkan);
#endif
}
```

`tests/CMakeLists.txt` 的 rd_tests 列表加 `rhi/depth_test.cpp`。

- [ ] **Step 2: 跑测试确认失败**

Run: `cmake -S . -B build > /dev/null && cmake --build build -j8 2>&1 | tail -3`
Expected: 编译通过但测试失败（`depthTest=true` 当前被后端拒绝，pipeline 无效 → 图像全空，isGreen 断言失败）

Run: `ctest --test-dir build --output-on-failure -R Depth`
Expected: 2 个用例 FAILED

- [ ] **Step 3: `rhi_types.h` 扩展**

`PrimitiveTopology` 之后加：

```cpp
/// 深度比较方式(Reverse-Z 预留:Greater + clearDepth 0.0 + 投影 near/far 对调)。
enum class DepthCompareOp { Less, Greater };
```

`ClearColor` 改为：

```cpp
/// 清屏值（RGBA + 深度;默认不透明黑 + 远平面 1.0）。
struct ClearColor {
  float r = 0, g = 0, b = 0, a = 1;
  float depth = 1.0f;  ///< 深度清屏值;Reverse-Z 场景传 0.0
};
```

`PipelineDesc` 的 depth 字段改为：

```cpp
  bool depthTest = false;               ///< 深度测试(须配 depth=true 的渲染目标)
  bool depthWrite = false;              ///< 深度写入
  DepthCompareOp depthCompare = DepthCompareOp::Less;  ///< 深度比较(Reverse-Z 用 Greater)
```

注意同步删掉三后端 createPipeline 里"深度附件 P1 引入"的拒绝守卫（本任务落地）。

- [ ] **Step 4: Vulkan 实现（双 render pass + 深度附件）**

a) `createRenderPass` 改为支持深度（原函数替换）:

```cpp
/// render pass 参数化创建:单颜色附件,可选 D32 深度附件。
/// 颜色 finalLayout=TRANSFER_SRC(readback 拷贝);深度 storeOp=DONT_CARE。
bool VulkanDevice::createRenderPass(VkFormat format, bool withDepth, VkRenderPass& out) {
  VkAttachmentDescription color{};
  color.format = format;
  color.samples = VK_SAMPLE_COUNT_1_BIT;
  color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  color.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

  VkAttachmentDescription depth{};
  depth.format = VK_FORMAT_D32_SFLOAT;
  depth.samples = VK_SAMPLE_COUNT_1_BIT;
  depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  depth.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  depth.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  depth.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

  VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkAttachmentReference depthRef{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
  VkSubpassDescription subpass{};
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount = 1;
  subpass.pColorAttachments = &colorRef;
  if (withDepth) subpass.pDepthStencilAttachment = &depthRef;

  VkSubpassDependency deps[2]{};
  deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
  deps[0].dstSubpass = 0;
  deps[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  deps[1].srcSubpass = 0;
  deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
  deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  deps[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
  deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  deps[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

  VkAttachmentDescription attachments[2] = {color, depth};
  VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
  rpci.attachmentCount = withDepth ? 2 : 1;
  rpci.pAttachments = attachments;
  rpci.subpassCount = 1;
  rpci.pSubpasses = &subpass;
  rpci.dependencyCount = 2;
  rpci.pDependencies = deps;
  VK_CHECK(vkCreateRenderPass(device_, &rpci, nullptr, &out));
  renderPassFormat_ = format;
  return true;
}
```

类成员：`renderPass_` 保留（无深度）+ 新增 `VkRenderPass renderPassDepth_ = VK_NULL_HANDLE;` 与访问器 `renderPassDepth()`。init 中两处调用：

```cpp
  if (!createRenderPass(VK_FORMAT_R8G8B8A8_UNORM, false, renderPass_)) return false;
  if (!createRenderPass(VK_FORMAT_R8G8B8A8_UNORM, true, renderPassDepth_)) return false;
```

析构中补 `vkDestroyRenderPass(device_, renderPassDepth_, nullptr);`。createSwapChain 的表面格式重建路径（`vkDestroyRenderPass(device_, renderPass_, ...)` 处）改为两个 pass 都销毁重建（深度 pass 同表面格式）。

b) `TargetRec` 加深度成员：

```cpp
  VkImage depthImg = VK_NULL_HANDLE; VkDeviceMemory depthMem = VK_NULL_HANDLE;
  VkImageView depthView = VK_NULL_HANDLE;
  bool hasDepth = false;
```

createOffscreenTarget 自建附件路径末尾（staging 创建之前）加：

```cpp
  if (desc.depth) {
    if (!createImage(desc.width, desc.height, VK_FORMAT_D32_SFLOAT, VK_IMAGE_TILING_OPTIMAL,
                     VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, rec.depthImg, rec.depthMem)) {
      return {};
    }
    VkImageViewCreateInfo dvci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    dvci.image = rec.depthImg;
    dvci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    dvci.format = VK_FORMAT_D32_SFLOAT;
    dvci.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(device_, &dvci, nullptr, &rec.depthView) != VK_SUCCESS) return {};
    rec.hasDepth = true;
  }
```

framebuffer 创建改为按深度选 pass 与附件：

```cpp
  VkImageView fbViews[2] = {rec.view, rec.depthView};
  VkFramebufferCreateInfo fbci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
  fbci.renderPass = desc.depth ? renderPassDepth_ : renderPass_;
  fbci.attachmentCount = desc.depth ? 2 : 1;
  fbci.pAttachments = fbViews;
  ...
```

destroyTarget 的 retire 闭包补深度资源销毁（fb/view/depthView/depthImg/depthMem 都在闭包内）。

c) `beginRenderPass` 按目标深度选 pass + 双清屏值：

```cpp
  VkClearValue clears[2]{};
  clears[0].color = {{clear.r, clear.g, clear.b, clear.a}};
  clears[1].depthStencil = {clear.depth, 0};
  VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  rp.renderPass = t.hasDepth ? device_->renderPassDepth() : device_->renderPass();
  rp.framebuffer = t.fb;
  rp.renderArea = {{0, 0}, {t.width, t.height}};
  rp.clearValueCount = t.hasDepth ? 2 : 1;
  rp.pClearValues = clears;
```

d) `createPipeline`：删掉深度拒绝守卫；深度状态启用并映射比较方式：

```cpp
  auto toVkCompare = [](DepthCompareOp op) {
    return op == DepthCompareOp::Greater ? VK_COMPARE_OP_GREATER : VK_COMPARE_OP_LESS;
  };
  VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  ds.depthTestEnable = desc.depthTest ? VK_TRUE : VK_FALSE;
  ds.depthWriteEnable = desc.depthWrite ? VK_TRUE : VK_FALSE;
  ds.depthCompareOp = toVkCompare(desc.depthCompare);
```

`gpci.renderPass` 改为按深度性选择：

```cpp
  const bool useDepth = desc.depthTest || desc.depthWrite;
  gpci.renderPass = useDepth ? renderPassDepth_ : renderPass_;
```
（约束：depth 管线须配 depth 目标——写进 createPipeline 的 doc 注释；反之非 depth 管线渲染 depth 目标也是调用方错误。)

`PipelineKey` 加 `uint32_t depthCompare = 0;`(operator== 与 hash mix 同步，key 构造处填 `uint32_t(desc.depthCompare)`)。

- [ ] **Step 5: Metal 实现（深度纹理 + DepthStencilState)**

a) `TargetRec` 加 `id<MTLTexture> depth = nil; bool hasDepth = false;`。

createOffscreenTarget 自建路径加：

```cpp
    if (desc.depth) {
      MTLTextureDescriptor* dd =
          [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                             width:desc.width
                                                            height:desc.height
                                                         mipmapped:NO];
      dd.usage = MTLTextureUsageRenderTarget;
      dd.storageMode = MTLStorageModePrivate;
      rec.depth = [device_ newTextureWithDescriptor:dd];
      if (!rec.depth) return {};
      rec.hasDepth = true;
    }
```

（createOffscreenTarget 当前用局部变量+`targets_.emplace(h, TargetRec{...})`，改为先构建完整 rec 再 emplace。)

b) `beginRenderPass` 加深度附件与清屏：

```cpp
  if (t.hasDepth) {
    rp.depthAttachment.texture = t.depth;
    rp.depthAttachment.loadAction = MTLLoadActionClear;
    rp.depthAttachment.clearDepth = clear.depth;
    rp.depthAttachment.storeAction = MTLStoreActionDontCare;
  }
```

c) `PipelineRec` 加 `id<MTLDepthStencilState> depthState = nil;`;createPipeline 删守卫并加：

```cpp
    if (desc.depthTest || desc.depthWrite) {
      pd.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;  // 约束:须配 depth 目标
      MTLDepthStencilDescriptor* dsd = [[MTLDepthStencilDescriptor alloc] init];
      dsd.depthCompareFunction = desc.depthCompare == DepthCompareOp::Greater
                                     ? MTLCompareFunctionGreater : MTLCompareFunctionLess;
      dsd.depthWriteEnabled = desc.depthWrite;
      rec.depthState = [device_ newDepthStencilStateWithDescriptor:dsd];
    }
```

（Metal 深度状态独立于 PSO;`PipelineKey` 同样加 depthCompare。)`bindPipeline` 内加：

```cpp
  if (pipeline_.depthState) [encoder_ setDepthStencilState:pipeline_.depthState];
```

- [ ] **Step 6: GLES 实现（深度 renderbuffer + GL 深度状态）**

a) `TargetRec` 加 `GLuint depthRbo = 0; bool hasDepth = false;`。

createOffscreenTarget 自建路径（颜色纹理之后）加：

```cpp
  if (desc.depth) {
    glGenRenderbuffers(1, &rec.depthRbo);
    glBindRenderbuffer(GL_RENDERBUFFER, rec.depthRbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, GLsizei(desc.width),
                          GLsizei(desc.height));
    glBindFramebuffer(GL_FRAMEBUFFER, rec.fbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER,
                              rec.depthRbo);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
      RD_LOGE("rhi.gles", "带深度 FBO 不完整");
      glDeleteRenderbuffers(1, &rec.depthRbo);
      return {};
    }
    rec.hasDepth = true;
  }
```

destroyTarget 补 `if (it->second.depthRbo) glDeleteRenderbuffers(1, &it->second.depthRbo);`。

b) beginRenderPass 闭包内（glClear 前）加：

```cpp
    if (t.hasDepth) {
      glDepthMask(GL_TRUE);  // 清深度前确保可写
      glClearDepthf(clear.depth);
      glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    } else {
      glClear(GL_COLOR_BUFFER_BIT);
    }
```
（替换原固定 `glClear(GL_COLOR_BUFFER_BIT)`。)

c) `PipelineRec` 加 `bool depthTest = false; bool depthWrite = false; DepthCompareOp depthCompare = DepthCompareOp::Less;`(createPipeline 填充并删守卫；`PipelineKey` 加 depthCompare)。bindPipeline 闭包内加：

```cpp
    if (rec.depthTest) {
      glEnable(GL_DEPTH_TEST);
      glDepthFunc(rec.depthCompare == DepthCompareOp::Greater ? GL_GREATER : GL_LESS);
    } else {
      glDisable(GL_DEPTH_TEST);
    }
    glDepthMask(rec.depthWrite ? GL_TRUE : GL_FALSE);
```

- [ ] **Step 7: 跑全部测试**

Run: `./scripts/check.sh 2>&1 | tail -3`
Expected: `100% tests passed`（含 `Depth.*` 2 用例；既有 58 项无回归——注意 cube golden 不受影响：cube pipeline depthTest=false → 无深度 pass)

Android 构建验证 GLES:`source /tmp/rd_env.sh && cd samples/android && ./gradlew :app:assembleDebug 2>&1 | tail -2` → `BUILD SUCCESSFUL`

- [ ] **Step 8: Commit**

```bash
git add core/rhi shaders/ tests/
git commit -m "feat(rhi): 离屏深度附件三后端 + depthTest/Write 放开 + CompareOp 预留"
```

---

### Task 2: image_codec 迁入 + gltf_loader + MeshRenderResource

**Files:**
- Create: `core/resource/image_codec.h`、`core/resource/image_codec.cpp`
- Create: `core/resource/gltf_loader.h`、`core/resource/gltf_loader.cpp`
- Create: `core/resource/mesh_render_resource.h`、`core/resource/mesh_render_resource.cpp`
- Modify: `cmake/Deps.cmake`（全平台分支加 stb + cgltf)
- Modify: `core/CMakeLists.txt`（新源文件 + include 目录）
- Modify: `tests/common/image.cpp`（改为调用 core codec,stb 实现宏移出）
- Test: `tests/resource/gltf_test.cpp`（新）
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: 写失败测试 `tests/resource/gltf_test.cpp`**

```cpp
// glTF 加载与资源上传测试:BoxTextured 结构断言、DamagedHelmet 多 mesh/32 位索引、
// 不存在文件返回空;GPU 上传(host Metal/Vulkan)句柄有效。
#include <gtest/gtest.h>
#include "resource/gltf_loader.h"
#include "resource/mesh_render_resource.h"
#include "rhi/rhi_device.h"

namespace {
const char* kAssets = RD_TEST_DATA_DIR "/assets";
} // namespace

TEST(Gltf, BoxTexturedStructure) {
  std::string path = std::string(kAssets) + "/BoxTextured.glb";
  auto model = rd::loadGltf(path.c_str());
  ASSERT_TRUE(model.valid());
  ASSERT_EQ(model.meshes.size(), 1u);
  const auto& m = model.meshes[0];
  EXPECT_EQ(m.indexCount, 36u);
  EXPECT_EQ(m.indexType, rd::IndexType::UInt16);
  EXPECT_EQ(m.vertices.size(), 24u * 8u);   // 24 顶点 × 8 float(stride 32)
  EXPECT_GT(m.baseColor.width, 0u);         // 内嵌纹理解码成功
  EXPECT_EQ(m.baseColor.pixels.size(), size_t(m.baseColor.width) * m.baseColor.height * 4u);
}

TEST(Gltf, DamagedHelmetMultiMesh32BitIndex) {
  std::string path = std::string(kAssets) + "/DamagedHelmet.glb";
  auto model = rd::loadGltf(path.c_str());
  ASSERT_TRUE(model.valid());
  EXPECT_GT(model.meshes.size(), 1u);
  bool hasU32 = false;
  for (const auto& m : model.meshes)
    if (m.indexType == rd::IndexType::UInt32) hasU32 = true;
  EXPECT_TRUE(hasU32);
}

TEST(Gltf, MissingFileReturnsEmpty) {
  auto model = rd::loadGltf("nonexistent.glb");
  EXPECT_FALSE(model.valid());
}

TEST(Gltf, UploadGpuResourcesMetal) {
#if defined(__APPLE__)
  rd::DeviceDesc d;
  d.backend = rd::Backend::Metal;
  auto dev = rd::createDevice(d);
  ASSERT_NE(dev, nullptr);
  std::string path = std::string(kAssets) + "/BoxTextured.glb";
  auto model = rd::loadGltf(path.c_str());
  ASSERT_TRUE(model.valid());
  auto res = rd::MeshRenderResource::upload(*dev, model);
  ASSERT_NE(res, nullptr);
  ASSERT_EQ(res->meshes().size(), 1u);
  EXPECT_TRUE(res->meshes()[0].vbo.valid());
  EXPECT_TRUE(res->meshes()[0].ibo.valid());
  EXPECT_TRUE(res->meshes()[0].baseColorTex.valid());
  EXPECT_TRUE(res->sampler().valid());
  res->destroy(*dev);
#endif
}
```

`tests/CMakeLists.txt` 加 `resource/gltf_test.cpp`。

- [ ] **Step 2: 跑测试确认编译失败**

Run: `cmake --build build -j8 2>&1 | grep -m3 error`
Expected: `resource/gltf_loader.h file not found`

- [ ] **Step 3: image_codec 迁入**

`core/resource/image_codec.h`:

```cpp
/**
 * @file image_codec.h
 * @brief 图像编解码(stb 封装):内存/文件 → RGBA8,RGBA8 → PNG。
 * 内核与测试/工具共用;解码失败返回 width==0 的 ImageData(优雅降级由调用方决定)。
 */
#pragma once
#include <cstdint>
#include <vector>

namespace rd {

/// 解码后的图像:RGBA8 紧凑排列(行主序,顶向下)。
struct ImageData {
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<uint8_t> pixels;
};

/// 内存解码(PNG/JPEG/KGA 等 stb 支持格式)→ RGBA8;失败返回 width==0。
ImageData decodeImageRGBA8(const void* data, uint64_t size);
/// 文件解码;失败返回 width==0。
ImageData loadImageRGBA8(const char* path);
/// 保存 RGBA8 为 PNG;成功返回 true。
bool saveImagePNG(const char* path, uint32_t w, uint32_t h, const uint8_t* rgba);

} // namespace rd
```

`core/resource/image_codec.cpp`（实现宏从 tests/common/image.cpp 整体迁入）:

```cpp
#include "resource/image_codec.h"

// stb 的 implementation 宏只能在一个编译单元定义（本文件即该单元）
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

namespace rd {

ImageData decodeImageRGBA8(const void* data, uint64_t size) {
  ImageData img;
  int w = 0, h = 0, channels = 0;
  uint8_t* decoded = stbi_load_from_memory(static_cast<const stbi_uc*>(data),
                                           static_cast<int>(size), &w, &h, &channels, 4);
  if (!decoded) return img;
  img.width = static_cast<uint32_t>(w);
  img.height = static_cast<uint32_t>(h);
  img.pixels.assign(decoded, decoded + static_cast<size_t>(w) * h * 4);
  stbi_image_free(decoded);
  return img;
}

ImageData loadImageRGBA8(const char* path) {
  ImageData img;
  int w = 0, h = 0, channels = 0;
  uint8_t* data = stbi_load(path, &w, &h, &channels, 4);
  if (!data) return img;
  img.width = static_cast<uint32_t>(w);
  img.height = static_cast<uint32_t>(h);
  img.pixels.assign(data, data + static_cast<size_t>(w) * h * 4);
  stbi_image_free(data);
  return img;
}

bool saveImagePNG(const char* path, uint32_t w, uint32_t h, const uint8_t* rgba) {
  return stbi_write_png(path, static_cast<int>(w), static_cast<int>(h), 4, rgba,
                        static_cast<int>(w * 4)) != 0;
}

} // namespace rd
```

`tests/common/image.cpp` 改为调用 core codec（删除 stb 实现宏与直接调用）:

```cpp
// image.h 的实现：PNG 读写委托 core/resource/image_codec;容差比较本地实现。
#include "common/image.h"
#include "resource/image_codec.h"
#include <cstdlib>

namespace rd::test {

bool savePNG(const std::string& path, uint32_t width, uint32_t height, const uint8_t* rgba) {
  return rd::saveImagePNG(path.c_str(), width, height, rgba);
}

Image loadPNG(const std::string& path) {
  Image img;
  auto d = rd::loadImageRGBA8(path.c_str());
  img.width = d.width;
  img.height = d.height;
  img.pixels = std::move(d.pixels);
  return img;
}

// compareRGBA8 保持现状(stb 无关)
```

- [ ] **Step 4: gltf_loader 实现**

`core/resource/gltf_loader.h`:

```cpp
/**
 * @file gltf_loader.h
 * @brief glTF 2.0 加载(cgltf):glb/gltf → ModelAsset(纯 CPU 数据,不碰 GPU)。
 *
 * 顶点统一规整为交错布局 pos(3f)|normal(3f)|uv(2f)(stride 32 字节);
 * 缺失属性(normal/uv)补 0。索引自适应 u16/u32(>65535 或源为 u32 时用 UInt32)。
 * 内嵌纹理图像解码为 RGBA8;解码失败返回无效 ImageData(调用方决定占位策略)。
 */
#pragma once
#include "resource/image_codec.h"
#include "rhi/rhi_types.h"
#include <cstdint>
#include <string>
#include <vector>

namespace rd {

/// 单个 mesh 的 CPU 数据。
struct MeshData {
  std::string name;
  std::vector<float> vertices;    // 交错 pos3|normal3|uv2
  std::vector<uint8_t> indices;   // 原始字节(indexType 决定位宽)
  IndexType indexType = IndexType::UInt16;
  uint32_t indexCount = 0;
  ImageData baseColor;            // 内嵌 baseColor 纹理;无效(width==0)表示无
};

/// 模型资产:一组 mesh。valid()==false 表示加载失败。
struct ModelAsset {
  std::vector<MeshData> meshes;
  bool valid() const { return !meshes.empty(); }
};

/// 加载 glb/gltf 文件;失败(不存在/解析错/无 mesh)返回空 ModelAsset 并记日志。
ModelAsset loadGltf(const char* path);

} // namespace rd
```

`core/resource/gltf_loader.cpp`:

```cpp
#include "resource/gltf_loader.h"
#include "foundation/log.h"
#include <cgltf.h>
#include <cstring>

namespace rd {
namespace {

/// 从 accessor 读 float 分量到 dst(每顶点 compCount 个);失败(缺失/类型错)填 0。
void readFloatAttr(const cgltf_attribute* attrs, cgltf_size attrCount, cgltf_attribute_type type,
                   cgltf_size vertexCount, uint32_t compCount, float* dst, uint32_t dstStride) {
  const cgltf_attribute* found = nullptr;
  for (cgltf_size i = 0; i < attrCount; ++i) {
    if (attrs[i].type == type) { found = &attrs[i]; break; }
  }
  if (!found) return;  // 缺失属性保持调用方预置的 0
  for (cgltf_size v = 0; v < vertexCount; ++v) {
    float tmp[4] = {0, 0, 0, 0};
    cgltf_accessor_read_float(found->data, v, tmp, compCount);
    float* out = dst + v * dstStride;
    for (uint32_t c = 0; c < compCount; ++c) out[c] = tmp[c];
  }
}

/// 解码 primitive 的 baseColor 纹理(仅内嵌 buffer view 路径;URI 外链记警告跳过)。
ImageData decodeBaseColor(const cgltf_primitive& prim) {
  ImageData img;
  const cgltf_material* mat = prim.material;
  if (!mat || !mat->has_pbr_metallic_roughness) return img;
  const cgltf_texture* tex = mat->pbr_metallic_roughness.base_color_texture.texture;
  if (!tex || !tex->image) return img;
  const cgltf_image* image = tex->image;
  if (image->buffer_view) {
    const cgltf_buffer_view* bv = image->buffer_view;
    const auto* bytes = static_cast<const uint8_t*>(bv->buffer->data);
    img = decodeImageRGBA8(bytes + bv->offset, uint64_t(bv->size));
  } else if (image->uri) {
    RD_LOGW("resource.gltf", "外链纹理 URI 暂不支持(2c KTX2 一起处理): %s", image->uri);
  }
  return img;
}

} // namespace

ModelAsset loadGltf(const char* path) {
  ModelAsset model;
  cgltf_options options{};
  cgltf_data* data = nullptr;
  if (cgltf_parse_file(&options, path, &data) != cgltf_result_success) {
    RD_LOGE("resource.gltf", "解析失败: %s", path);
    return model;
  }
  if (cgltf_load_buffers(&options, data, path) != cgltf_result_success) {
    RD_LOGE("resource.gltf", "缓冲加载失败: %s", path);
    cgltf_free(data);
    return model;
  }

  for (cgltf_size mi = 0; mi < data->meshes_count; ++mi) {
    const cgltf_mesh& mesh = data->meshes[mi];
    for (cgltf_size pi = 0; pi < mesh.primitives_count; ++pi) {
      const cgltf_primitive& prim = mesh.primitives[pi];
      if (prim.type != cgltf_primitive_type_triangles || !prim.indices) continue;

      MeshData out;
      out.name = mesh.name ? mesh.name : "mesh";
      const cgltf_accessor* pos = nullptr;
      for (cgltf_size ai = 0; ai < prim.attributes_count; ++ai) {
        if (prim.attributes[ai].type == cgltf_attribute_type_position) {
          pos = prim.attributes[ai].data;
          break;
        }
      }
      if (!pos) continue;
      const cgltf_size vertexCount = pos->count;
      out.vertices.resize(size_t(vertexCount) * 8, 0.0f);  // 8 float/顶点,缺省 0
      readFloatAttr(prim.attributes, prim.attributes_count, cgltf_attribute_type_position,
                    vertexCount, 3, out.vertices.data(), 8);
      readFloatAttr(prim.attributes, prim.attributes_count, cgltf_attribute_type_normal,
                    vertexCount, 3, out.vertices.data() + 3, 8);
      readFloatAttr(prim.attributes, prim.attributes_count, cgltf_attribute_type_texcoord,
                    vertexCount, 2, out.vertices.data() + 6, 8);

      // 索引:>65535 或源 u32 → UInt32
      const cgltf_size indexCount = prim.indices->count;
      const bool needU32 = prim.indices->component_type == cgltf_component_type_r_32u ||
                           vertexCount > 65535;
      out.indexType = needU32 ? IndexType::UInt32 : IndexType::UInt16;
      out.indexCount = uint32_t(indexCount);
      if (needU32) {
        out.indices.resize(size_t(indexCount) * 4);
        auto* dst = reinterpret_cast<uint32_t*>(out.indices.data());
        for (cgltf_size i = 0; i < indexCount; ++i)
          dst[i] = uint32_t(cgltf_accessor_read_index(prim.indices, i));
      } else {
        out.indices.resize(size_t(indexCount) * 2);
        auto* dst = reinterpret_cast<uint16_t*>(out.indices.data());
        for (cgltf_size i = 0; i < indexCount; ++i)
          dst[i] = uint16_t(cgltf_accessor_read_index(prim.indices, i));
      }

      out.baseColor = decodeBaseColor(prim);
      model.meshes.push_back(std::move(out));
    }
  }
  cgltf_free(data);
  if (!model.valid()) RD_LOGE("resource.gltf", "无有效 mesh: %s", path);
  return model;
}

} // namespace rd
```

- [ ] **Step 5: MeshRenderResource 实现**

`core/resource/mesh_render_resource.h`:

```cpp
/**
 * @file mesh_render_resource.h
 * @brief ModelAsset(CPU)→ 持久 GPU 资源集(device-local vbo/ibo + 纹理 + sampler)。
 * 由 scene 层或调用方以 shared_ptr 持有;Renderable 只引用不拥有。
 */
#pragma once
#include "resource/gltf_loader.h"
#include "rhi/rhi_device.h"
#include <memory>
#include <vector>

namespace rd {

/// 单个 mesh 的 GPU 资源。
struct MeshGpuData {
  BufferHandle vbo;
  BufferHandle ibo;
  IndexType indexType = IndexType::UInt16;
  uint32_t indexCount = 0;
  TextureHandle baseColorTex;   // 无纹理 mesh 指向 1x1 灰占位(与有纹理 mesh 统一绑定路径)
};

class MeshRenderResource {
public:
  /// 上传整个 ModelAsset;任一步失败记日志并返回 nullptr(已建资源自动清理)。
  static std::shared_ptr<MeshRenderResource> upload(Device& dev, const ModelAsset& model);
  const std::vector<MeshGpuData>& meshes() const { return meshes_; }
  SamplerHandle sampler() const { return sampler_; }

  /// 释放全部 GPU 资源(持有方在不再使用时调用;句柄 destroy 后本对象不可再用)。
  void destroy(Device& dev);

private:
  std::vector<MeshGpuData> meshes_;
  SamplerHandle sampler_;
  TextureHandle fallbackTex_;
};

} // namespace rd
```

`core/resource/mesh_render_resource.cpp`:

```cpp
#include "resource/mesh_render_resource.h"
#include "foundation/log.h"

namespace rd {

std::shared_ptr<MeshRenderResource> MeshRenderResource::upload(Device& dev,
                                                               const ModelAsset& model) {
  if (!model.valid()) return nullptr;
  auto res = std::shared_ptr<MeshRenderResource>(new MeshRenderResource());
  // 1x1 中性灰占位:无纹理 mesh 的统一绑定对象
  const uint8_t gray[4] = {128, 128, 128, 255};
  rd::TextureDesc fd;
  fd.width = 1;
  fd.height = 1;
  fd.data = gray;
  fd.dataSize = 4;
  res->fallbackTex_ = dev.createTexture(fd);
  res->sampler_ = dev.createSampler({});
  if (!res->fallbackTex_.valid() || !res->sampler_.valid()) {
    res->destroy(dev);
    return nullptr;
  }

  for (const auto& m : model.meshes) {
    MeshGpuData g;
    g.vbo = dev.createBuffer({uint64_t(m.vertices.size() * 4), BufferUsage::Vertex, false,
                              false, m.vertices.data()});
    g.ibo = dev.createBuffer({uint64_t(m.indices.size()), BufferUsage::Index, false, false,
                              m.indices.data()});
    g.indexType = m.indexType;
    g.indexCount = m.indexCount;
    if (m.baseColor.width > 0) {
      rd::TextureDesc td;
      td.width = m.baseColor.width;
      td.height = m.baseColor.height;
      td.data = m.baseColor.pixels.data();
      td.dataSize = uint64_t(m.baseColor.pixels.size());
      g.baseColorTex = dev.createTexture(td);
    } else {
      g.baseColorTex = res->fallbackTex_;
      RD_LOGW("resource.gltf", "mesh %s 无 baseColor 纹理,用 1x1 灰占位", m.name.c_str());
    }
    if (!g.vbo.valid() || !g.ibo.valid() || !g.baseColorTex.valid()) {
      res->destroy(dev);
      return nullptr;
    }
    res->meshes_.push_back(g);
  }
  return res;
}

void MeshRenderResource::destroy(Device& dev) {
  for (auto& g : meshes_) {
    if (g.vbo.valid()) dev.destroyBuffer(g.vbo);
    if (g.ibo.valid()) dev.destroyBuffer(g.ibo);
    if (g.baseColorTex.valid() && g.baseColorTex != fallbackTex_)
      dev.destroyTexture(g.baseColorTex);
  }
  meshes_.clear();
  if (fallbackTex_.valid()) dev.destroyTexture(fallbackTex_);
  if (sampler_.valid()) dev.destroySampler(sampler_);
  fallbackTex_ = {};
  sampler_ = {};
}

} // namespace rd
```

- [ ] **Step 6: CMake 接线**

`cmake/Deps.cmake`：把 stb 的 FetchContent 移出 host-only 分支（所有平台都要，image_codec 进 rd_core)，并在同位置（glm 之后）加：

```cmake
FetchContent_Declare(stb
  URL https://github.com/nothings/stb/archive/refs/heads/master.tar.gz)
FetchContent_MakeAvailable(stb)

FetchContent_Declare(cgltf
  URL https://github.com/jkuhlmann/cgltf/archive/refs/tags/v1.14.tar.gz)
FetchContent_MakeAvailable(cgltf)
```
（注意删掉原 host-only 分支里重复的 stb 声明；host 分支其余不动。)

`core/CMakeLists.txt` 的 rd_core 源列表加：

```
  resource/image_codec.cpp
  resource/gltf_loader.cpp
  resource/mesh_render_resource.cpp
```

并在 `target_include_directories(rd_core PUBLIC ...)` 行追加 `${stb_SOURCE_DIR} ${cgltf_SOURCE_DIR}`。

- [ ] **Step 7: 跑全部测试**

Run: `./scripts/check.sh 2>&1 | tail -3`
Expected: `100% tests passed`（含 `Gltf.*` 4 用例；golden 无回归）

- [ ] **Step 8: Commit**

```bash
git add core/resource/ tests/common/image.cpp tests/resource/ tests/CMakeLists.txt \
        cmake/Deps.cmake core/CMakeLists.txt
git commit -m "feat(resource): image_codec 迁入 + gltf_loader(cgltf)+ MeshRenderResource"
```

---

### Task 3: scene 层（Node/Camera/Scene)+ 单测

**Files:**
- Create: `core/scene/node.h`、`core/scene/node.cpp`
- Create: `core/scene/camera.h`
- Create: `core/scene/scene.h`、`core/scene/scene.cpp`
- Modify: `core/CMakeLists.txt`
- Test: `tests/scene/scene_test.cpp`（新）
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: 写失败测试 `tests/scene/scene_test.cpp`**

```cpp
// scene 层单测:Node TRS/局部矩阵、父子 world 复合、Camera view/proj。
#include <gtest/gtest.h>
#include "scene/node.h"
#include "scene/camera.h"
#include "foundation/math.h"
#include <glm/glm.hpp>

namespace {
bool near4(const rd::math::Mat4& a, const rd::math::Mat4& b, float eps = 1e-5f) {
  for (int c = 0; c < 4; ++c)
    for (int r = 0; r < 4; ++r)
      if (std::abs(a[c][r] - b[c][r]) > eps) return false;
  return true;
}
} // namespace

TEST(SceneNode, LocalMatrixTRS) {
  rd::scene::Node n;
  n.setTRS({1, 2, 3}, {0, 0, 0}, {2, 2, 2});
  auto expect = glm::scale(glm::translate(rd::math::Mat4(1.0f), glm::vec3(1, 2, 3)),
                           glm::vec3(2, 2, 2));
  EXPECT_TRUE(near4(n.localMatrix(), expect));
}

TEST(SceneNode, WorldMatrixParentChain) {
  rd::scene::Node parent;
  parent.setTRS({10, 0, 0}, {0, 0, 0}, {1, 1, 1});
  auto child = std::make_unique<rd::scene::Node>();
  child->setTRS({1, 0, 0}, {0, 0, 0}, {1, 1, 1});
  auto* childPtr = child.get();
  parent.addChild(std::move(child));
  // world = parent.local × child.local → 平移 (11,0,0)
  auto w = childPtr->worldMatrix();
  EXPECT_NEAR(w[3][0], 11.0f, 1e-5f);
}

TEST(SceneCamera, ViewProj) {
  rd::scene::Camera cam;
  cam.lookAt({0, 0, 3}, {0, 0, 0}, {0, 1, 0});
  cam.setPerspective(glm::radians(45.0f), 1.0f, 0.1f, 100.0f);
  auto v = cam.viewMatrix();
  EXPECT_NEAR(v[3][2], -3.0f, 1e-5f);  // 视图平移 -3(z)
  auto p = cam.projMatrix();
  // GLM_FORCE_DEPTH_ZERO_TO_ONE:zNear 点映射到 NDC z=0
  glm::vec4 nearPt = p * glm::vec4(0, 0, -0.1f, 1);
  EXPECT_NEAR(nearPt.z / nearPt.w, 0.0f, 1e-4f);
}
```

`tests/CMakeLists.txt` 加 `scene/scene_test.cpp`。

- [ ] **Step 2: 跑测试确认编译失败**

Run: `cmake --build build -j8 2>&1 | grep -m3 error`
Expected: `scene/node.h file not found`

- [ ] **Step 3: 实现**

`core/scene/node.h`:

```cpp
/**
 * @file node.h
 * @brief 场景节点:局部 TRS + 子节点树;world 矩阵经父链复合。
 * MeshNode 挂渲染资源(renderer 层的 submit 由 Scene::collect 驱动)。
 */
#pragma once
#include "foundation/math.h"
#include <memory>
#include <vector>

namespace rd { class Renderer; class MeshRenderResource; }

namespace rd::scene {

class Node {
public:
  virtual ~Node() = default;

  /// 设置局部 TRS(平移/欧拉弧度 XYZ/缩放)。
  void setTRS(const math::Vec3& t, const math::Vec3& eulerRad, const math::Vec3& s);
  /// 局部矩阵:T × R(euler XYZ) × S。
  math::Mat4 localMatrix() const;
  /// 世界矩阵:沿父链左乘到根。
  math::Mat4 worldMatrix() const;

  /// 挂载子节点(接管所有权);返回子节点引用便于继续挂接。
  Node& addChild(std::unique_ptr<Node> child);
  const std::vector<std::unique_ptr<Node>>& children() const { return children_; }

  /// 收集渲染项(基类空实现;MeshNode 重写)。由 Scene::collect 遍历调用。
  virtual void collect(Renderer& renderer) const {}

private:
  math::Vec3 t_{0.0f}, euler_{0.0f}, s_{1.0f};
  Node* parent_ = nullptr;  // 非拥有
  std::vector<std::unique_ptr<Node>> children_;
};

/// 挂网格渲染资源的节点(资源为 shared_ptr 持久持有,节点只引用)。
class MeshNode : public Node {
public:
  std::shared_ptr<MeshRenderResource> mesh;
  void collect(Renderer& renderer) const override;
};

} // namespace rd::scene
```

`core/scene/node.cpp`:

```cpp
#include "scene/node.h"
#include "renderer/renderer.h"
#include "resource/mesh_render_resource.h"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace rd::scene {

void Node::setTRS(const math::Vec3& t, const math::Vec3& eulerRad, const math::Vec3& s) {
  t_ = t;
  euler_ = eulerRad;
  s_ = s;
}

math::Mat4 Node::localMatrix() const {
  auto m = glm::translate(math::Mat4(1.0f), t_);
  m = glm::rotate(m, euler_.z, math::Vec3(0, 0, 1));
  m = glm::rotate(m, euler_.y, math::Vec3(0, 1, 0));
  m = glm::rotate(m, euler_.x, math::Vec3(1, 0, 0));
  return glm::scale(m, s_);
}

math::Mat4 Node::worldMatrix() const {
  math::Mat4 m = localMatrix();
  for (Node* p = parent_; p; p = p->parent_) m = p->localMatrix() * m;
  return m;
}

Node& Node::addChild(std::unique_ptr<Node> child) {
  child->parent_ = this;
  children_.push_back(std::move(child));
  return *children_.back();
}

void MeshNode::collect(Renderer& renderer) const {
  if (mesh) renderer.submit(mesh, worldMatrix());
}

} // namespace rd::scene
```

`core/scene/camera.h`（纯头文件）:

```cpp
/**
 * @file camera.h
 * @brief 相机:位置/朝向 + 透视投影;view/proj 矩阵供 Renderer 场景 UBO。
 * 投影遵循全局 GLM_FORCE_DEPTH_ZERO_TO_ONE(NDC z∈[0,1],右手系)。
 */
#pragma once
#include "foundation/math.h"

namespace rd::scene {

class Camera {
public:
  void lookAt(const math::Vec3& eye, const math::Vec3& center, const math::Vec3& up) {
    eye_ = eye;
    center_ = center;
    up_ = up;
  }
  void setPerspective(float fovYRad, float aspect, float zNear, float zFar) {
    fovY_ = fovYRad;
    aspect_ = aspect;
    zNear_ = zNear;
    zFar_ = zFar;
  }
  math::Mat4 viewMatrix() const { return math::lookAt(eye_, center_, up_); }
  math::Mat4 projMatrix() const { return math::perspective(fovY_, aspect_, zNear_, zFar_); }

private:
  math::Vec3 eye_{0, 0, 3}, center_{0}, up_{0, 1, 0};
  float fovY_ = 0.78539816f, aspect_ = 1.0f, zNear_ = 0.1f, zFar_ = 100.0f;
};

} // namespace rd::scene
```

`core/scene/scene.h`:

```cpp
/**
 * @file scene.h
 * @brief 场景:root Node + 深度优先遍历收集渲染项投给 Renderer。
 */
#pragma once
#include "scene/node.h"

namespace rd { class Renderer; }

namespace rd::scene {

class Scene {
public:
  /// 根节点(挂载/编辑场景结构的入口)。
  Node& root() { return root_; }
  /// 深度优先遍历,对每个节点调用 collect(renderer)。
  void collect(Renderer& renderer);

private:
  Node root_;
};

} // namespace rd::scene
```

`core/scene/scene.cpp`:

```cpp
#include "scene/scene.h"
#include "renderer/renderer.h"

namespace rd::scene {
namespace {
void collectRecursive(const Node& n, Renderer& renderer) {
  n.collect(renderer);
  for (const auto& c : n.children()) collectRecursive(*c, renderer);
}
} // namespace

void Scene::collect(Renderer& renderer) { collectRecursive(root_, renderer); }

} // namespace rd::scene
```

`core/CMakeLists.txt` 源列表加 `scene/node.cpp` `scene/scene.cpp`。

- [ ] **Step 4: 跑测试确认通过**

Run: `cmake --build build -j8 2>&1 | grep -m3 error`; `ctest --test-dir build --output-on-failure -R "Scene"`
Expected: 无 error;`SceneNode.*`/`SceneCamera.*` 3 用例 PASSED

- [ ] **Step 5: Commit**

```bash
git add core/scene/ core/CMakeLists.txt tests/scene/ tests/CMakeLists.txt
git commit -m "feat(scene): Node/Camera/Scene 骨架 + 单测"
```

---

### Task 4: renderer 层骨架（Renderer/Renderable/MeshRenderable)

**Files:**
- Create: `core/renderer/renderable.h`
- Create: `core/renderer/mesh_renderable.h`、`core/renderer/mesh_renderable.cpp`
- Create: `core/renderer/renderer.h`、`core/renderer/renderer.cpp`
- Modify: `core/CMakeLists.txt`
- Test: `tests/renderer/renderer_test.cpp`（新，smoke：空帧/单 mesh 提交不崩）
- Modify: `tests/CMakeLists.txt`

设计要点（与 spec §3.1 对齐）:
- `Renderer` 持有：unlit 管线（init 时创建，走阶段一管线缓存）、per-item mvp 动态 UBO(hostWrite,**256B 对齐步进**,grow-only)、render queue（帧末清空）。
- per-item mvp:CPU 计算 `viewProj × world`,endScene 开头统一填入 UBO;每个 draw 以 `offset = i×256` 绑定子区间（三后端对齐要求的最小公倍）。
- `Renderable::prepass` 默认空（2b IBL 预滤波钩子）。

- [ ] **Step 1: 写失败测试 `tests/renderer/renderer_test.cpp`**

```cpp
// renderer 骨架 smoke 测试:init → 空帧 → 提交 BoxTextured 一帧 → 不崩溃且句柄有效。
// (渲染正确性由 Task 5 的 BoxTextured golden 兜底)
#include <gtest/gtest.h>
#include "renderer/renderer.h"
#include "resource/gltf_loader.h"
#include "resource/mesh_render_resource.h"
#include "scene/camera.h"
#include "scene/scene.h"
#include "common/shader_code.h"
#include "rhi/rhi_device.h"
#include "rd_shader_dir.h"

namespace {
constexpr uint32_t kW = 256, kH = 256;

std::unique_ptr<rd::Renderer> makeRenderer(rd::Device& dev, rd::Backend b) {
  auto vs = rd::test::loadShaderCode(b, RD_SHADER_DIR, "unlit.vert");
  auto fs = rd::test::loadShaderCode(b, RD_SHADER_DIR, "unlit.frag");
  auto r = std::make_unique<rd::Renderer>();
  rd::RendererShaderDesc sd{vs.code, fs.code, vs.entry, rd::Format::RGBA8_UNORM};
  if (!r->init(dev, sd)) return nullptr;
  return r;
}

void runCase(rd::Backend b) {
  rd::DeviceDesc d;
  d.backend = b;
  auto dev = rd::createDevice(d);
  ASSERT_NE(dev, nullptr);
  auto renderer = makeRenderer(*dev, b);
  ASSERT_NE(renderer, nullptr);

  rd::OffscreenTargetDesc td;
  td.width = kW;
  td.height = kH;
  td.depth = true;
  auto target = dev->createOffscreenTarget(td);
  ASSERT_TRUE(target.valid());

  rd::scene::Camera cam;
  cam.lookAt({0, 0, 3}, {0, 0, 0}, {0, 1, 0});
  cam.setPerspective(0.78539816f, 1.0f, 0.1f, 100.0f);

  // 空帧(无提交)
  dev->beginFrame();
  renderer->beginScene(cam, {0.1f, 0.1f, 0.1f, 1.0f});
  auto* cmd = dev->acquireCommandBuffer();
  renderer->endScene(cmd, target);
  dev->submit(cmd);
  dev->waitIdle();
  dev->endFrame();

  // 提交 BoxTextured 一帧
  auto model = rd::loadGltf(RD_TEST_DATA_DIR "/assets/BoxTextured.glb");
  ASSERT_TRUE(model.valid());
  auto res = rd::MeshRenderResource::upload(*dev, model);
  ASSERT_NE(res, nullptr);
  rd::scene::Scene scene;
  auto meshNode = std::make_unique<rd::scene::MeshNode>();
  meshNode->mesh = res;
  scene.root().addChild(std::move(meshNode));

  dev->beginFrame();
  renderer->beginScene(cam, {0.1f, 0.1f, 0.1f, 1.0f});
  scene.collect(*renderer);
  cmd = dev->acquireCommandBuffer();
  renderer->endScene(cmd, target);
  dev->submit(cmd);
  dev->waitIdle();
  dev->endFrame();
  res->destroy(*dev);
}
} // namespace

TEST(Renderer, MetalSmokeFrame) {
#if defined(__APPLE__)
  runCase(rd::Backend::Metal);
#endif
}

TEST(Renderer, VulkanSmokeFrame) {
#if defined(RD_WITH_VULKAN)
  runCase(rd::Backend::Vulkan);
#endif
}
```

`tests/CMakeLists.txt` 加 `renderer/renderer_test.cpp`。

- [ ] **Step 2: 跑测试确认编译失败**

Run: `cmake --build build -j8 2>&1 | grep -m3 error`
Expected: `renderer/renderer.h file not found`

- [ ] **Step 3: 实现**

`core/renderer/renderable.h`:

```cpp
/**
 * @file renderable.h
 * @brief Renderable 基类:一帧内可录制命令的渲染项。
 * prepass 为 2b IBL 预滤波等 GPU 预处理的钩子(对齐 GGUI 两阶段录制,默认空)。
 */
#pragma once
#include "rhi/rhi_device.h"

namespace rd {

class Renderable {
public:
  virtual ~Renderable() = default;
  /// render pass 前的预处理钩子(默认空)。
  virtual void prepass(CommandBuffer* cmd) { (void)cmd; }
  /// 录制本项绘制命令;sceneUBO 为 per-item mvp 动态缓冲(offset 由实现自管)。
  virtual void record(CommandBuffer* cmd, BufferHandle sceneUBO, uint64_t uboOffset) = 0;
};

} // namespace rd
```

`core/renderer/mesh_renderable.h`:

```cpp
/**
 * @file mesh_renderable.h
 * @brief MeshRenderable:引用持久 MeshRenderResource 的渲染项(只引用不拥有)。
 */
#pragma once
#include "renderer/renderable.h"
#include "resource/mesh_render_resource.h"
#include "foundation/math.h"
#include <memory>

namespace rd {

class MeshRenderable : public Renderable {
public:
  MeshRenderable(std::shared_ptr<MeshRenderResource> mesh, PipelineHandle pipeline)
      : mesh_(std::move(mesh)), pipeline_(pipeline) {}

  void record(CommandBuffer* cmd, BufferHandle sceneUBO, uint64_t uboOffset) override;

private:
  std::shared_ptr<MeshRenderResource> mesh_;
  PipelineHandle pipeline_;
};

} // namespace rd
```

`core/renderer/mesh_renderable.cpp`:

```cpp
#include "renderer/mesh_renderable.h"

namespace rd {

void MeshRenderable::record(CommandBuffer* cmd, BufferHandle sceneUBO, uint64_t uboOffset) {
  for (const auto& g : mesh_->meshes()) {
    cmd->bindPipeline(pipeline_);
    cmd->bindUniformBuffer(0, sceneUBO, uboOffset, 64);  // 本项的 mvp 子区间
    cmd->bindTexture(0, g.baseColorTex, mesh_->sampler());
    cmd->bindVertexBuffer(0, g.vbo, 0);
    cmd->bindIndexBuffer(g.ibo, 0, g.indexType);
    cmd->drawIndexed(g.indexCount, 0, 0);
  }
}

} // namespace rd
```

`core/renderer/renderer.h`:

```cpp
/**
 * @file renderer.h
 * @brief Renderer:帧流程编排——场景相机/清屏 → 收集渲染项 → pass 序列执行。
 *
 * 立即模式执行(GGUI 哲学):每帧 beginScene → submit×N → endScene,
 * 帧末渲染项清空;per-item mvp 经 256B 对齐的动态 UBO 子区间绑定。
 */
#pragma once
#include "renderer/renderable.h"
#include "foundation/math.h"
#include <memory>
#include <vector>

namespace rd::scene { class Camera; }

namespace rd {

/// Renderer init 的 shader 描述(调用方从离线产物加载传入)。
struct RendererShaderDesc {
  std::vector<uint8_t> vsCode;
  std::vector<uint8_t> fsCode;
  std::string entry;            // Metal="main0",其他="main"
  Format colorFormat = Format::RGBA8_UNORM;
};

class Renderer {
public:
  ~Renderer() { shutdown(); }
  /// 初始化:unlit 管线(depthTest 开)+ per-item 动态 UBO。失败返回 false。
  bool init(Device& dev, const RendererShaderDesc& desc);
  /// 释放 GPU 资源(设备销毁前调用)。
  void shutdown();

  /// 帧开始:存相机 viewProj 与清屏值。
  void beginScene(const scene::Camera& camera, const ClearColor& clear);
  /// 提交一个网格渲染项(资源 shared_ptr 持久持有,本帧引用)。
  void submit(const std::shared_ptr<MeshRenderResource>& mesh, const math::Mat4& world);
  /// pass 序列执行:prepass 钩子 → MainPass(depth);帧末队列清空。
  void endScene(CommandBuffer* cmd, TargetHandle target);

private:
  static constexpr uint32_t kUboStride = 256;   // 三后端对齐最小公倍
  static constexpr uint32_t kMaxItems = 64;     // 动态 UBO 容量(超出记警告截断)

  Device* dev_ = nullptr;
  ShaderModuleHandle vs_, fs_;
  PipelineHandle pipeline_;
  BufferHandle sceneUBO_;       // hostWrite,kUboStride*kMaxItems
  math::Mat4 viewProj_{1.0f};
  ClearColor clear_;
  std::vector<std::unique_ptr<Renderable>> queue_;
  std::vector<math::Mat4> worldStack_;  // 与 queue_ 平行的 world 矩阵(endScene 算 mvp)
};

} // namespace rd
```

`core/renderer/renderer.cpp`:

```cpp
#include "renderer/renderer.h"
#include "renderer/mesh_renderable.h"
#include "scene/camera.h"
#include "foundation/log.h"

namespace rd {

bool Renderer::init(Device& dev, const RendererShaderDesc& desc) {
  dev_ = &dev;
  vs_ = dev.createShaderModule({ShaderStage::Vertex, desc.vsCode, desc.entry});
  fs_ = dev.createShaderModule({ShaderStage::Fragment, desc.fsCode, desc.entry});

  PipelineDesc pd;
  pd.vertexShader = vs_;
  pd.fragmentShader = fs_;
  pd.vertexBindings = {{0, 32}};  // pos3|normal3|uv2 交错
  pd.attributes = {{0, Format::R32G32B32_FLOAT, 0, 0},
                   {1, Format::R32G32B32_FLOAT, 12, 0},
                   {2, Format::R32G32_FLOAT, 24, 0}};
  pd.cullMode = CullMode::None;   // 2a 保守不剔除(2b 定绕序约定后开 Back)
  pd.depthTest = true;
  pd.depthWrite = true;
  pd.colorFormat = desc.colorFormat;
  pipeline_ = dev.createPipeline(pd);

  sceneUBO_ = dev.createBuffer({uint64_t(kUboStride) * kMaxItems, BufferUsage::Uniform,
                                true, false, nullptr});
  if (!vs_.valid() || !fs_.valid() || !pipeline_.valid() || !sceneUBO_.valid()) {
    shutdown();
    return false;
  }
  return true;
}

void Renderer::shutdown() {
  if (!dev_) return;
  if (pipeline_.valid()) dev_->destroyPipeline(pipeline_);
  if (vs_.valid()) dev_->destroyShaderModule(vs_);
  if (fs_.valid()) dev_->destroyShaderModule(fs_);
  if (sceneUBO_.valid()) dev_->destroyBuffer(sceneUBO_);
  pipeline_ = {};
  vs_ = {};
  fs_ = {};
  sceneUBO_ = {};
  dev_ = nullptr;
}

void Renderer::beginScene(const scene::Camera& camera, const ClearColor& clear) {
  viewProj_ = camera.projMatrix() * camera.viewMatrix();
  clear_ = clear;
}

void Renderer::submit(const std::shared_ptr<MeshRenderResource>& mesh,
                      const math::Mat4& world) {
  if (queue_.size() >= kMaxItems) {
    RD_LOGW("renderer", "渲染项超出 %u,截断", kMaxItems);
    return;
  }
  auto item = std::make_unique<MeshRenderable>(mesh, pipeline_);
  // mvp 在 endScene 统一计算填充(此处先存 world)
  worldStack_.push_back(world);
  queue_.push_back(std::move(item));
}

void Renderer::endScene(CommandBuffer* cmd, TargetHandle target) {
  // 统一填充 per-item mvp(viewProj × world)
  const uint32_t count = uint32_t(queue_.size());
  for (uint32_t i = 0; i < count; ++i) {
    math::Mat4 mvp = viewProj_ * worldStack_[i];
    dev_->updateBuffer(sceneUBO_, &mvp, sizeof(mvp), uint64_t(i) * kUboStride);
  }
  cmd->beginRenderPass(target, clear_);
  for (uint32_t i = 0; i < count; ++i) {
    queue_[i]->prepass(cmd);  // 2b 钩子(默认空)
    queue_[i]->record(cmd, sceneUBO_, uint64_t(i) * kUboStride);
  }
  cmd->endRenderPass();
  queue_.clear();
  worldStack_.clear();
}

} // namespace rd
```

`core/CMakeLists.txt` 源列表加 `renderer/renderer.cpp` `renderer/mesh_renderable.cpp`。

- [ ] **Step 4: 跑测试确认通过**

Run: `cmake --build build -j8 2>&1 | grep -m3 error`; `ctest --test-dir build --output-on-failure -R "Renderer"`
Expected: 无 error;`Renderer.*SmokeFrame` 2 用例 PASSED（注：unlit shader 在 Task 5 才加入——本测试的 `loadShaderCode("unlit.vert")` 会失败 → 若因此 init 失败属预期，先补 Task 5 Step 1 的 shader 再回本步）

> **顺序注意**:Task 5 的 unlit shader 文件是本任务测试的前置。执行时先把 Task 5 Step 1 的两个 shader 文件与 `shaders/CMakeLists.txt` 行加上，再跑本步（或交换 Task 4/5 顺序亦可——golden 测试放最后）。

- [ ] **Step 5: Commit**

```bash
git add core/renderer/ core/CMakeLists.txt tests/renderer/ tests/CMakeLists.txt
git commit -m "feat(renderer): Renderer/Renderable/MeshRenderable 帧流程骨架"
```

---

### Task 5: unlit shader + BoxTextured golden + render_test --model

**Files:**
- Create: `shaders/unlit.vert`、`shaders/unlit.frag`
- Modify: `shaders/CMakeLists.txt`
- Test: `tests/renderer/box_unlit_test.cpp`（新 golden）
- Modify: `tests/CMakeLists.txt`
- Modify: `tools/render_test/main.cpp`(--model 模式）

- [ ] **Step 1: 写 shader**

`shaders/unlit.vert`:

```glsl
// unlit.vert:glTF 静态模型 unlit 顶点着色器。
// 布局:location0=pos(vec3)@0,location1=normal(vec3)@12(2b 用,本版未采样),
//       location2=uv(vec2)@24;交错 stride 32。UBO binding0=mvp。
#version 450
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(binding = 0) uniform UBO { mat4 mvp; };
layout(location = 0) out vec2 vUV;
void main() {
  gl_Position = mvp * vec4(aPos, 1.0);
  vUV = aUV;
}
```

`shaders/unlit.frag`:

```glsl
// unlit.frag:baseColor 纹理直出。texture slot 0 ↔ set0 binding4 / texN(GLES)。
#version 450
layout(location = 0) in vec2 vUV;
layout(binding = 4) uniform sampler2D tex0;
layout(location = 0) out vec4 outColor;
void main() { outColor = texture(tex0, vUV); }
```

`shaders/CMakeLists.txt` 追加：`rd_compile_shader(unlit.vert)` / `rd_compile_shader(unlit.frag)`。

- [ ] **Step 2: 写 golden 测试 `tests/renderer/box_unlit_test.cpp`**

```cpp
// BoxTextured 固定机位 unlit 渲染 golden:相机 (0,0,3) 看向原点,fov 45°,
// 512x512 带深度,与 tests/golden/ 基准 PNG 容差比较(Metal/Vulkan 各一份)。
// 更新 golden:RD_UPDATE_GOLDENS=1 ctest --test-dir build -R BoxUnlit(更新后须目视核对)
#include <gtest/gtest.h>
#include <cstdlib>
#include "common/image.h"
#include "common/shader_code.h"
#include "renderer/renderer.h"
#include "resource/gltf_loader.h"
#include "resource/mesh_render_resource.h"
#include "scene/camera.h"
#include "scene/scene.h"
#include "rhi/rhi_device.h"
#include "rd_shader_dir.h"

namespace {
constexpr uint32_t kW = 512, kH = 512;

std::string goldenPath(rd::Backend b) {
  std::string name = (b == rd::Backend::Metal) ? "box_unlit_metal.png" : "box_unlit_vulkan.png";
  return std::string(RD_TEST_DATA_DIR) + "/golden/" + name;
}

rd::test::Image renderBox(rd::Backend b) {
  rd::DeviceDesc d;
  d.backend = b;
  auto device = rd::createDevice(d);
  if (!device) return {};
  auto vs = rd::test::loadShaderCode(b, RD_SHADER_DIR, "unlit.vert");
  auto fs = rd::test::loadShaderCode(b, RD_SHADER_DIR, "unlit.frag");
  rd::Renderer renderer;
  rd::RendererShaderDesc sd{vs.code, fs.code, vs.entry, rd::Format::RGBA8_UNORM};
  rd::OffscreenTargetDesc td;
  td.width = kW;
  td.height = kH;
  td.depth = true;
  auto target = device->createOffscreenTarget(td);
  auto model = rd::loadGltf(RD_TEST_DATA_DIR "/assets/BoxTextured.glb");
  if (!target.valid() || !model.valid() || !renderer.init(*device, sd)) return {};
  auto res = rd::MeshRenderResource::upload(*device, model);
  if (!res) return {};

  rd::scene::Scene scene;
  auto meshNode = std::make_unique<rd::scene::MeshNode>();
  meshNode->mesh = res;
  scene.root().addChild(std::move(meshNode));
  rd::scene::Camera cam;
  cam.lookAt({0, 0, 3}, {0, 0, 0}, {0, 1, 0});
  cam.setPerspective(0.78539816f, 1.0f, 0.1f, 100.0f);

  device->beginFrame();
  renderer.beginScene(cam, {0.2f, 0.2f, 0.25f, 1.0f});
  scene.collect(renderer);
  auto* cmd = device->acquireCommandBuffer();
  renderer.endScene(cmd, target);
  device->submit(cmd);
  device->waitIdle();
  device->endFrame();

  rd::test::Image img;
  img.width = kW;
  img.height = kH;
  img.pixels.resize(kW * kH * 4);
  device->readbackTarget(target, img.pixels.data(), img.pixels.size());
  res->destroy(*device);
  renderer.shutdown();
  return img;
}

void runGolden(rd::Backend b) {
  auto img = renderBox(b);
  ASSERT_EQ(img.pixels.size(), kW * kH * 4);
  const std::string path = goldenPath(b);
  if (std::getenv("RD_UPDATE_GOLDENS")) {
    ASSERT_TRUE(rd::test::savePNG(path, img.width, img.height, img.pixels.data()));
    return;
  }
  auto golden = rd::test::loadPNG(path);
  ASSERT_EQ(golden.pixels.size(), img.pixels.size()) << "golden 缺失: " << path;
  auto cmp = rd::test::compareRGBA8(img.pixels.data(), golden.pixels.data(), kW, kH, 3, 0.02);
  EXPECT_TRUE(cmp.pass) << "diffRatio=" << cmp.diffRatio;
}
} // namespace

TEST(BoxUnlit, MetalGolden) {
#if defined(__APPLE__)
  runGolden(rd::Backend::Metal);
#endif
}

TEST(BoxUnlit, VulkanGolden) {
#if defined(RD_WITH_VULKAN)
  runGolden(rd::Backend::Vulkan);
#endif
}
```

`tests/CMakeLists.txt` 加 `renderer/box_unlit_test.cpp`。

- [ ] **Step 3: 生成 golden 并目视核对**

Run: `cmake -S . -B build > /dev/null && cmake --build build -j8 2>&1 | grep -m3 error`
Run: `RD_UPDATE_GOLDENS=1 ctest --test-dir build -R BoxUnlit --output-on-failure`
然后**目视核对** `tests/golden/box_unlit_metal.png` 与 `box_unlit_vulkan.png`（应为一个带棋盘格纹理的立方体，深灰蓝背景）；再用系统看图工具或 `./build/tools/img_check/img_check tests/golden/box_unlit_metal.png --min-coverage 0.03` 确认覆盖率。

- [ ] **Step 4: 跑 golden 比对**

Run: `ctest --test-dir build --output-on-failure -R BoxUnlit`
Expected: 2 用例 PASSED

- [ ] **Step 5: render_test --model 模式**

`tools/render_test/main.cpp` 参数区加：

```cpp
  std::string model;
  // 解析循环内加:
    } else if (!strcmp(argv[i], "--model") && i + 1 < argc) {
      model = argv[++i];
    }
```

cube 渲染分支之前插入 model 分支：

```cpp
  if (!model.empty()) {
    auto vs = rd::test::loadShaderCode(backend, RD_SHADER_DIR, "unlit.vert");
    auto fs = rd::test::loadShaderCode(backend, RD_SHADER_DIR, "unlit.frag");
    rd::Renderer renderer;
    rd::RendererShaderDesc sd{vs.code, fs.code, vs.entry, rd::Format::RGBA8_UNORM};
    rd::OffscreenTargetDesc td;
    td.width = kW;
    td.height = kH;
    td.depth = true;
    auto target = device->createOffscreenTarget(td);
    auto m = rd::loadGltf(model.c_str());
    if (!target.valid() || !m.valid() || !renderer.init(*device, sd)) {
      fprintf(stderr, "初始化失败\n");
      return 1;
    }
    auto res = rd::MeshRenderResource::upload(*device, m);
    if (!res) return 1;
    rd::scene::Scene scene;
    auto node = std::make_unique<rd::scene::MeshNode>();
    node->mesh = res;
    scene.root().addChild(std::move(node));
    rd::scene::Camera cam;
    cam.lookAt({0, 0, 3}, {0, 0, 0}, {0, 1, 0});
    cam.setPerspective(0.78539816f, 1.0f, 0.1f, 100.0f);
    device->beginFrame();
    renderer.beginScene(cam, {0.2f, 0.2f, 0.25f, 1.0f});
    scene.collect(renderer);
    auto* cmd = device->acquireCommandBuffer();
    renderer.endScene(cmd, target);
    device->submit(cmd);
    device->waitIdle();
    device->endFrame();
    std::vector<uint8_t> pixels(static_cast<size_t>(kW) * kH * 4);
    if (!device->readbackTarget(target, pixels.data(), pixels.size())) {
      fprintf(stderr, "readback 失败\n");
      return 1;
    }
    res->destroy(*device);
    renderer.shutdown();
    if (!rd::test::savePNG(out, kW, kH, pixels.data())) {
      fprintf(stderr, "保存失败\n");
      return 1;
    }
    printf("已保存 %s (%ux%u)\n", out.c_str(), kW, kH);
    return 0;
  }
```

（include 区补 `renderer/renderer.h` `resource/gltf_loader.h` `resource/mesh_render_resource.h` `scene/camera.h` `scene/scene.h`;`tools/render_test` 的 CMake 若未链 rd_core 新层，确认 `target_link_libraries(... rd_core)` 不变即可——所有新层都在 rd_core 内。)

- [ ] **Step 6: 工具验证 + 全量测试**

Run: `./build/tools/render_test/render_test --backend vulkan --model tests/assets/BoxTextured.glb --out /tmp/box.png && ./build/tools/img_check/img_check /tmp/box.png --min-coverage 0.03`
Expected: 保存成功 + 覆盖率通过
Run: `./scripts/check.sh 2>&1 | tail -3`
Expected: `100% tests passed`

- [ ] **Step 7: Commit**

```bash
git add shaders/ tests/renderer/ tests/CMakeLists.txt tests/golden/ tools/render_test/
git commit -m "feat(renderer): unlit shader + BoxTextured golden + render_test --model"
```

---

### Task 6: 文档同步

**Files:**
- Modify: `AGENTS.md`
- Modify: `docs/superpowers/plans/2026-08-09-p1-1-pbr-core.md`（顶部加取代标注）

- [ ] **Step 1: AGENTS.md 更新**

「当前状态」改为：

```markdown
## 当前状态
- P0 完成:构建基建 + foundation + RHI 三后端 + shader 离线管线 + C API + 双端容器
- 阶段一完成:RHI 底座强化(能力表/内存 flag/N 帧退休/管线缓存/instancing/cube 渲染目标/
  GLES 纹理+延迟回放)
- 阶段二 a 完成:renderer/scene/resource 三层骨架 + 离屏深度附件 + glTF unlit 渲染
  (BoxTextured golden 双后端)
- 下一步:阶段二 b(PBR/IBL)
```

「代码约定」追加：

```markdown
- 顶点布局约定(glTF 模型):pos(3f)@0 | normal(3f)@12 | uv(2f)@24,交错 stride 32
- 深度:离屏目标 `OffscreenTargetDesc.depth=true` + pipeline `depthTest/depthWrite`;
  depth 管线须配 depth 目标;CompareOp 默认 Less(Reverse-Z 预留)
- renderer 层 per-item UBO 步进 256B(三后端对齐最小公倍)
```

- [ ] **Step 2: 旧 P1 计划标注**

`docs/superpowers/plans/2026-08-09-p1-1-pbr-core.md` 顶部标题下加：

```markdown
> **状态注记(2026-08-13)**: 本计划的 RHI 部分(T1-T4 纹理/采样器/深度)已由
> 「阶段一 RHI 底座强化」与「阶段二 a 框架骨架」以更强形态完成;
> T5-T10 中:cgltf 加载/图像编解码已由阶段二 a 完成(见 2026-08-13 两份 spec/plan);
> 环境生成/uber-shader/DamagedHelmet golden 将在阶段二 b 参考本计划实施。
```

- [ ] **Step 3: Commit**

```bash
git add AGENTS.md docs/superpowers/plans/2026-08-09-p1-1-pbr-core.md
git commit -m "docs: 阶段二 a 收尾(AGENTS.md 新约定 + 旧 P1 计划取代标注)"
```

---

## Self-Review 记录

- **Spec 覆盖**:spec §2 分层→Task 2/3/4;§3.1 帧流程→Task 4;§3.2 深度→Task 1;§3.3 resource→Task 2;§3.4 scene→Task 3;§3.5 unlit→Task 5;§4 测试→各 Task 内嵌 + Task 5 golden;§5 六个 commit→Task 1-6 一一对应。
- **顺序修正**:Task 4 测试依赖 Task 5 的 unlit shader——已在 Task 4 Step 4 显式标注（先加 shader 再跑），两任务可合并执行。
- **类型一致**:`MeshRenderResource::upload/destroy`、`Renderer::submit(mesh, world)`、`RendererShaderDesc`、`record(cmd, sceneUBO, uboOffset)` 在 Task 3/4/5 间一致;`ClearColor.depth` 与 Task 1 各后端用法一致。
- **已知遗留（归 2b/2c)**:tangent/mesh_utils、IBL、orbit 手势、KTX2、swapchain 深度、Reverse-Z 启用评估。

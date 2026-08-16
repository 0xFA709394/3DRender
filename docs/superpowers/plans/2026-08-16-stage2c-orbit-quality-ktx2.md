# 阶段二 c:Orbit 手势 + 画质分级 + KTX2 实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 按 `docs/superpowers/specs/2026-08-16-stage2c-orbit-quality-ktx2-design.md` 实现:①RHI 压缩纹理格式(ASTC/ETC2)+ MSAA/resolve;②KTX2(libktx)资源链 + KHR_texture_basisu;③renderer 上屏链(SceneTarget→upscale pass)+ 画质三档;④OrbitController + C API 输入/模型加载 + iOS/Android 接线;⑤render_test --interactive(GLFW)。

**Architecture:** RHI 先行(契约测试门控);renderer 的 endScene 改为两段(场景→内部 SceneTarget,全屏三角形 upscale→最终目标),upscale pass 即 P2 后处理挂载点;OrbitController 在 scene 层吃归一化指针事件(纯数学可单测);engine(rd_api)从 CubeScene 迁移到 Renderer 链,内嵌 shader 泛化为多 shader 表。

**Tech Stack:** C++17、glm、cgltf、KTX-Software(libktx,FetchContent)、GLFW(仅 host)、既有三后端 RHI 与离线 shader 管线。

**通用约定(全任务遵守):**
- 构建+测试:`./scripts/check.sh`;单跑:`ctest --test-dir build --output-on-failure -R <正则>`
- golden 更新:`RD_UPDATE_GOLDENS=1 ctest --test-dir build -R <用例>` 后**像素核对** `tests/golden/*.png` 再提交
- 提交规范:每 Task 一个 commit,`<type>(<scope>): 描述`
- 绑定约定不变:uniform slot N ↔ Metal buffer(N) ↔ Vulkan set0 binding N ↔ GLES binding N;texture slot N ↔ Metal texture/sampler(N+4) ↔ Vulkan set0 binding(N+4) ↔ GLES 纹理单元 N(sampler 名 `texN`)
- UBO:slot0=FrameUBO(256B),slot1=ItemUBO(256B 步进);顶点 48B 交错 pos3@0|normal3@12|tangent4@24|uv2@40
- 纹理槽:0=baseColor,1=MR,2=normal,3=emissive,4=occlusion,5=prefilterCube,6=brdfLut
- 新增纹理槽(blit):**0=sceneColor**(blit pass 独立管线,与 PBR 槽位表无冲突)
- 错误处理:内核不用异常;创建失败返回无效句柄/nullptr + `RD_LOGE`
- 句柄约定:`Handle<Tag>` 0 无效;GPU 资源只经 `rhi::Device` 创建/销毁
- NDC y 约定:三后端渲染均为 y 向上(Vulkan 负 viewport 高度已对齐);**采样纹理 v=0 = 内存首行 = 图像顶行**;GLES 渲染到纹理时 NDC+Y 落在内存末行(与 Metal/Vulkan 相反),blit 顶点着色器用 uniform 翻转吸收(见 Task 11)
- 测试后端门控:Metal 用例 `#if defined(__APPLE__)`;Vulkan 用例 `#if defined(RD_WITH_VULKAN)`;GLES 仅 Android(host 不跑)

---

### Task 1: 压缩格式枚举 + formatBlockInfo + caps + 三后端映射/上传

**Files:**
- Modify: `core/rhi/rhi_types.h`(Format 新增 + formatBlockInfo/formatMipBytes)
- Modify: `core/rhi/rhi_constants.inc.h`(新 caps)
- Modify: `core/rhi/backends/metal/metal_device.mm`(toMTLPixelFormat + caps + createTexture/updateTexture 块计算)
- Modify: `core/rhi/backends/vulkan/vulkan_device.cpp`(toVkFormat + caps + 上传块计算)
- Modify: `core/rhi/backends/gles/gles_device.cpp`(格式三元组 + caps + glCompressedTexImage2D 路径)
- Test: `tests/rhi/rhi_types_test.cpp`(追加)、`tests/rhi/caps_test.cpp`(追加名表断言)

- [ ] **Step 1: 写失败测试**

`tests/rhi/rhi_types_test.cpp` 追加:

```cpp
// 压缩格式的 block 信息:ASTC/ETC2 均 4x4 block、16 字节
TEST(RhiTypes, CompressedFormatBlockInfo) {
  const auto astc = rd::formatBlockInfo(rd::Format::ASTC_4x4_UNORM);
  EXPECT_EQ(astc.blockW, 4u);
  EXPECT_EQ(astc.blockH, 4u);
  EXPECT_EQ(astc.bytesPerBlock, 16u);
  const auto etc2 = rd::formatBlockInfo(rd::Format::ETC2_RGBA8_UNORM);
  EXPECT_EQ(etc2.blockW, 4u);
  EXPECT_EQ(etc2.bytesPerBlock, 16u);
  // 非压缩格式退化为 1x1 block
  const auto rgba = rd::formatBlockInfo(rd::Format::RGBA8_UNORM);
  EXPECT_EQ(rgba.blockW, 1u);
  EXPECT_EQ(rgba.bytesPerBlock, 4u);
}

// mip 字节数:尺寸按 block 上取整(非 block 对齐尺寸合法)
TEST(RhiTypes, CompressedMipBytes) {
  // 8x8 ASTC = 2x2 block × 16B = 64B;9x9 → 3x3 block = 144B
  EXPECT_EQ(rd::formatMipBytes(rd::Format::ASTC_4x4_UNORM, 8, 8), 64u);
  EXPECT_EQ(rd::formatMipBytes(rd::Format::ASTC_4x4_UNORM, 9, 9), 144u);
  EXPECT_EQ(rd::formatMipBytes(rd::Format::RGBA8_UNORM, 4, 4), 64u);
}
```

`tests/rhi/caps_test.cpp` 追加(名表与枚举同源,不会越界):

```cpp
TEST(Caps, CompressionCapsNames) {
  EXPECT_STREQ(rd::DeviceCaps::name(rd::Capability::texture_compression_astc),
               "texture_compression_astc");
  EXPECT_STREQ(rd::DeviceCaps::name(rd::Capability::texture_compression_etc2),
               "texture_compression_etc2");
}
```

- [ ] **Step 2: 跑测试确认失败**

Run: `./scripts/check.sh 2>&1 | tail -5`(编译失败:formatBlockInfo 未定义)
Expected: 编译错误 `formatBlockInfo is not a member of rd`

- [ ] **Step 3: 实现 rhi_types.h 扩展**

`core/rhi/rhi_types.h` 的 `enum class Format` 追加两个枚举值(尾部追加,不改既有值序):

```cpp
  ASTC_4x4_UNORM,       ///< ASTC 4x4 block 压缩(16B/block;Metal/Vulkan caps 门控)
  ETC2_RGBA8_UNORM,     ///< ETC2 RGBA 压缩(16B/block;GLES ES3 core,Vulkan 罕见)
```

`formatSize` 对压缩格式返回 0(压缩格式无"每像素字节"语义),switch 追加:

```cpp
    case Format::ASTC_4x4_UNORM:
    case Format::ETC2_RGBA8_UNORM:
      return 0;  // 压缩格式按 block 计,见 formatBlockInfo
```

并在 `formatSize` 后新增:

```cpp
/// 压缩格式 block 信息;非压缩格式退化为 1x1、bytesPerBlock=formatSize。
struct FormatBlockInfo {
  uint32_t blockW, blockH, bytesPerBlock;
};

inline FormatBlockInfo formatBlockInfo(Format f) {
  switch (f) {
    case Format::ASTC_4x4_UNORM:
    case Format::ETC2_RGBA8_UNORM:
      return {4, 4, 16};
    default:
      return {1, 1, formatSize(f)};
  }
}

/// 单 mip(或单 face-mip)数据字节数:尺寸按 block 上取整 × bytesPerBlock。
inline uint64_t formatMipBytes(Format f, uint32_t w, uint32_t h) {
  const FormatBlockInfo bi = formatBlockInfo(f);
  return uint64_t((w + bi.blockW - 1) / bi.blockW) *
         ((h + bi.blockH - 1) / bi.blockH) * bi.bytesPerBlock;
}
```

- [ ] **Step 4: caps 枚举**

`core/rhi/rhi_constants.inc.h` 在 `RD_CAPABILITY(anisotropy)` 前追加:

```c
RD_CAPABILITY(texture_compression_astc) // ASTC 4x4 LDR 纹理(1=支持)
RD_CAPABILITY(texture_compression_etc2) // ETC2 RGBA 纹理(1=支持)
```

- [ ] **Step 5: Metal 后端**

`metal_device.mm`:
1. `toMTLPixelFormat` 追加:

```objc
    case Format::ASTC_4x4_UNORM: return MTLPixelFormatASTC_4x4_LDR;
    case Format::ETC2_RGBA8_UNORM: return MTLPixelFormatInvalid;  // Metal 不支持 ETC2
```

2. caps 上报(init 内既有 caps_.set 区追加):

```objc
    const bool isAppleGpu = [device_ supportsFamily:MTLGPUFamilyApple1] ||
                            [device_ supportsFamily:MTLGPUFamilyMac2];
    caps_.set(Capability::texture_compression_astc, isAppleGpu ? 1 : 0);
    caps_.set(Capability::texture_compression_etc2, 0);  // Metal 无 ETC2
```

3. createTexture:前置校验追加压缩格式 caps 门控(放在尺寸校验之后):

```objc
    if ((desc.format == Format::ASTC_4x4_UNORM &&
         !caps_.supports(Capability::texture_compression_astc)) ||
        (desc.format == Format::ETC2_RGBA8_UNORM &&
         !caps_.supports(Capability::texture_compression_etc2))) {
      RD_LOGE("rhi.metal", "createTexture: 压缩格式 %d 不受本后端支持", int(desc.format));
      return {};
    }
```

4. createTexture 上传路径:把 `const uint32_t fmtSize = formatSize(desc.format);` 与
   `const uint64_t levelBytes = uint64_t(w) * h * fmtSize;` 改为块计算:

```objc
      const FormatBlockInfo bi = formatBlockInfo(desc.format);
      ...
        const uint64_t levelBytes = formatMipBytes(desc.format, w, h);
```

`replaceRegion` 调用处:压缩格式 `bytesPerRow` 传 `((w + bi.blockW-1)/bi.blockW) * bi.bytesPerBlock`(Metal 对压缩纹理的 bytesPerRow 即"每行 block 字节数");非压缩维持 `w * bi.bytesPerBlock`。统一写法:

```objc
          const uint64_t rowBytes =
              uint64_t((w + bi.blockW - 1) / bi.blockW) * bi.bytesPerBlock;
```

(`replaceRegion:... withBytes:bytesPerRow:` 处使用该 rowBytes;ASTC 上传要求 rowBytes ≥ 256 时 Metal 无特殊对齐要求,4x4 block 8 列=128B 亦可)

5. updateTexture 同样把 `formatSize` 改为 `formatMipBytes`/`rowBytes` 计算(同 createTexture 模式)。

- [ ] **Step 6: Vulkan 后端**

`vulkan_device.cpp`:
1. `toVkFormat` 追加:

```cpp
    case Format::ASTC_4x4_UNORM: return VK_FORMAT_ASTC_4x4_UNORM_BLOCK;
    case Format::ETC2_RGBA8_UNORM: return VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK;
```

2. caps(init 内,物理设备 feature 查询处;features 结构体已存在则直接读):

```cpp
    caps_.set(Capability::texture_compression_astc,
              physFeatures.textureCompressionASTC_LDR ? 1 : 0);
    caps_.set(Capability::texture_compression_etc2,
              physFeatures.textureCompressionETC2 ? 1 : 0);
```

(若 init 未查询 features:在 pickPhysicalDevice 后 `VkPhysicalDeviceFeatures physFeatures; vkGetPhysicalDeviceFeatures(phys_, &physFeatures);`)

3. createTexture 前置校验追加(同 Metal 的 caps 门控,日志 tag `rhi.vk`)。
4. 上传路径(createTexture 内 staging 拷贝与 updateTexture):mip 尺寸推进改为
   `uint64_t levelBytes = formatMipBytes(desc.format, w, h);`,`VkBufferImageCopy`
   的 `bufferRowLength=0/bufferImageHeight=0`(tight,压缩格式合法),`imageExtent`
   仍传像素尺寸 `{w,h,1}`(Vulkan 对压缩格式自动按 block 解释,无需调用方对齐——
   但 w/h 本身可以非 block 对齐,driver 内部 clamp)。

- [ ] **Step 7: GLES 后端**

`gles_device.cpp`:
1. 格式三元组函数(约 line 150 的 format 映射)追加:

```cpp
      case Format::ASTC_4x4_UNORM:
        internal = GL_COMPRESSED_RGBA_ASTC_4x4_KHR; upload = 0; type = 0; break;
      case Format::ETC2_RGBA8_UNORM:
        internal = GL_COMPRESSED_RGBA8_ETC2_EAC; upload = 0; type = 0; break;
```

(upload/type 为 0 作为"压缩格式"标记)
2. caps:ETC2 恒 1(ES3 core);ASTC 查扩展:

```cpp
    caps_.set(Capability::texture_compression_etc2, 1);
    const char* exts = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
    caps_.set(Capability::texture_compression_astc,
              exts && strstr(exts, "GL_KHR_texture_compression_astc_ldr") ? 1 : 0);
```

3. createTexture/updateTexture 上传:`upload == 0` 时走 `glCompressedTexImage2D(target, mip, internal, w, h, 0, GLsizei(levelBytes), src)`,levelBytes 用 `formatMipBytes`;数据越界检查同步改块计算;caps 门控同 Metal。
4. `#include <cstring>`(strstr;若已包含则跳过)。

- [ ] **Step 8: 跑测试**

Run: `./scripts/check.sh 2>&1 | tail -8`
Expected: 全绿(RhiTypes.CompressedFormatBlockInfo / CompressedMipBytes / Caps.CompressionCapsNames 通过;既有用例不回归)

- [ ] **Step 9: Commit**

```bash
git add core/rhi tests/rhi
git commit -m "feat(rhi): ASTC/ETC2 压缩格式枚举 + formatBlockInfo + caps + 三后端上传路径"
```

---

### Task 2: 压缩纹理契约测试(创建/上传/采样冒烟)

**Files:**
- Create: `tests/rhi/compressed_texture_test.cpp`
- Modify: `tests/CMakeLists.txt`(注册新文件)

说明:host 上 Metal(Apple Silicon)/Vulkan(MoltenVK)支持 ASTC;ETC2 在 host 两端均不支持 → 预期创建失败(caps 门控行为即断言)。像素级正确性由 Task 19 的 KTX2 golden 兜底,本任务只验契约行为与链路不崩。

- [ ] **Step 1: 写测试**

`tests/rhi/compressed_texture_test.cpp`:

```cpp
// 压缩纹理契约测试:caps 门控的创建/上传/渲染冒烟。
// ASTC 在 Apple Silicon(Metal/MoltenVK)可用;ETC2 host 不可用(预期拒绝)。
#include <gtest/gtest.h>
#include "common/shader_code.h"
#include "rhi/rhi_device.h"
#include "rd_shader_dir.h"
#include <cstring>

namespace {
constexpr uint32_t kW = 64, kH = 64;

// 8x8 RGBA8 像素(quad 采样源无所谓内容,只验链路)
void fillPixels(uint8_t* p, uint32_t n) {
  for (uint32_t i = 0; i < n; ++i) p[i] = uint8_t((i * 37) & 0xFF);
}

void runContract(rd::Backend b) {
  rd::DeviceDesc d;
  d.backend = b;
  auto dev = rd::createDevice(d);
  ASSERT_NE(dev, nullptr);

  // ---- ETC2:host 两端均不支持 → 创建必须失败 ----
  if (!dev->caps().supports(rd::Capability::texture_compression_etc2)) {
    rd::TextureDesc td;
    td.width = 8;
    td.height = 8;
    td.format = rd::Format::ETC2_RGBA8_UNORM;
    uint8_t data[64] = {};
    td.data = data;
    td.dataSize = sizeof(data);
    EXPECT_FALSE(dev->createTexture(td).valid()) << "ETC2 无 caps 却创建成功";
  }

  // ---- ASTC:按 caps 分支 ----
  rd::TextureDesc td;
  td.width = 8;
  td.height = 8;
  td.mipLevels = 2;  // 8x8(64B) + 4x4(16B) = 80B
  td.format = rd::Format::ASTC_4x4_UNORM;
  uint8_t astcData[80] = {};
  fillPixels(astcData, sizeof(astcData));
  td.data = astcData;
  td.dataSize = sizeof(astcData);
  auto tex = dev->createTexture(td);
  if (!dev->caps().supports(rd::Capability::texture_compression_astc)) {
    EXPECT_FALSE(tex.valid());
    return;  // 无 caps:后续用例无意义
  }
  ASSERT_TRUE(tex.valid());

  // 数据量不足必须失败(2 mip 需 80B,只给 64B)
  {
    rd::TextureDesc bad = td;
    bad.dataSize = 64;
    EXPECT_FALSE(dev->createTexture(bad).valid());
  }

  // ---- 渲染冒烟:离屏目标 + texquad shader 采样 ASTC 纹理 ----
  auto vs = rd::test::loadShaderCode(b, RD_SHADER_DIR, "texquad.vert");
  auto fs = rd::test::loadShaderCode(b, RD_SHADER_DIR, "texquad.frag");
  auto vsMod = dev->createShaderModule({rd::ShaderStage::Vertex, vs.code, vs.entry});
  auto fsMod = dev->createShaderModule({rd::ShaderStage::Fragment, fs.code, fs.entry});
  ASSERT_TRUE(vsMod.valid() && fsMod.valid());
  rd::PipelineDesc pd;
  pd.vertexShader = vsMod;
  pd.fragmentShader = fsMod;
  // texquad 顶点布局(与 shaders/texquad.vert 一致):pos3@0|uv2@12,binding0 stride20
  pd.vertexBindings = {{0, 20}};
  pd.attributes = {{0, rd::Format::R32G32B32_FLOAT, 0, 0},
                   {1, rd::Format::R32G32_FLOAT, 12, 0}};
  auto pipe = dev->createPipeline(pd);
  ASSERT_TRUE(pipe.valid());

  rd::OffscreenTargetDesc od;
  od.width = kW;
  od.height = kH;
  auto target = dev->createOffscreenTarget(od);
  ASSERT_TRUE(target.valid());
  auto sampler = dev->createSampler({});
  ASSERT_TRUE(sampler.valid());

  // 全屏 quad(两个三角形,6 顶点;pos3+uv2 交错,stride 20)
  const float quad[6 * 5] = {-1, -1, 0, 0, 0, 1, -1, 0, 1, 0, -1, 1, 0, 0, 1,
                             1,  -1, 0, 1, 0, 1, 1,  0, 1, 1, -1, 1, 0, 0, 1};
  auto vbo = dev->createBuffer({sizeof(quad), rd::BufferUsage::Vertex, false, false, quad});
  ASSERT_TRUE(vbo.valid());

  auto* cmd = dev->acquireCommandBuffer();
  cmd->beginRenderPass(target, {0, 0, 0, 1});
  cmd->bindPipeline(pipe);
  cmd->bindVertexBuffer(0, vbo, 0);
  cmd->bindTexture(0, tex, sampler);
  cmd->draw(6, 0);
  cmd->endRenderPass();
  dev->submit(cmd);
  dev->waitIdle();

  // readback 成功即链路通畅(内容不做像素断言——ASTC 解码由 GPU 保证)
  std::vector<uint8_t> px(size_t(kW) * kH * 4);
  EXPECT_TRUE(dev->readbackTarget(target, px.data(), px.size()));

  dev->destroyBuffer(vbo);
  dev->destroySampler(sampler);
  dev->destroyTarget(target);
  dev->destroyPipeline(pipe);
  dev->destroyShaderModule(vsMod);
  dev->destroyShaderModule(fsMod);
  dev->destroyTexture(tex);
}
} // namespace

TEST(CompressedTexture, Metal) {
#if defined(__APPLE__)
  runContract(rd::Backend::Metal);
#endif
}

TEST(CompressedTexture, Vulkan) {
#if defined(RD_WITH_VULKAN)
  runContract(rd::Backend::Vulkan);
#endif
}
```

- [ ] **Step 2: 验证测试编译并运行**

`tests/CMakeLists.txt` 的 `add_executable(rd_tests ...)` 列表追加 `rhi/compressed_texture_test.cpp`。

Run: `./scripts/check.sh 2>&1 | tail -8`
Expected: 全绿;`CompressedTexture.Metal/Vulkan` 通过(若 host GPU 意外无 ASTC caps,用例按 caps 分支跳过渲染部分仍通过)

注意:若 `texquad` 的顶点布局与上面假设不同,打开 `tests/rhi/texture_quad_test.cpp` 核对 vertexBindings/attributes/quad 数据并改成一致。

- [ ] **Step 3: Commit**

```bash
git add tests/rhi/compressed_texture_test.cpp tests/CMakeLists.txt
git commit -m "test(rhi): 压缩纹理契约测试(caps 门控 + 上传校验 + 采样冒烟)"
```

---

### Task 3: Device::targetColorTexture/targetSize + 目标可采样化(三后端)

**Files:**
- Modify: `core/rhi/rhi_device.h`(两个接口方法)
- Modify: `core/rhi/backends/vulkan/vulkan_device.cpp`(color 注册纹理 + SAMPLED usage + endRenderPass layout 链 + swapchain 目标跳过 staging 拷贝)
- Modify: `core/rhi/backends/metal/metal_device.mm`(color 注册 + srcTexture 补存)
- Modify: `core/rhi/backends/gles/gles_device.cpp`(colorTex 注册 + srcTexture 补存)
- Test: `tests/rhi/target_texture_test.cpp`(新建)、`tests/CMakeLists.txt`(注册)

背景:upscale pass(Task 11)要"渲染到目标 A → 把 A 当纹理采样渲染到目标 B"。
当前缺口:①自建离屏目标的颜色不是纹理句柄(Vulkan 的 color 是裸 VkImage);
②Vulkan 颜色 finalLayout=TRANSFER_SRC 不能采样。本任务打通该链路,MSAA 的
resolve 纹理(Task 4-6)复用同一接口。

- [ ] **Step 1: 写失败测试**

`tests/rhi/target_texture_test.cpp`:

```cpp
// 目标可采样化契约:渲染到目标 A(清屏红色)→ 把 A 的颜色当纹理采样画到目标 B
// → readback B 验红色。覆盖 targetColorTexture/targetSize 接口。
#include <gtest/gtest.h>
#include "common/shader_code.h"
#include "rhi/rhi_device.h"
#include "rd_shader_dir.h"
#include <vector>

namespace {
constexpr uint32_t kW = 64, kH = 64;

void runRoundtrip(rd::Backend b) {
  rd::DeviceDesc d;
  d.backend = b;
  auto dev = rd::createDevice(d);
  ASSERT_NE(dev, nullptr);

  rd::OffscreenTargetDesc od;
  od.width = kW;
  od.height = kH;
  auto targetA = dev->createOffscreenTarget(od);
  auto targetB = dev->createOffscreenTarget(od);
  ASSERT_TRUE(targetA.valid() && targetB.valid());

  // targetSize 接口
  uint32_t w = 0, h = 0;
  dev->targetSize(targetA, w, h);
  EXPECT_EQ(w, kW);
  EXPECT_EQ(h, kH);

  // pass1:清屏红色到 A
  auto* cmd = dev->acquireCommandBuffer();
  cmd->beginRenderPass(targetA, {1, 0, 0, 1});
  cmd->endRenderPass();

  // pass2:采样 A 画全屏 quad 到 B(texquad shader,布局与 texture_quad_test 一致)
  auto vs = rd::test::loadShaderCode(b, RD_SHADER_DIR, "texquad.vert");
  auto fs = rd::test::loadShaderCode(b, RD_SHADER_DIR, "texquad.frag");
  auto vsMod = dev->createShaderModule({rd::ShaderStage::Vertex, vs.code, vs.entry});
  auto fsMod = dev->createShaderModule({rd::ShaderStage::Fragment, fs.code, fs.entry});
  ASSERT_TRUE(vsMod.valid() && fsMod.valid());
  rd::PipelineDesc pd;
  pd.vertexShader = vsMod;
  pd.fragmentShader = fsMod;
  pd.vertexBindings = {{0, 20}};  // 若与 texture_quad_test 布局不同,以其为准
  pd.attributes = {{0, rd::Format::R32G32B32_FLOAT, 0, 0},
                   {1, rd::Format::R32G32_FLOAT, 12, 0}};
  auto pipe = dev->createPipeline(pd);
  ASSERT_TRUE(pipe.valid());
  auto sampler = dev->createSampler({});
  ASSERT_TRUE(sampler.valid());
  rd::TextureHandle sceneTex = dev->targetColorTexture(targetA);
  ASSERT_TRUE(sceneTex.valid()) << "targetColorTexture 返回无效句柄";

  const float quad[6 * 5] = {-1, -1, 0, 0, 0, 1, -1, 0, 1, 0, -1, 1, 0, 0, 1,
                             1,  -1, 0, 1, 0, 1, 1,  0, 1, 1, -1, 1, 0, 0, 1};
  auto vbo = dev->createBuffer({sizeof(quad), rd::BufferUsage::Vertex, false, false, quad});
  ASSERT_TRUE(vbo.valid());

  cmd->beginRenderPass(targetB, {0, 0, 0, 1});
  cmd->bindPipeline(pipe);
  cmd->bindVertexBuffer(0, vbo, 0);
  cmd->bindTexture(0, sceneTex, sampler);
  cmd->draw(6, 0);
  cmd->endRenderPass();
  dev->submit(cmd);
  dev->waitIdle();

  std::vector<uint8_t> px(size_t(kW) * kH * 4);
  ASSERT_TRUE(dev->readbackTarget(targetB, px.data(), px.size()));
  // 中心像素应为红(允许采样/格式微差)
  const size_t c = (size_t(kH / 2) * kW + kW / 2) * 4;
  EXPECT_GT(px[c], 200u) << "R";
  EXPECT_LT(px[c + 1], 60u) << "G";
  EXPECT_LT(px[c + 2], 60u) << "B";

  dev->destroyBuffer(vbo);
  dev->destroySampler(sampler);
  dev->destroyPipeline(pipe);
  dev->destroyShaderModule(vsMod);
  dev->destroyShaderModule(fsMod);
  dev->destroyTarget(targetA);
  dev->destroyTarget(targetB);
}
} // namespace

TEST(TargetTexture, Metal) {
#if defined(__APPLE__)
  runRoundtrip(rd::Backend::Metal);
#endif
}
TEST(TargetTexture, Vulkan) {
#if defined(RD_WITH_VULKAN)
  runRoundtrip(rd::Backend::Vulkan);
#endif
}
```

`tests/CMakeLists.txt` 追加 `rhi/target_texture_test.cpp`。

注意:写 quad 顶点数据前打开 `tests/rhi/texture_quad_test.cpp` 核对顶点布局
(stride/uv offset)与 quad 数组,以其为准修正上面的 `vertexBindings`/`attributes`/quad。

- [ ] **Step 2: 跑测试确认失败**

Run: `./scripts/check.sh 2>&1 | tail -5`
Expected: 编译错误(`targetColorTexture`/`targetSize` 不是 Device 成员)

- [ ] **Step 3: Device 接口**

`core/rhi/rhi_device.h` 的 Device「渲染目标」组内追加:

```cpp
  /// 查询目标尺寸(离屏/texture-backed/swapchain 目标均有效)。
  virtual void targetSize(TargetHandle target, uint32_t& outW, uint32_t& outH) const = 0;
  /// 目标的可采样颜色纹理:MSAA 目标返回 resolve 纹理;texture-backed 返回源纹理;
  /// swapchain 目标/无效句柄返回无效 TextureHandle。
  /// 句柄生命周期随目标(destroyTarget 后失效,勿 destroyTexture)。
  virtual TextureHandle targetColorTexture(TargetHandle target) = 0;
```

- [ ] **Step 4: Vulkan 实现**

`vulkan_device.cpp`:
1. `TargetRec` 追加字段:`TextureHandle colorTex;`(自建路径注册的可采样句柄)。
2. `createOffscreenTarget` 自建路径:
   - color 图像 `createImage` 的 usage 追加 `| VK_IMAGE_USAGE_SAMPLED_BIT`。
   - view 创建成功后,注册纹理句柄(颜色图像/视图归 TargetRec 所有,句柄仅引用):

```cpp
   TextureRec trec{};
   trec.image = rec.color;
   trec.view = rec.view;
   trec.width = desc.width;
   trec.height = desc.height;
   trec.mipLevels = 1;
   trec.format = desc.colorFormat;
   trec.faces = 1;
   trec.subLayouts = {VK_IMAGE_LAYOUT_UNDEFINED};
   rec.colorTex = TextureHandle(nextId_++);
   textures_.emplace(rec.colorTex, trec);
```

3. `destroyTarget` 自建路径 retire 前:`if (t.colorTex.valid()) textures_.erase(t.colorTex);`
   (只摘句柄,不销毁底层 image/view——由 TargetRec 退休闭包释放)
4. `endRenderPass`(vulkan_device.cpp 1641 起):
   - textureBacked 分支保持不变(已转 SHADER_READ)。
   - **swapchain 目标提前返回**(安全修正:staging 为 null,现路径拷贝无意义):

```cpp
   if (t.isSwapchain) return;
```

   - staging 拷贝完成之后(最后一个 barrier 把 staging 转 GENERAL 之后)追加:
     颜色图像 TRANSFER_SRC → SHADER_READ_ONLY(供后续采样;下帧 pass 的
     initialLayout=UNDEFINED 天然兼容任意入 layout):

```cpp
  // 颜色附件转 SHADER_READ_ONLY(可采样化);下帧 pass initialLayout=UNDEFINED 兼容
  VkImageMemoryBarrier toRead{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  toRead.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  toRead.image = t.color;
  toRead.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                       &toRead);
```

5. 两个接口:

```cpp
void targetSize(TargetHandle target, uint32_t& outW, uint32_t& outH) const override {
  auto it = targets_.find(target);
  outW = it != targets_.end() ? it->second.width : 0;
  outH = it != targets_.end() ? it->second.height : 0;
}
TextureHandle targetColorTexture(TargetHandle target) override {
  auto it = targets_.find(target);
  if (it == targets_.end() || it->second.isSwapchain) return {};
  const TargetRec& t = it->second;
  return t.textureBacked ? t.srcTexture : t.colorTex;
}
```

(声明加进类内既有 override 区;`targets_` 的 const 查找如编译报 const 问题,
把 targetColorTexture 声明去 const 或 targets_ 加 mutable——按现有代码风格调整)

- [ ] **Step 5: Metal 实现**

`metal_device.mm`:
1. `TargetRec` 追加:`TextureHandle colorHandle; TextureHandle srcTexture;`。
2. 自建路径:`rec.color = tex` 之后注册:

```objc
    TextureRec trec;
    trec.texture = tex;
    trec.isCube = false;
    trec.width = desc.width;
    trec.height = desc.height;
    trec.mipLevels = 1;
    trec.format = desc.colorFormat;
    rec.colorHandle = TextureHandle(nextId_++);
    textures_.emplace(rec.colorHandle, trec);
```

(Metal TextureRec 字段名以文件顶部现有定义为准微调)
3. textureBacked 路径补:`rec.srcTexture = desc.colorFromTexture;`
4. `destroyTarget`:`if (rec.colorHandle.valid()) textures_.erase(rec.colorHandle);`
5. 两个接口(targetColorTexture:textureBacked 返回 srcTexture,否则 colorHandle;
   swapchain 返回无效——Metal TargetRec 有 isSwapchain 标记则同样判断)。

- [ ] **Step 6: GLES 实现**

`gles_device.cpp`:
1. `TargetRec` 追加:`TextureHandle colorHandle; TextureHandle srcTexture;`。
2. 自建路径(`glFramebufferTexture2D` 之后)注册 colorTex 到 textures_:
   `TextureRec{rec.colorTex, GL_TEXTURE_2D, w, h, 1, desc.colorFormat}` → colorHandle。
   textureBacked 路径补 `rec.srcTexture = desc.colorFromTexture;`。
3. `destroyTarget`:`if (rec.colorHandle.valid()) textures_.erase(...)`(GL 纹理本体随
   target 的 glDeleteTextures 释放,句柄只摘表)。
4. 两个接口(同 Metal 语义)。

- [ ] **Step 7: 跑测试**

Run: `./scripts/check.sh 2>&1 | tail -8`
Expected: 全绿;`TargetTexture.Metal/Vulkan` 通过;既有 golden(cube/helmet/box 等)不回归
(Vulkan endRenderPass 多了一个 barrier——行为等价,pixel 不变;若有回归先查
barrier 的 stage/access 是否覆盖原拷贝语义)

- [ ] **Step 8: Commit**

```bash
git add core/rhi tests/rhi tests/CMakeLists.txt
git commit -m "feat(rhi): Device::targetColorTexture/targetSize + 离屏目标可采样化(三后端)"
```

---

### Task 4: Vulkan MSAA(render pass 缓存 + resolve)

**Files:**
- Modify: `core/rhi/backends/vulkan/vulkan_device.cpp`(render pass 缓存化 + createOffscreenTarget MSAA + createPipeline sampleCount 放开 + endRenderPass resolve 分支)
- Test: `tests/rhi/msaa_test.cpp`(新建,GLES 部分 Task 6 补)

**Files 说明:** MSAA 契约测试跨三后端,本任务先建文件跑 Metal/Vulkan;GLES 在 Task 6 实现后自动生效(Android 侧验证)。

- [ ] **Step 1: 写失败测试**

`tests/rhi/msaa_test.cpp`:

```cpp
// MSAA 契约:sampleCount=4 目标渲染斜边三角形,边缘覆盖率直方图应出现中间灰阶
// (单采样目标对照组应只有 0/255 两值);同时验证 targetColorTexture 返回 resolve 纹理。
#include <gtest/gtest.h>
#include "rhi/rhi_device.h"
#include <algorithm>
#include <set>
#include <vector>

namespace {
constexpr uint32_t kW = 128, kH = 128;

void runMsaa(rd::Backend b) {
  rd::DeviceDesc d;
  d.backend = b;
  auto dev = rd::createDevice(d);
  ASSERT_NE(dev, nullptr);
  if (dev->caps().get(rd::Capability::msaa) < 4) {
    GTEST_SKIP() << "后端 MSAA<4";
  }

  // cube shader 画一个斜边三角形(顶点色纯白,无纹理)
  // 注:直接用清屏+简单 shader;复用 tests/common 的 cube shader 会带旋转,
  // 这里用更小的内嵌 GLSL 不可行(离线管线),故用 texquad 的顶点直通思路:
  // 改用 cube.vert/cube.frag 但传单位 MVP,只画 1 个大三角形。
  // —— 简化:直接用 PipelineDesc 无顶点缓冲不可行;用 cube shader 需要 UBO。
  // 最终选择:复用 tests/rhi/cube_test.cpp 的渲染辅助(若有),否则用
  // texquad 管线画斜切 quad(几何边缘由顶点插值产生)。
  // 本测试采用:两个三角形顶点色 quad 覆盖左下半屏,斜边即 MSAA 采样区。
  auto loadShader = [&](const char* n) { return rd::test::loadShaderCode(b, RD_SHADER_DIR, n); };
  auto vs = loadShader("texquad.vert");
  auto fs = loadShader("texquad.frag");
  auto vsMod = dev->createShaderModule({rd::ShaderStage::Vertex, vs.code, vs.entry});
  auto fsMod = dev->createShaderModule({rd::ShaderStage::Fragment, fs.code, fs.entry});
  ASSERT_TRUE(vsMod.valid() && fsMod.valid());

  rd::PipelineDesc pd;
  pd.vertexShader = vsMod;
  pd.fragmentShader = fsMod;
  pd.vertexBindings = {{0, 20}};
  pd.attributes = {{0, rd::Format::R32G32B32_FLOAT, 0, 0},
                   {1, rd::Format::R32G32_FLOAT, 12, 0}};
  pd.sampleCount = 4;  // ← 本任务核心
  auto pipe4 = dev->createPipeline(pd);
  ASSERT_TRUE(pipe4.valid()) << "sampleCount=4 管线创建失败";

  rd::OffscreenTargetDesc od;
  od.width = kW;
  od.height = kH;
  od.sampleCount = 4;
  auto target = dev->createOffscreenTarget(od);
  ASSERT_TRUE(target.valid()) << "sampleCount=4 目标创建失败";

  // resolve 纹理可查询
  EXPECT_TRUE(dev->targetColorTexture(target).valid());

  // 斜切 quad:左下半(对角线斜边);uv 无所谓(texquad 采样 1x1 白纹理或
  // 无纹理时输出颜色——按 texquad.frag 实际行为,无绑纹理则采样结果未定义:
  // 绑一张 1x1 白纹理保证确定性)
  uint8_t white[4] = {255, 255, 255, 255};
  auto white1 = dev->createTexture({rd::TextureType::Texture2D, rd::TextureUsage::Sampled,
                                    1, 1, rd::Format::RGBA8_UNORM, 1, white, 4});
  ASSERT_TRUE(white1.valid());
  auto sampler = dev->createSampler({});
  const float quad[6 * 5] = {-1, -1, 0, 0, 0, 1, 1, 0, 1, 0, -1, 1, 0, 0, 1,
                             -1, -1, 0, 0, 0, 1, 1, 0, 1, 1, -1, 1, 0, 0, 1};
  auto vbo = dev->createBuffer({sizeof(quad), rd::BufferUsage::Vertex, false, false, quad});
  ASSERT_TRUE(vbo.valid());

  auto* cmd = dev->acquireCommandBuffer();
  cmd->beginRenderPass(target, {0, 0, 0, 1});
  cmd->bindPipeline(pipe4);
  cmd->bindVertexBuffer(0, vbo, 0);
  cmd->bindTexture(0, white1, sampler);
  cmd->draw(6, 0);
  cmd->endRenderPass();
  dev->submit(cmd);
  dev->waitIdle();

  std::vector<uint8_t> px(size_t(kW) * kH * 4);
  ASSERT_TRUE(dev->readbackTarget(target, px.data(), px.size()));
  // MSAA 特征:斜边附近存在中间灰阶(非 0/255)
  std::set<int> levels;
  for (size_t i = 0; i < px.size(); i += 4) levels.insert(px[i]);
  bool hasMid =
      std::any_of(levels.begin(), levels.end(), [](int v) { return v > 16 && v < 239; });
  EXPECT_TRUE(hasMid) << "无中间灰阶,MSAA 未生效( levels=" << levels.size() << " )";

  dev->destroyBuffer(vbo);
  dev->destroySampler(sampler);
  dev->destroyTexture(white1);
  dev->destroyTarget(target);
  dev->destroyPipeline(pipe4);
  dev->destroyShaderModule(vsMod);
  dev->destroyShaderModule(fsMod);
}

// 非法:sampleCount 超 caps 必须拒绝
void runReject(rd::Backend b) {
  rd::DeviceDesc d;
  d.backend = b;
  auto dev = rd::createDevice(d);
  ASSERT_NE(dev, nullptr);
  const uint32_t msaa = dev->caps().get(rd::Capability::msaa);
  if (msaa == 0) GTEST_SKIP() << "后端无 MSAA caps";
  rd::OffscreenTargetDesc od;
  od.width = 16;
  od.height = 16;
  od.sampleCount = msaa * 2;  // 超 caps 必须拒绝
  EXPECT_FALSE(dev->createOffscreenTarget(od).valid());
}
} // namespace

TEST(Msaa, Metal) {
#if defined(__APPLE__)
  runMsaa(rd::Backend::Metal);
  runReject(rd::Backend::Metal);
#endif
}
TEST(Msaa, Vulkan) {
#if defined(RD_WITH_VULKAN)
  runMsaa(rd::Backend::Vulkan);
  runReject(rd::Backend::Vulkan);
#endif
}
```

`tests/CMakeLists.txt` 追加 `rhi/msaa_test.cpp`(文件头 `#include "common/shader_code.h"`
与 `#include "rd_shader_dir.h"` 别漏)。

注意:texquad 顶点布局/quad 数据以 `tests/rhi/texture_quad_test.cpp` 为准修正。

- [ ] **Step 2: 跑测试确认失败**

Run: `./scripts/check.sh 2>&1 | tail -5`
Expected: 编译错(`OffscreenTargetDesc` 无 sampleCount 成员)

- [ ] **Step 3: OffscreenTargetDesc 加 sampleCount**

`core/rhi/rhi_types.h` 的 `OffscreenTargetDesc` 追加:

```cpp
  /// MSAA 采样数(默认 1);>1 时创建 MSAA 颜色附件 + 单采样 resolve 纹理,
  /// pass 结束自动 resolve;须 ≤ caps().get(Capability::msaa)。
  uint32_t sampleCount = 1;
```

- [ ] **Step 4: Vulkan render pass 缓存**

`vulkan_device.cpp`:
1. 删除 `renderPass_`/`renderPassDepth_` 成员与 `renderPass()/renderPassDepth()` 访问器,
   替换为缓存:

```cpp
struct RenderPassKey {
  VkFormat format;
  bool depth;
  uint32_t samples;
  bool operator<(const RenderPassKey& o) const {
    if (format != o.format) return format < o.format;
    if (depth != o.depth) return depth < o.depth;
    return samples < o.samples;
  }
};
std::map<RenderPassKey, VkRenderPass> renderPasses_;  // 成员
/// 按 (格式,深度,采样数) find-or-create render pass;samples>1 时带 resolve 附件。
VkRenderPass findOrCreateRenderPass(VkFormat format, bool depth, uint32_t samples);
```

2. `createRenderPass` 改为带 samples 参数的实现(挂到 findOrCreateRenderPass):

```cpp
VkRenderPass VulkanDevice::findOrCreateRenderPass(VkFormat format, bool depth,
                                                  uint32_t samples) {
  const RenderPassKey key{format, depth, samples};
  auto it = renderPasses_.find(key);
  if (it != renderPasses_.end()) return it->second;

  const VkSampleCountFlagBits vkSamples = VkSampleCountFlagBits(samples);
  VkAttachmentDescription color{};
  color.format = format;
  color.samples = vkSamples;
  color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  // MSAA 颜色内容 resolve 后即弃;单采样须 STORE(staging 拷贝/readback 依赖)
  color.storeOp = samples > 1 ? VK_ATTACHMENT_STORE_OP_DONT_CARE
                              : VK_ATTACHMENT_STORE_OP_STORE;
  color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  color.finalLayout = samples > 1 ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                                  : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

  VkAttachmentDescription depthAtt{};
  depthAtt.format = VK_FORMAT_D32_SFLOAT;
  depthAtt.samples = vkSamples;
  depthAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  depthAtt.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  depthAtt.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  depthAtt.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  depthAtt.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  depthAtt.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

  VkAttachmentDescription resolve{};
  resolve.format = format;
  resolve.samples = VK_SAMPLE_COUNT_1_BIT;
  resolve.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  resolve.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  resolve.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  resolve.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  resolve.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  resolve.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;  // staging 拷贝源

  VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkAttachmentReference depthRef{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
  VkAttachmentReference resolveRef{depth ? 2u : 1u, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};

  VkSubpassDescription subpass{};
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount = 1;
  subpass.pColorAttachments = &colorRef;
  if (depth) subpass.pDepthStencilAttachment = &depthRef;
  if (samples > 1) subpass.pResolveAttachments = &resolveRef;

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

  VkAttachmentDescription atts[3] = {color, depthAtt, resolve};
  VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
  rpci.attachmentCount = samples > 1 ? (depth ? 3u : 2u) : (depth ? 2u : 1u);
  // 附件数组顺序:0=color,1=depth 或 resolve,2=resolve(带 depth 的 MSAA)
  if (samples > 1 && !depth) atts[1] = resolve;
  rpci.pAttachments = atts;
  rpci.subpassCount = 1;
  rpci.pSubpasses = &subpass;
  rpci.dependencyCount = 2;
  rpci.pDependencies = deps;
  VkRenderPass rp = VK_NULL_HANDLE;
  VK_CHECK(vkCreateRenderPass(device_, &rpci, nullptr, &rp));
  renderPasses_.emplace(key, rp);
  renderPassFormat_ = format;  // 保留既有语义(swapchain 对齐用)
  return rp;
}
```

注意附件下标:samples>1 && !depth 时附件为 [color, resolve](resolveRef 下标 1);
samples>1 && depth 时 [color, depth, resolve](resolveRef 下标 2)——上面代码已处理。

3. init 里两处 `createRenderPass(...)` 调用改为 `findOrCreateRenderPass(VK_FORMAT_R8G8B8A8_UNORM, false, 1)` / `(…, true, 1)`(保持启动行为;返回句柄不再需要存成员)。析构里 `vkDestroyRenderPass(renderPass_)` 等两处改为遍历缓存销毁:

```cpp
  for (auto& kv : renderPasses_) vkDestroyRenderPass(device_, kv.second, nullptr);
  renderPasses_.clear();
```

4. `createSwapChain` 中"表面格式不一致时重建 render pass"的逻辑改为直接
   `findOrCreateRenderPass(surfaceFormat, false, 1)`(缓存幂等,无需销毁旧 pass);
   `buildSwapChainTargets` 的 `fbci.renderPass = renderPass_` 改为
   `findOrCreateRenderPass(renderPassFormat_, false, 1)`。

- [ ] **Step 5: Vulkan createPipeline 放开 sampleCount**

删除"MSAA 为 P2 预留"拒绝分支,替换为:

```cpp
  const uint32_t maxMsaa = caps_.get(Capability::msaa);
  if (desc.sampleCount == 0 || desc.sampleCount > maxMsaa) {
    RD_LOGE("rhi.vk", "sampleCount %u 超出 caps %u", desc.sampleCount, maxMsaa);
    return {};
  }
```

管线创建处:
- `VkPipelineMultisampleStateCreateInfo` 的 `rasterizationSamples` 用
  `VkSampleCountFlagBits(desc.sampleCount)`(现在应是固定 1,改掉)。
- `gpci.renderPass = useDepth ? renderPassDepth_ : renderPass_` 改为
  `findOrCreateRenderPass(toVkFormat(desc.colorFormat), useDepth, desc.sampleCount)`。

- [ ] **Step 6: Vulkan createOffscreenTarget MSAA 路径**

入口校验:

```cpp
  if (desc.sampleCount > 1) {
    const uint32_t maxMsaa = caps_.get(Capability::msaa);
    if (desc.sampleCount > maxMsaa) {
      RD_LOGE("rhi.vk", "MSAA 目标 sampleCount %u 超出 caps %u", desc.sampleCount, maxMsaa);
      return {};
    }
    if (desc.colorFromTexture.valid()) {
      RD_LOGE("rhi.vk", "texture-backed 目标不支持 MSAA");
      return {};
    }
  }
```

`TargetRec` 追加:`uint32_t samples = 1; VkImage msaaColor; VkDeviceMemory msaaColorMem; VkImageView msaaView;`

自建路径在 `desc.sampleCount > 1` 时:
1. MSAA 颜色图像(samples=N,usage=COLOR_ATTACHMENT,storeOp 由 pass 决定为 DONT_CARE):

```cpp
    rec.samples = desc.sampleCount;
    // MSAA 颜色图像(不注册纹理句柄,不可采样)
    {
      VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
      ici.imageType = VK_IMAGE_TYPE_2D;
      ici.format = toVkFormat(desc.colorFormat);
      ici.extent = {desc.width, desc.height, 1};
      ici.mipLevels = 1;
      ici.arrayLayers = 1;
      ici.samples = VkSampleCountFlagBits(desc.sampleCount);
      ici.tiling = VK_IMAGE_TILING_OPTIMAL;
      ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
      ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      VK_CHECK(vkCreateImage(device_, &ici, nullptr, &rec.msaaColor));
      VkMemoryRequirements req;
      vkGetImageMemoryRequirements(device_, rec.msaaColor, &req);
      VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
      mai.allocationSize = req.size;
      mai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
      if (mai.memoryTypeIndex == UINT32_MAX) return {};
      VK_CHECK(vkAllocateMemory(device_, &mai, nullptr, &rec.msaaColorMem));
      VK_CHECK(vkBindImageMemory(device_, rec.msaaColor, rec.msaaColorMem, 0));
      VkImageViewCreateInfo mvci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
      mvci.image = rec.msaaColor;
      mvci.viewType = VK_IMAGE_VIEW_TYPE_2D;
      mvci.format = toVkFormat(desc.colorFormat);
      mvci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      if (vkCreateImageView(device_, &mvci, nullptr, &rec.msaaView) != VK_SUCCESS) return {};
    }
```

2. resolve 目标 = 原 color 图像路径不变(单采样,SAMPLED|TRANSFER_SRC|COLOR_ATTACHMENT;
   已注册 colorTex 句柄——Task 3 的注册逻辑对 MSAA 目标同样生效,colorTex 即 resolve 纹理)。
3. depth(desc.depth 时):图像 samples 同样改为 N(createImage 加 samples 参数或内联
   创建;注意 createImage 辅助函数 samples 固定 1——给它加 `uint32_t samples = 1` 参数,
   内部 `ici.samples = VkSampleCountFlagBits(samples)`)。
4. framebuffer 附件数组:MSAA 时 `views = {msaaView, depthView?, rec.view}`,顺序与
   render pass 附件一致;color/depth 用 MSAA view,resolve 用 rec.view;
   `fbci.renderPass = findOrCreateRenderPass(toVkFormat(desc.colorFormat), desc.depth, desc.sampleCount)`。
   非 MSAA 路径保持 `{rec.view, rec.depthView}` 与 `findOrCreateRenderPass(format, depth, 1)`。

5. `destroyTarget` 退休闭包追加 MSAA 资源:

```cpp
      if (t.msaaView) vkDestroyImageView(device_, t.msaaView, nullptr);
      if (t.msaaColor) vkDestroyImage(device_, t.msaaColor, nullptr);
      if (t.msaaColorMem) vkFreeMemory(device_, t.msaaColorMem, nullptr);
```

- [ ] **Step 7: Vulkan beginRenderPass/endRenderPass 适配**

1. `beginRenderPass`:`rp.renderPass = t.hasDepth ? device_->renderPassDepth() : device_->renderPass()`
   改为:

```cpp
  rp.renderPass = device_->findOrCreateRenderPass(
      t.isSwapchain ? device_->swapChainFormat() /* 见下注 */
                    : toVkFormat(t.format),
      t.hasDepth, t.samples);
```

   (TargetRec 需补 `Format format = Format::RGBA8_UNORM;` 字段并在各自建/swapchain 路径填;
   swapchain 目标填 renderPassFormat_ 对应 Format。swapChainFormat() 可不加——直接用
   t.format。)
   clear 值:`VkClearValue clears[3]`;`rp.clearValueCount = t.samples > 1 ? (t.hasDepth ? 3 : 2) : (t.hasDepth ? 2 : 1)`;
   clears[0]=color、clears[1]=depth(或 MSAA 无深度时的 resolve 占位)、clears[2]=resolve 占位。
   注意与 render pass 附件顺序一致:[color, depth?, resolve] / MSAA 无 depth 时 [color, resolve]。

2. `endRenderPass`:staging 拷贝的源图像 MSAA 时用 resolve(即 rec.color——MSAA 路径下
   rec.color 就是单采样 resolve 图像,**拷贝源代码无需改**);SHADER_READ 转换同样作用
   于 rec.color(Task 3 已加)。即:endRenderPass 对 MSAA 零改动(pass 的 resolve 在
   vkCmdEndRenderPass 时自动发生)。

- [ ] **Step 8: 跑测试**

Run: `./scripts/check.sh 2>&1 | tail -8`
Expected: `Msaa.Vulkan` 通过(Metal 用例此时仍失败——Step 9 前注释掉?**不**:
Metal createPipeline 仍拒绝 sampleCount≠1 → `Msaa.Metal` 断言失败。处理方式:
本任务提交前先把 `Msaa.Metal` 用 `#if 0` 或 GTEST_SKIP 临时跳过,Task 5 完成后再启用。
**采用:测试里 Metal 用例加 `if (!msaaMetalDone) GTEST_SKIP()` 太丑——直接在本任务的
metal_device.mm 里把 createPipeline 的 sampleCount 拒绝改为 caps 校验放行(pipeline 侧先放行,
createOffscreenTarget 的 MSAA 在 Task 5 做;`Msaa.Metal` 在 Task 5 才启用,本任务中该用例
临时 `#if defined(RD_MSAA_METAL_DONE)` 包裹,Task 5 Step 1 解开。**)

简化决策:本任务测试中 `Msaa.Metal` 整体先 `#if 0 ... #endif`,Task 5 恢复。

Run again: `./scripts/check.sh 2>&1 | tail -8`
Expected: 全绿;既有 golden 不回归(render pass 缓存化后单采样 pass 结构不变)

- [ ] **Step 9: Commit**

```bash
git add core/rhi tests/rhi/msaa_test.cpp tests/CMakeLists.txt
git commit -m "feat(rhi): Vulkan MSAA(render pass 缓存 + resolve 附件 + sampleCount 放开)"
```

---

### Task 5: Metal MSAA

**Files:**
- Modify: `core/rhi/backends/metal/metal_device.mm`
- Modify: `tests/rhi/msaa_test.cpp`(解开 Metal 用例)

- [ ] **Step 1: 解开 Metal 用例**

`tests/rhi/msaa_test.cpp` 中 `TEST(Msaa, Metal)` 的 `#if 0` 去掉(保留 `#if defined(__APPLE__)`)。

Run: `./scripts/check.sh 2>&1 | tail -5`
Expected: `Msaa.Metal` 失败(创建失败或断言)

- [ ] **Step 2: Metal 实现**

`metal_device.mm`:
1. `TargetRec` 追加:`uint32_t samples = 1; id<MTLTexture> msaaColor; id<MTLTexture> msaaDepth;`。
2. `createOffscreenTarget` 入口校验(同 Vulkan:超 caps 拒绝、textureBacked+MSAA 拒绝)。
3. 自建路径 `desc.sampleCount > 1` 时:

```objc
    rec.samples = desc.sampleCount;
    MTLTextureDescriptor* md = [MTLTextureDescriptor
        texture2DMultisampleDescriptorWithPixelFormat:toMTLPixelFormat(desc.colorFormat)
                                                width:desc.width
                                               height:desc.height
                                        sampleCount:desc.sampleCount];
    md.usage = MTLTextureUsageRenderTarget;
    md.storageMode = MTLStorageModePrivate;
    rec.msaaColor = [device_ newTextureWithDescriptor:md];
    if (!rec.msaaColor) return {};
    // 颜色纹理(rec.color,Shared,RenderTarget|ShaderRead)照旧创建——作为 resolve 目标
    if (desc.depth) {  // 深度同步 MSAA(替换单采样 depth 的创建)
      MTLTextureDescriptor* mdd = [MTLTextureDescriptor
          texture2DMultisampleDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                  width:desc.width
                                                 height:desc.height
                                          sampleCount:desc.sampleCount];
      mdd.usage = MTLTextureUsageRenderTarget;
      mdd.storageMode = MTLStorageModePrivate;
      rec.msaaDepth = [device_ newTextureWithDescriptor:mdd];
      if (!rec.msaaDepth) return {};
      rec.hasDepth = true;
    }
```

(depth 为 MSAA 时不再创建单采样 rec.depth;原 desc.depth 分支包一层 `if (desc.sampleCount > 1) {...} else {现有逻辑}`)
4. `beginRenderPass` 适配:

```objc
  if (t.samples > 1) {
    rp.colorAttachments[0].texture = t.msaaColor;
    rp.colorAttachments[0].resolveTexture = t.color;
    rp.colorAttachments[0].storeAction = MTLStoreActionMultisampleResolve;
    if (t.hasDepth) rp.depthAttachment.texture = t.msaaDepth;
  } else {
    rp.colorAttachments[0].texture = t.color;  // 现有逻辑(textureBacked slice/level 保持)
    ...
  }
```

5. `createPipeline`:删除"MSAA 为 P2 预留"拒绝,改 caps 校验(同 Vulkan),
   `pd.rasterSampleCount = desc.sampleCount;`(MTLRenderPipelineDescriptor)。
6. `destroyTarget`:无需特判(ARC 随 TargetRec 释放;retire 闭包已按值持有)。

- [ ] **Step 3: 跑测试**

Run: `./scripts/check.sh 2>&1 | tail -8`
Expected: 全绿(含 `Msaa.Metal/Vulkan`);既有 golden 不回归

- [ ] **Step 4: Commit**

```bash
git add core/rhi/backends/metal/metal_device.mm tests/rhi/msaa_test.cpp
git commit -m "feat(rhi): Metal MSAA(multisample 纹理 + MultisampleResolve + rasterSampleCount)"
```

---

### Task 6: GLES MSAA + 契约测试收尾

**Files:**
- Modify: `core/rhi/backends/gles/gles_device.cpp`

说明:GLES 仅 Android 编译,host 无测试覆盖;本任务靠 Android 构建 + 模拟器手势 demo
(Task 17)间接验证,代码评审为主。MSAA 契约测试文件已在 Task 4 建好(GLES 分支
host 编译不到,无需改动)。

- [ ] **Step 1: GLES 实现**

`gles_device.cpp`:
1. `TargetRec` 追加:`uint32_t samples = 1; GLuint msaaColorRbo = 0; GLuint resolveFbo = 0;`。
2. `createOffscreenTarget` 入口校验(超 caps 拒绝、textureBacked+MSAA 拒绝)。
3. 自建路径 `desc.sampleCount > 1` 时:

```cpp
    rec.samples = desc.sampleCount;
    // MSAA 颜色 renderbuffer 绑 rec.fbo
    glGenRenderbuffers(1, &rec.msaaColorRbo);
    glBindRenderbuffer(GL_RENDERBUFFER, rec.msaaColorRbo);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, GLsizei(desc.sampleCount), GL_RGBA8,
                                     GLsizei(desc.width), GLsizei(desc.height));
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER,
                              rec.msaaColorRbo);
    // 深度:MSAA renderbuffer(替换单采样分支)
    if (desc.depth) {
      glGenRenderbuffers(1, &rec.depthRbo);
      glBindRenderbuffer(GL_RENDERBUFFER, rec.depthRbo);
      glRenderbufferStorageMultisample(GL_RENDERBUFFER, GLsizei(desc.sampleCount),
                                       GL_DEPTH_COMPONENT24, GLsizei(desc.width),
                                       GLsizei(desc.height));
      glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER,
                                rec.depthRbo);
      rec.hasDepth = true;
    }
    // resolve 目标:colorTex 挂到 resolveFbo
    glGenFramebuffers(1, &rec.resolveFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, rec.resolveFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rec.colorTex, 0);
    // 两个 FBO 完整性检查,任一失败清理返回 {}
```

(colorTex 创建/注册 colorHandle 逻辑与单采样共用——先建 colorTex,再按 samples 分支
决定它挂到 rec.fbo 还是 rec.resolveFbo)
4. `GLESCommandBuffer`:加 `TargetHandle currentTarget_` 成员;beginRenderPass 里记录;
   `endRenderPass()` 不再是空实现:

```cpp
  void endRenderPass() override {
    TargetRec t;
    if (!device_->target(currentTarget_, t)) return;
    if (t.samples <= 1) return;
    const GLuint src = t.fbo, dst = t.resolveFbo;
    const uint32_t w = t.width, h = t.height;
    cmds_.push_back([src, dst, w, h] {
      glBindFramebuffer(GL_READ_FRAMEBUFFER, src);
      glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dst);
      glBlitFramebuffer(0, 0, GLint(w), GLint(h), 0, 0, GLint(w), GLint(h),
                        GL_COLOR_BUFFER_BIT, GL_NEAREST);
      glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
      glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    });
  }
```

(cmds_ 的实际容器名/类型以文件中延迟回放实现为准;device_->target 访问器名同理)
5. `readbackTarget`:`glBindFramebuffer(GL_FRAMEBUFFER, t.samples > 1 ? t.resolveFbo : t.fbo)`。
6. `createPipeline`:删除"MSAA 预留"拒绝(若有;GLES 管线无 MSAA 状态,由 FBO 决定),
   仅保留 `desc.sampleCount <= caps` 校验。
7. `destroyTarget`:追加 `glDeleteRenderbuffers(1, &rec.msaaColorRbo)` 与
   `glDeleteFramebuffers(1, &rec.resolveFbo)`(0 值安全)。

- [ ] **Step 2: 编译验证(Android 交叉编译)**

Run: `source /tmp/rd_env.sh && cd samples/android && ./gradlew :app:assembleDebug 2>&1 | tail -5`
Expected: BUILD SUCCESSFUL(GLES 代码编译通过;运行验证在 Task 17/19)

- [ ] **Step 3: Commit**

```bash
git add core/rhi/backends/gles/gles_device.cpp
git commit -m "feat(rhi): GLES MSAA(multisample RBO + endRenderPass blit resolve)"
```

---

### Task 7: libktx 接入 + ktx2_codec + 测试资产生成器

**Files:**
- Modify: `cmake/Deps.cmake`(FetchContent KTX-Software)
- Create: `core/resource/ktx2_codec.h`、`core/resource/ktx2_codec.cpp`
- Modify: `core/CMakeLists.txt`(新源文件 + 链接 ktx)
- Create: `tests/common/ktx2_gen.h`、`tests/common/ktx2_gen.cpp`(运行时生成测试用 ktx2)
- Test: `tests/resource/ktx2_test.cpp`(新建)、`tests/CMakeLists.txt`(注册两个新文件)

- [ ] **Step 1: Deps.cmake 接入**

`cmake/Deps.cmake` 全平台区(stb/cgltf 之后)追加:

```cmake
# KTX-Software:KTX2/BasisU 解码与转码(裁剪:无 tools/tests/doc,静态库)
set(KTX_FEATURE_TOOLS OFF CACHE BOOL "" FORCE)
set(KTX_FEATURE_TESTS OFF CACHE BOOL "" FORCE)
set(KTX_FEATURE_DOC OFF CACHE BOOL "" FORCE)
set(KTX_FEATURE_LOADTEST_APPS OFF CACHE BOOL "" FORCE)
set(KTX_FEATURE_STATIC_LIBRARY ON CACHE BOOL "" FORCE)
FetchContent_Declare(ktx
  URL https://github.com/KhronosGroup/KTX-Software/archive/refs/tags/v4.3.2.tar.gz)
FetchContent_MakeAvailable(ktx)
```

`core/CMakeLists.txt`:
- `add_library(rd_core STATIC ...)` 列表追加 `resource/ktx2_codec.cpp`
- `target_link_libraries(rd_core PUBLIC glm::glm)` 后追加:

```cmake
target_link_libraries(rd_core PRIVATE ktx)
target_include_directories(rd_core PRIVATE ${ktx_SOURCE_DIR}/include)
```

(iOS 编译若 KTX-Software 的 CMake 对 toolchain 报错,按报错关 `KTX_FEATURE_VK_UPLOAD`
之类平台相关开关;host/Android 先行,iOS 在 Task 16 验证)

- [ ] **Step 2: 写失败测试**

`tests/common/ktx2_gen.h`:

```cpp
// 运行时生成确定性测试用 KTX2(BasisU ETC1S 压缩,8x8,2 mip,红绿棋盘)。
// 供 ktx2_test / gltf_test / KTX2 golden 复用,避免提交二进制资产。
#pragma once
#include <cstdint>
#include <vector>
namespace rd::test {
/// 生成 ktx2 文件到 path;成功(编码+写盘)返回 true。失败记日志返回 false。
bool writeTestKtx2(const char* path, uint32_t size = 8);
/// 同参数直接生成到内存(供 decodeKtx2 内存路径测试)。
std::vector<uint8_t> makeTestKtx2(uint32_t size = 8);
} // namespace rd::test
```

`tests/common/ktx2_gen.cpp`:

```cpp
#include "common/ktx2_gen.h"
#include "foundation/log.h"
#include <ktx.h>
#include <cstdio>
#include <vector>

namespace rd::test {

std::vector<uint8_t> makeTestKtx2(uint32_t size) {
  std::vector<uint8_t> out;
  ktxTextureCreateInfo ci{};
  ci.vkFormat = 37;  // VK_FORMAT_R8G8B8A8_UNORM(避免引 vulkan 头)
  ci.baseWidth = size;
  ci.baseHeight = size;
  ci.baseDepth = 1;
  ci.numDimensions = 2;
  ci.numLevels = 2;  // size 与 size/2 两级(第 2 级数据也要填)
  ci.numLayers = 1;
  ci.numFaces = 1;
  ci.isArray = KTX_FALSE;
  ci.generateMipmaps = KTX_FALSE;
  ktxTexture2* tex = nullptr;
  if (ktxTexture2_Create(&ci, KTX_TEXTURE_CREATE_ALLOC_STORAGE, &tex) != KTX_SUCCESS)
    return out;
  // 确定性图案:level0 = 红绿棋盘,level1 = 纯蓝
  for (uint32_t level = 0; level < 2; ++level) {
    const uint32_t w = size >> level;
    std::vector<uint8_t> px(size_t(w) * w * 4);
    for (uint32_t y = 0; y < w; ++y)
      for (uint32_t x = 0; x < w; ++x) {
        uint8_t* p = px.data() + (size_t(y) * w + x) * 4;
        if (level == 0) {
          p[0] = ((x ^ y) & 1) ? 220 : 30;  // 红通道棋盘
          p[1] = ((x ^ y) & 1) ? 30 : 220;  // 绿通道反相
        } else {
          p[2] = 255;
        }
        p[3] = 255;
      }
    if (ktxTexture_SetImageFromMemory(ktxTexture(tex), level, 0, 0, px.data(),
                                      px.size()) != KTX_SUCCESS) {
      ktxTexture2_Destroy(tex);
      return out;
    }
  }
  // ETC1S 压缩(确定性编码;若库配置不含编码器,此处失败 → 测试应跳过而非崩溃)
  // 注:若 ktx.h 中签名不符(版本差异),备选 ktxTexture2_CompressBasisEx(tex, &params)
  if (ktxTexture2_CompressBasis(tex, 0) != KTX_SUCCESS) {
    RD_LOGW("test.ktx2", "CompressBasis 不可用,退化为未压缩 ktx2");
  }
  ktx_uint8_t* bytes = nullptr;
  ktx_size_t len = 0;
  if (ktxTexture_WriteToMemory(ktxTexture(tex), &bytes, &len) == KTX_SUCCESS)
    out.assign(bytes, bytes + len);
  ktxTexture2_Destroy(tex);
  return out;
}

bool writeTestKtx2(const char* path, uint32_t size) {
  const auto bytes = makeTestKtx2(size);
  if (bytes.empty()) return false;
  FILE* f = fopen(path, "wb");
  if (!f) return false;
  const bool ok = fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
  fclose(f);
  return ok;
}

} // namespace rd::test
```

`tests/resource/ktx2_test.cpp`:

```cpp
// ktx2_codec 单测:魔数探测 / 转码目标选择 / 内存解码(round-trip:libktx 现场编码)。
#include <gtest/gtest.h>
#include "common/ktx2_gen.h"
#include "resource/ktx2_codec.h"

TEST(Ktx2, MagicDetect) {
  const uint8_t good[12] = {0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32,
                            0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A};
  EXPECT_TRUE(rd::isKtx2(good, sizeof(good)));
  const uint8_t bad[12] = {0x89, 0x50, 0x4E, 0x47};  // PNG 头
  EXPECT_FALSE(rd::isKtx2(bad, sizeof(bad)));
  EXPECT_FALSE(rd::isKtx2(good, 4));  // 太短
}

TEST(Ktx2, PickTarget) {
  EXPECT_EQ(rd::pickTranscodeTarget(true, true), rd::Ktx2Target::Astc);
  EXPECT_EQ(rd::pickTranscodeTarget(false, true), rd::Ktx2Target::Etc2);
  EXPECT_EQ(rd::pickTranscodeTarget(false, false), rd::Ktx2Target::Rgba32);
}

TEST(Ktx2, DecodeRoundtripRgba32) {
  const auto bytes = rd::test::makeTestKtx2(8);
  ASSERT_FALSE(bytes.empty());
  auto img = rd::decodeKtx2(bytes.data(), bytes.size(), rd::Ktx2Target::Rgba32);
  ASSERT_EQ(img.width, 8u);
  ASSERT_EQ(img.height, 8u);
  EXPECT_EQ(img.mipLevels, 2u);
  EXPECT_EQ(img.format, rd::Format::RGBA8_UNORM);
  // 2 mip:8x8x4 + 4x4x4 = 320B
  EXPECT_EQ(img.data.size(), 320u);
}

TEST(Ktx2, DecodeRoundtripAstc) {
  const auto bytes = rd::test::makeTestKtx2(8);
  ASSERT_FALSE(bytes.empty());
  auto img = rd::decodeKtx2(bytes.data(), bytes.size(), rd::Ktx2Target::Astc);
  if (img.width == 0) GTEST_SKIP() << "未压缩 ktx2(编码器缺失),ASTC 路径不适用";
  EXPECT_EQ(img.format, rd::Format::ASTC_4x4_UNORM);
  // ASTC 4x4:8x8=64B,4x4=16B → 80B
  EXPECT_EQ(img.data.size(), 80u);
}
```

`tests/CMakeLists.txt` 追加 `resource/ktx2_test.cpp`、`common/ktx2_gen.cpp`;
`target_link_libraries(rd_tests PRIVATE rd_core GTest::gtest_main)` 改为
`rd_tests PRIVATE rd_core GTest::gtest_main ktx`,并
`target_include_directories(rd_tests PRIVATE ... ${ktx_SOURCE_DIR}/include)`。

- [ ] **Step 3: 跑测试确认失败**

Run: `./scripts/check.sh 2>&1 | tail -5`
Expected: 编译错误(`resource/ktx2_codec.h` 不存在)

- [ ] **Step 4: 实现 ktx2_codec**

`core/resource/ktx2_codec.h`:

```cpp
/**
 * @file ktx2_codec.h
 * @brief KTX2/BasisU 解码(libktx 封装):内存 → 逐 mip 紧凑图像数据。
 * 转码目标由调用方按 device caps 推导(astc > etc2 > rgba32 兜底);
 * 本模块不碰 GPU,纯 CPU 解码。
 */
#pragma once
#include "rhi/rhi_types.h"
#include <cstdint>
#include <vector>

namespace rd {

/// KTX2 转码目标。
enum class Ktx2Target { Astc, Etc2, Rgba32 };

/// 按能力选转码目标:astc > etc2 > rgba32 兜底。
Ktx2Target pickTranscodeTarget(bool astc, bool etc2);

/// 解码后的 KTX2 图像:逐 mip 紧凑排列(与 TextureDesc::data 布局一致);
/// 压缩格式时 data 为 block 数据(formatMipBytes 对齐)。
struct Ktx2Image {
  Format format = Format::RGBA8_UNORM;
  uint32_t width = 0, height = 0;
  uint32_t mipLevels = 1;
  std::vector<uint8_t> data;
};

/// 内存解码 KTX2;BasisU supercompressed 则 transcode 到 target。
/// 失败(非 KTX2/损坏/不支持的 vkFormat)返回 width==0 并记日志。
Ktx2Image decodeKtx2(const void* data, uint64_t size, Ktx2Target target);

/// KTX2 魔数探测(«KTX 20»)。
bool isKtx2(const void* data, uint64_t size);

} // namespace rd
```

`core/resource/ktx2_codec.cpp`:

```cpp
#include "resource/ktx2_codec.h"
#include "foundation/log.h"
#include <ktx.h>
#include <algorithm>
#include <cstring>

namespace rd {

bool isKtx2(const void* data, uint64_t size) {
  static const uint8_t kMagic[12] = {0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32,
                                     0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A};
  return size >= 12 && memcmp(data, kMagic, 12) == 0;
}

Ktx2Target pickTranscodeTarget(bool astc, bool etc2) {
  if (astc) return Ktx2Target::Astc;
  if (etc2) return Ktx2Target::Etc2;
  return Ktx2Target::Rgba32;
}

namespace {
/// 非 supercompressed KTX2 的 vkFormat → rhi Format;不支持返回 false。
bool mapVkFormat(uint32_t vkFormat, Format& out) {
  switch (vkFormat) {
    case 37: out = Format::RGBA8_UNORM; return true;          // VK_FORMAT_R8G8B8A8_UNORM
    case 43: out = Format::RGBA8_UNORM; return true;          // VK_FORMAT_R8G8B8A8_SRGB(按 UNORM 读)
    case 157: out = Format::ASTC_4x4_UNORM; return true;      // VK_FORMAT_ASTC_4x4_UNORM_BLOCK
    case 147: out = Format::ETC2_RGBA8_UNORM; return true;    // VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK
    default: return false;
  }
}
} // namespace

Ktx2Image decodeKtx2(const void* data, uint64_t size, Ktx2Target target) {
  Ktx2Image out;
  if (!isKtx2(data, size)) {
    RD_LOGE("resource.ktx2", "非 KTX2 魔数");
    return out;
  }
  ktxTexture2* tex = nullptr;
  if (ktxTexture2_CreateFromMemory(static_cast<const ktx_uint8_t*>(data), size,
                                   KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT,
                                   &tex) != KTX_SUCCESS) {
    RD_LOGE("resource.ktx2", "KTX2 解析失败");
    return out;
  }
  if (ktxTexture2_NeedsTranscoding(tex)) {
    ktx_transcode_fmt_e fmt;
    switch (target) {
      case Ktx2Target::Astc: fmt = KTX_TTF_ASTC_4x4_RGBA; out.format = Format::ASTC_4x4_UNORM; break;
      case Ktx2Target::Etc2: fmt = KTX_TTF_ETC2_RGBA; out.format = Format::ETC2_RGBA8_UNORM; break;
      default: fmt = KTX_TTF_RGBA32; out.format = Format::RGBA8_UNORM; break;
    }
    if (ktxTexture2_TranscodeBasis(tex, fmt, 0) != KTX_SUCCESS) {
      RD_LOGE("resource.ktx2", "BasisU 转码失败(target=%d)", int(target));
      ktxTexture2_Destroy(tex);
      return Ktx2Image{};
    }
  } else if (!mapVkFormat(tex->vkFormat, out.format)) {
    RD_LOGE("resource.ktx2", "不支持的 vkFormat %u", tex->vkFormat);
    ktxTexture2_Destroy(tex);
    return Ktx2Image{};
  }
  out.width = tex->baseWidth;
  out.height = tex->baseHeight;
  out.mipLevels = std::max(1u, tex->numLevels);
  // 逐 mip 紧凑收集
  uint64_t total = 0;
  for (uint32_t m = 0; m < out.mipLevels; ++m) {
    total += formatMipBytes(out.format, std::max(1u, out.width >> m),
                            std::max(1u, out.height >> m));
  }
  out.data.resize(total);
  uint64_t off = 0;
  const uint8_t* base = ktxTexture_GetData(ktxTexture(tex));
  for (uint32_t m = 0; m < out.mipLevels; ++m) {
    const uint32_t mw = std::max(1u, out.width >> m);
    const uint32_t mh = std::max(1u, out.height >> m);
    const uint64_t bytes = formatMipBytes(out.format, mw, mh);
    size_t imgOff = 0;
    ktxTexture_GetImageOffset(ktxTexture(tex), m, 0, 0, &imgOff);
    memcpy(out.data.data() + off, base + imgOff, bytes);
    off += bytes;
  }
  ktxTexture2_Destroy(tex);
  return out;
}

} // namespace rd
```

注意:ASTC/ETC2 转码结果要求设备支持——`decodeKtx2` 不做 caps 判断(由调用方
pickTranscodeTarget 时保证);若运行时不支持,后续 createTexture 会失败,
由 Task 8 的 loader 回落逻辑兜底重转 RGBA32。

- [ ] **Step 5: 跑测试**

Run: `./scripts/check.sh 2>&1 | tail -8`
Expected: 全绿(Ktx2.* 通过;若 `CompressBasis` 不可用,DecodeRoundtripAstc skip、
DecodeRoundtripRgba32 走未压缩路径仍过——此时记 TODO 到 Task 19 处理资产)

- [ ] **Step 6: Commit**

```bash
git add cmake/Deps.cmake core/CMakeLists.txt core/resource/ktx2_codec.* tests/common/ktx2_gen.* tests/resource/ktx2_test.cpp tests/CMakeLists.txt
git commit -m "feat(resource): libktx 接入 + ktx2_codec(解码/转码目标选择)+ 测试资产生成器"
```

---

### Task 8: gltf_loader KHR_texture_basisu + 外链 URI + 加载偏好

**Files:**
- Modify: `core/resource/image_codec.h`(ImageData 扩展 format/mipLevels)
- Modify: `core/resource/gltf_loader.h`、`core/resource/gltf_loader.cpp`(TextureLoadPref + basisu + URI)
- Modify: `core/resource/mesh_render_resource.cpp`(uploadOr 带 format/mipLevels)
- Test: `tests/resource/gltf_test.cpp`(追加 basisu/URI 用例)

- [ ] **Step 1: 写失败测试**

`tests/resource/gltf_test.cpp` 追加:

```cpp
// KHR_texture_basisu + 外链 URI:运行时生成 gltf+bin+ktx2 到临时目录再加载。
TEST(Gltf, BasisuExternalUri) {
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() / "rd_gltf_basisu";
  fs::create_directories(dir);
  const std::string gltfPath = (dir / "tri.gltf").string();
  const std::string binPath = (dir / "tri.bin").string();
  const std::string ktxPath = (dir / "tex.ktx2").string();
  ASSERT_TRUE(rd::test::writeTestKtx2(ktxPath.c_str(), 8));

  // 单三角形:pos(36B)|uv(24B)|idx(6B) 三段布局写入 tri.bin(共 66B)
  const float pos[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
  const float uv[6] = {0, 0, 1, 0, 0, 1};
  const uint16_t idx[3] = {0, 1, 2};
  {
    FILE* f = fopen(binPath.c_str(), "wb");
    ASSERT_NE(f, nullptr);
    fwrite(pos, 4, 9, f);
    fwrite(uv, 4, 6, f);
    fwrite(idx, 2, 3, f);
    fclose(f);
  }
  // gltf JSON:KHR_texture_basisu 引用外链 tex.ktx2;POSITION/UV 各一个 bufferView
  const char* json = R"({
    "asset": {"version": "2.0"},
    "extensionsUsed": ["KHR_texture_basisu"],
    "scenes": [{"nodes": [0]}], "scene": 0,
    "nodes": [{"mesh": 0}],
    "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "TEXCOORD_0": 1},
                                "indices": 2, "material": 0}]}],
    "materials": [{"pbrMetallicRoughness": {"baseColorTexture": {"index": 0}}}],
    "textures": [{"extensions": {"KHR_texture_basisu": {"source": 0}}}],
    "images": [{"uri": "tex.ktx2"}],
    "buffers": [{"uri": "tri.bin", "byteLength": 66}],
    "bufferViews": [
      {"buffer": 0, "byteOffset": 0, "byteLength": 36},
      {"buffer": 0, "byteOffset": 36, "byteLength": 24},
      {"buffer": 0, "byteOffset": 60, "byteLength": 6}],
    "accessors": [
      {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"},
      {"bufferView": 1, "componentType": 5126, "count": 3, "type": "VEC2"},
      {"bufferView": 2, "componentType": 5123, "count": 3, "type": "SCALAR"}]
  })";
  {
    FILE* f = fopen(gltfPath.c_str(), "wb");
    ASSERT_NE(f, nullptr);
    fwrite(json, 1, strlen(json), f);
    fclose(f);
  }

  rd::TextureLoadPref pref;
  pref.ktx2Target = rd::Ktx2Target::Rgba32;  // host 无 GPU caps 断言,走 rgba32 路径
  auto model = rd::loadGltf(gltfPath.c_str(), pref);
  ASSERT_TRUE(model.valid());
  ASSERT_EQ(model.meshes.size(), 1u);
  const auto& bc = model.meshes[0].material.baseColor;
  EXPECT_EQ(bc.width, 8u);
  EXPECT_EQ(bc.height, 8u);
  EXPECT_EQ(bc.format, rd::Format::RGBA8_UNORM);
  EXPECT_EQ(bc.mipLevels, 2u);
  EXPECT_FALSE(bc.pixels.empty());
}
```

(文件头补 `#include "common/ktx2_gen.h"`、`#include <filesystem>`、`#include <cstring>`)

- [ ] **Step 2: 跑测试确认失败**

Run: `./scripts/check.sh 2>&1 | tail -5`
Expected: 编译错误(`TextureLoadPref` 未声明 / loadGltf 单参版本不匹配)

- [ ] **Step 3: ImageData 扩展**

`core/resource/image_codec.h`:

```cpp
/// 解码后的图像:默认 RGBA8 紧凑排列(行主序,顶向下);
/// KTX2 压缩图像时 format 为压缩格式、pixels 为逐 mip 紧凑 block 数据。
struct ImageData {
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<uint8_t> pixels;
  Format format = Format::RGBA8_UNORM;  ///< 像素格式(压缩格式时 mipLevels>1 常见)
  uint32_t mipLevels = 1;               ///< mip 级数(仅 KTX2 路径 >1)
};
```

(image_codec.h 需 `#include "rhi/rhi_types.h"`)

- [ ] **Step 4: gltf_loader 修改**

`gltf_loader.h`:
- 顶部 `#include "resource/ktx2_codec.h"`。
- 新增:

```cpp
/// 纹理解码偏好(由调用方按 device caps 推导;loader 不直接碰 device)。
struct TextureLoadPref {
  Ktx2Target ktx2Target = Ktx2Target::Rgba32;  ///< KTX2 转码目标
  uint32_t maxDim = 4096;                       ///< PNG/JPEG 解码尺寸上限(等比降采样)
};
ModelAsset loadGltf(const char* path, const TextureLoadPref& pref);
```

(保留 `loadGltf(const char* path)` 声明,实现改为转发 `loadGltf(path, {})`)

`gltf_loader.cpp`:
1. `decodeImage` 重写:

```cpp
/// 读文件全部字节(外链 URI 用);失败返回空 vector 并记警告。
std::vector<uint8_t> readFileBytes(const std::string& path) {
  std::vector<uint8_t> out;
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) {
    RD_LOGW("resource.gltf", "外链资源打开失败: %s", path.c_str());
    return out;
  }
  fseek(f, 0, SEEK_END);
  const long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (n > 0) {
    out.resize(size_t(n));
    if (fread(out.data(), 1, out.size(), f) != out.size()) out.clear();
  }
  fclose(f);
  return out;
}

/// 图像字节 → ImageData:KTX2 走 ktx2_codec(带转码目标),
/// 否则 stb 解码(带 maxDim 降采样)。
ImageData decodeImageBytes(const uint8_t* bytes, uint64_t size,
                           const TextureLoadPref& pref) {
  ImageData img;
  if (!bytes || size == 0) return img;
  if (isKtx2(bytes, size)) {
    Ktx2Image k = decodeKtx2(bytes, size, pref.ktx2Target);
    img.width = k.width;
    img.height = k.height;
    img.pixels = std::move(k.data);
    img.format = k.format;
    img.mipLevels = k.mipLevels;
    return img;
  }
  img = decodeImageRGBA8(bytes, size, pref.maxDim);
  return img;
}

/// 解码纹理视图:内嵌 buffer_view 与外链 URI(相对 gltf 文件目录)两条路径;
/// KHR_texture_basisu 优先取 basisu_image。
ImageData decodeImage(const cgltf_texture* tex, const char* gltfDir,
                      const TextureLoadPref& pref) {
  ImageData img;
  if (!tex) return img;
  const cgltf_image* image = tex->basisu_image ? tex->basisu_image : tex->image;
  if (!image) return img;
  if (image->buffer_view) {
    const cgltf_buffer_view* bv = image->buffer_view;
    const auto* bytes = static_cast<const uint8_t*>(bv->buffer->data);
    return decodeImageBytes(bytes + bv->offset, uint64_t(bv->size), pref);
  }
  if (image->uri) {
    const std::string full = std::string(gltfDir) + "/" + image->uri;
    const auto bytes = readFileBytes(full);
    if (!bytes.empty()) return decodeImageBytes(bytes.data(), bytes.size(), pref);
  }
  return img;
}
```

2. `readMaterial` 加 `gltfDir`/`pref` 参数并透传;`loadGltf(path, pref)` 内:
   `const std::string dir = 目录部分(path)`(最后一个 '/' 之前,无则 ".")。
3. 原 `decodeImage(tex)` 调用点全部改为新签名。

- [ ] **Step 5: mesh_render_resource 上传适配**

`mesh_render_resource.cpp` 的 `uploadOr`:

```cpp
TextureHandle uploadOr(Device& dev, const ImageData& img, TextureHandle fallback) {
  if (img.width == 0) return fallback;
  rd::TextureDesc td;
  td.width = img.width;
  td.height = img.height;
  td.format = img.format;        // 压缩格式直通(RHI caps 门控)
  td.mipLevels = img.mipLevels;
  td.data = img.pixels.data();
  td.dataSize = uint64_t(img.pixels.size());
  auto tex = dev.createTexture(td);
  if (!tex.valid() && img.format != Format::RGBA8_UNORM) {
    // 压缩格式 caps 缺失的兜底在 loader 层(decodeKtx2 目标选择)已保证;
    // 此处再失败属异常,记日志返回占位,避免整个模型加载失败
    RD_LOGE("resource", "压缩纹理创建失败(format=%d),回退占位", int(img.format));
    return fallback;
  }
  return tex;
}
```

- [ ] **Step 6: image_codec maxDim**

`image_codec.h`:`decodeImageRGBA8(const void* data, uint64_t size, uint32_t maxDim = 0);`
(0=不限)
`image_codec.cpp`:stb 解码后若 maxDim>0 且超限时用 stb_image_resize2 等比缩小:

```cpp
#include <stb_image_resize2.h>  // 实现宏 STB_IMAGE_RESIZE_IMPLEMENTATION 见文件头说明
  ...
  if (maxDim > 0 && (w > maxDim || h > maxDim)) {
    const float s = float(maxDim) / float(std::max(w, h));
    const uint32_t nw = std::max(1u, uint32_t(w * s));
    const uint32_t nh = std::max(1u, uint32_t(h * s));
    std::vector<uint8_t> dst(size_t(nw) * nh * 4);
    stbir_resize_uint8_srgb(src, w, h, 0, dst.data(), nw, nh, 0, STBIR_RGBA);
    // 替换 img 内容
  }
```

(stb_image_resize2.h 在 stb 仓库根目录,`${stb_SOURCE_DIR}` 已在 include path;
实现宏名以头文件注释为准,在 image_codec.cpp 顶部与其他 stb 实现宏并列定义)

- [ ] **Step 7: 跑测试**

Run: `./scripts/check.sh 2>&1 | tail -8`
Expected: 全绿(含 `Gltf.BasisuExternalUri`;既有 `Gltf.*` 不回归——既有调用
`loadGltf(path)` 走默认 pref)

- [ ] **Step 8: Commit**

```bash
git add core/resource tests/resource tests/CMakeLists.txt
git commit -m "feat(resource): KHR_texture_basisu + 外链 URI + TextureLoadPref(转码目标/尺寸上限)"
```

---

### Task 9: image_codec maxDim 单测

**Files:**
- Test: `tests/common/image_test.cpp`(追加)

- [ ] **Step 1: 写失败测试**

`tests/common/image_test.cpp` 追加:

```cpp
// maxDim 等比降采样:16x8 PNG 限 8 → 8x4
TEST(Image, MaxDimDownscale) {
  // 运行时生成 16x8 PNG(红绿渐变)
  std::vector<uint8_t> px(16 * 8 * 4);
  for (uint32_t y = 0; y < 8; ++y)
    for (uint32_t x = 0; x < 16; ++x) {
      uint8_t* p = px.data() + (size_t(y) * 16 + x) * 4;
      p[0] = uint8_t(x * 16);
      p[1] = uint8_t(y * 32);
      p[3] = 255;
    }
  const std::string path =
      (std::filesystem::temp_directory_path() / "rd_maxdim.png").string();
  ASSERT_TRUE(rd::saveImagePNG(path.c_str(), 16, 8, px.data()));
  const auto bytes = [&] {  // 读回文件字节走内存解码
    FILE* f = fopen(path.c_str(), "rb");
    EXPECT_NE(f, nullptr);
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> b(size_t(n));
    EXPECT_EQ(fread(b.data(), 1, b.size(), f), b.size());
    fclose(f);
    return b;
  }();
  auto img = rd::decodeImageRGBA8(bytes.data(), bytes.size(), 8);
  EXPECT_EQ(img.width, 8u);
  EXPECT_EQ(img.height, 4u);
  // 不限时保持原尺寸
  auto full = rd::decodeImageRGBA8(bytes.data(), bytes.size());
  EXPECT_EQ(full.width, 16u);
  EXPECT_EQ(full.height, 8u);
}
```

(image_test.cpp 已在 rd_tests 中,直接追加;`#include <filesystem>` 如缺则补)

- [ ] **Step 2: 跑测试确认失败**

Run: `./scripts/check.sh 2>&1 | tail -5`
Expected: 若 Task 8 Step 6 已完成本步骤直接通过;否则编译错误(参数不匹配)。
**说明:本任务与 Task 8 Step 6 同 commit 亦可;单独成 Task 是为 TDD 节奏,
执行时可并入 Task 8 的 commit。**

- [ ] **Step 3: Commit(若单独提交)**

```bash
git add tests/common/image_test.cpp
git commit -m "test(resource): image_codec maxDim 降采样单测"
```

---

### Task 10: 内嵌 shader 泛化(多 shader 名表)

**Files:**
- Modify: `cmake/GenEmbedded.cmake`(泛化为名表驱动)
- Modify: `core/api/embedded_shaders.h`(新 embeddedShader 接口)
- Test: `tests/api/embedded_test.cpp`(新建)、`tests/CMakeLists.txt`(注册)

背景:engine 迁移 Renderer 链(Task 13)需要 unlit/pbr_forward/prefilter/blit 的内嵌字节;
现 `embeddedCubeShader` 只嵌 cube。泛化为按名查询。

- [ ] **Step 1: 写失败测试**

`tests/api/embedded_test.cpp`:

```cpp
// 内嵌 shader 名表:host 两后端的既有 shader 均可取到非空字节;未知名返回 false。
#include <gtest/gtest.h>
#include "api/embedded_shaders.h"
#include <cstring>

namespace {
void expectEmbedded(rd::Backend b, const char* name) {
  const uint8_t* data = nullptr;
  size_t size = 0;
  EXPECT_TRUE(rd::embeddedShader(b, name, rd::ShaderStage::Vertex, &data, &size))
      << name << " vert 缺失";
  EXPECT_GT(size, 1u);
  EXPECT_TRUE(rd::embeddedShader(b, name, rd::ShaderStage::Fragment, &data, &size))
      << name << " frag 缺失";
  EXPECT_GT(size, 1u);
}
} // namespace

TEST(Embedded, MetalShaders) {
#if defined(__APPLE__)
  expectEmbedded(rd::Backend::Metal, "cube");
  expectEmbedded(rd::Backend::Metal, "unlit");
  expectEmbedded(rd::Backend::Metal, "pbr_forward");
  expectEmbedded(rd::Backend::Metal, "prefilter");
#endif
}
TEST(Embedded, VulkanShaders) {
#if defined(RD_WITH_VULKAN)
  expectEmbedded(rd::Backend::Vulkan, "cube");
  expectEmbedded(rd::Backend::Vulkan, "unlit");
  expectEmbedded(rd::Backend::Vulkan, "pbr_forward");
  expectEmbedded(rd::Backend::Vulkan, "prefilter");
#endif
}
TEST(Embedded, UnknownNameReturnsFalse) {
#if defined(__APPLE__)
  const uint8_t* data = nullptr;
  size_t size = 0;
  EXPECT_FALSE(rd::embeddedShader(rd::Backend::Metal, "nope", rd::ShaderStage::Vertex,
                                  &data, &size));
#endif
}
// 兼容包装不回归
TEST(Embedded, CubeWrapperCompat) {
#if defined(__APPLE__)
  const uint8_t* d1 = nullptr;
  size_t n1 = 0;
  ASSERT_TRUE(rd::embeddedCubeShader(rd::Backend::Metal, rd::ShaderStage::Vertex, &d1, &n1));
  const uint8_t* d2 = nullptr;
  size_t n2 = 0;
  ASSERT_TRUE(rd::embeddedShader(rd::Backend::Metal, "cube", rd::ShaderStage::Vertex, &d2, &n2));
  EXPECT_EQ(n1, n2);
  EXPECT_EQ(memcmp(d1, d2, n1), 0);
#endif
}
```

`tests/CMakeLists.txt` 追加 `api/embedded_test.cpp`。

- [ ] **Step 2: 跑测试确认失败**

Run: `./scripts/check.sh 2>&1 | tail -5`
Expected: 编译错误(`embeddedShader` 未声明)

- [ ] **Step 3: embedded_shaders.h 扩展**

`core/api/embedded_shaders.h` 追加(保留 embeddedCubeShader 声明):

```cpp
/**
 * @brief 按 shader 名返回内嵌字节。
 * @param name  shader 源文件名(不含扩展名):"cube"/"unlit"/"pbr_forward"/
 *              "prefilter"/"blit"。
 * @return 该后端无产物或名不存在返回 false。
 */
bool embeddedShader(Backend backend, const char* name, ShaderStage stage,
                    const uint8_t** data, size_t* size);
```

头文件注释"内嵌 cube shader"改为"内嵌 shader 字节查询(cube/unlit/pbr_forward/
prefilter/blit,实现由构建系统生成)"。

- [ ] **Step 4: GenEmbedded.cmake 泛化**

`cmake/GenEmbedded.cmake` 重写为名表驱动(完整替换文件内容):

```cmake
# ============================================================================
# embedded_shaders.cpp 生成脚本(cmake -P 模式):
#   用法: cmake -DOUT=<输出cpp> -DDIR=<shaders_out> -DDIR_IOS=<shaders_out_ios>
#               [-DDIR_IOSSIM=<shaders_out_iossim>] -P GenEmbedded.cmake
# 把名表内 shader 的各后端产物十六进制内嵌;缺失产物生成占位空数组(sizeof==1)。
# ============================================================================
function(embed_file VAR_NAME FILE_PATH OUT_LINES)
  if(EXISTS ${FILE_PATH})
    file(READ ${FILE_PATH} hex HEX)
    string(REGEX MATCHALL ".." bytes "${hex}")
    set(body "")
    foreach(b ${bytes})
      string(APPEND body "0x${b},")
    endforeach()
    set(${OUT_LINES} "static const uint8_t ${VAR_NAME}[] = {${body}};\n" PARENT_SCOPE)
  else()
    set(${OUT_LINES} "static const uint8_t ${VAR_NAME}[] = {0};\n" PARENT_SCOPE)
  endif()
endfunction()

set(SHADERS cube unlit pbr_forward prefilter blit)
set(ALL_LINES "")
foreach(S ${SHADERS})
  embed_file(k_${S}_vert_spv     ${DIR}/${S}.vert.spv          L)
  string(APPEND ALL_LINES "${L}")
  embed_file(k_${S}_frag_spv     ${DIR}/${S}.frag.spv          L)
  string(APPEND ALL_LINES "${L}")
  embed_file(k_${S}_vert_gles    ${DIR}/${S}.vert.gles         L)
  string(APPEND ALL_LINES "${L}")
  embed_file(k_${S}_frag_gles    ${DIR}/${S}.frag.gles         L)
  string(APPEND ALL_LINES "${L}")
  embed_file(k_${S}_vert_metal   ${DIR}/${S}.vert.metallib     L)
  string(APPEND ALL_LINES "${L}")
  embed_file(k_${S}_frag_metal   ${DIR}/${S}.frag.metallib     L)
  string(APPEND ALL_LINES "${L}")
  embed_file(k_${S}_vert_metali  ${DIR_IOS}/${S}.vert.metallib L)
  string(APPEND ALL_LINES "${L}")
  embed_file(k_${S}_frag_metali  ${DIR_IOS}/${S}.frag.metallib L)
  string(APPEND ALL_LINES "${L}")
  embed_file(k_${S}_vert_metals  ${DIR_IOSSIM}/${S}.vert.metallib L)
  string(APPEND ALL_LINES "${L}")
  embed_file(k_${S}_frag_metals  ${DIR_IOSSIM}/${S}.frag.metallib L)
  string(APPEND ALL_LINES "${L}")
endforeach()

# 查询函数:名 → 各后端数组(Metal 三份由编译宏选择);
# 生成重复的小函数串,避免手写维护。
set(LOOKUP "")
foreach(S ${SHADERS})
  string(APPEND LOOKUP "
  if (strcmp(name, \"${S}\") == 0) {
    switch (backend) {
      case Backend::Vulkan:
        if (vs) { d = k_${S}_vert_spv; n = sizeof(k_${S}_vert_spv); }
        else    { d = k_${S}_frag_spv; n = sizeof(k_${S}_frag_spv); }
        break;
      case Backend::GLES:
        if (vs) { d = k_${S}_vert_gles; n = sizeof(k_${S}_vert_gles); }
        else    { d = k_${S}_frag_gles; n = sizeof(k_${S}_frag_gles); }
        break;
      case Backend::Metal:
#if defined(RD_EMBED_IOS_SIMULATOR)
        if (vs) { d = k_${S}_vert_metals; n = sizeof(k_${S}_vert_metals); }
        else    { d = k_${S}_frag_metals; n = sizeof(k_${S}_frag_metals); }
#elif defined(RD_EMBED_IOS_METAL)
        if (vs) { d = k_${S}_vert_metali; n = sizeof(k_${S}_vert_metali); }
        else    { d = k_${S}_frag_metali; n = sizeof(k_${S}_frag_metali); }
#else
        if (vs) { d = k_${S}_vert_metal; n = sizeof(k_${S}_vert_metal); }
        else    { d = k_${S}_frag_metal; n = sizeof(k_${S}_frag_metal); }
#endif
        break;
    }
  }
")
endforeach()

file(WRITE ${OUT} "// GENERATED FILE - 勿手改
#include \"api/embedded_shaders.h\"
#include <cstring>
namespace {
${ALL_LINES}
}
namespace rd {
bool embeddedShader(Backend backend, const char* name, ShaderStage stage,
                    const uint8_t** data, size_t* size) {
  const uint8_t* d = nullptr; size_t n = 0;
  const bool vs = (stage == ShaderStage::Vertex);
${LOOKUP}
  if (!d || n <= 1) return false;  // 占位空数组 sizeof==1
  *data = d; *size = n; return true;
}
bool embeddedCubeShader(Backend backend, ShaderStage stage, const uint8_t** data,
                        size_t* size) {
  return embeddedShader(backend, \"cube\", stage, data, size);
}
} // namespace rd
")
```

注意:blit 产物在 Task 11 才存在,本任务中 embed_file 对缺失文件生成占位
`{0}`——`Embedded.*` 测试不断言 blit,安全。`core/CMakeLists.txt` 的
add_custom_command DEPENDS 列表(`shader_cube.vert shader_cube.frag`)无需改
(缺产物不阻塞生成;blit 的 DEPENDS 在 Task 11 一并补)。

- [ ] **Step 5: 跑测试**

Run: `./scripts/check.sh 2>&1 | tail -8`
Expected: 全绿(Embedded.* 通过;api_test 既有用例不回归——embeddedCubeShader 行为不变)

- [ ] **Step 6: Commit**

```bash
git add cmake/GenEmbedded.cmake core/api/embedded_shaders.h tests/api/embedded_test.cpp tests/CMakeLists.txt
git commit -m "feat(api): 内嵌 shader 泛化为名表驱动(embeddedShader 按名查询)"
```

---

### Task 11: blit shader + Renderer 上屏链(SceneTarget→upscale)

**Files:**
- Create: `shaders/blit.vert`、`shaders/blit.frag`
- Modify: `shaders/CMakeLists.txt`(编译清单)
- Modify: `core/renderer/renderer.h`、`core/renderer/renderer.cpp`
- Modify: `core/rhi/backends/gles/gles_device.cpp`(uniform 块名表加 BlitUBO→0)
- Modify: `core/CMakeLists.txt`(embed DEPENDS 加 blit)
- Modify: `tools/render_test/main.cpp`、`tests/renderer/renderer_test.cpp`、`tests/renderer/box_render_test.cpp`、`tests/renderer/pbr_test.cpp`(RendererShaderDesc 构造点)

- [ ] **Step 1: blit shader**

`shaders/blit.vert`:

```glsl
// blit.vert:全屏三角形(无顶点缓冲,gl_VertexIndex 生成)。
// vFlip:GLES 渲染到纹理时 NDC+Y 落在内存末行(与 Metal/Vulkan 相反),
// 由 BlitUBO.params.x 翻转 v 吸收;Metal/Vulkan 传 0。
#version 450
layout(location = 0) out vec2 vUV;
layout(binding = 0) uniform BlitUBO { vec4 params; } u;  // uniform slot 0 ↔ binding 0
void main() {
  vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);  // (0,0),(2,0),(0,2)
  vUV = vec2(p.x, u.params.x > 0.5 ? p.y : 1.0 - p.y);
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
```

`shaders/blit.frag`:

```glsl
// blit.frag:采样 sceneColor(slot 0)直接输出(upscale pass;P2 后处理链挂载点)。
#version 450
layout(location = 0) in vec2 vUV;
layout(binding = 4) uniform sampler2D tex0;  // texture slot 0 → binding 4
layout(location = 0) out vec4 outColor;
void main() { outColor = texture(tex0, vUV); }
```

`shaders/CMakeLists.txt` 追加:

```cmake
rd_compile_shader(blit.vert)
rd_compile_shader(blit.frag)
```

`core/CMakeLists.txt` 的 add_custom_command DEPENDS 追加 `shader_blit.vert shader_blit.frag`。

- [ ] **Step 2: GLES uniform 块名表**

`gles_device.cpp` 名表(约 line 753)追加:

```cpp
      {"UBO", 0}, {"FrameUBO", 0}, {"ItemUBO", 1}, {"BlitUBO", 0},
```

- [ ] **Step 3: Renderer 改造**

`core/renderer/renderer.h`:
1. `RendererShaderDesc` 追加成员(在 prefilterFs 之后、entry 之前):

```cpp
  std::vector<uint8_t> blitVs, blitFs;          ///< 上屏链 upscale pass(全屏三角形)
```

2. 类内追加成员与方法:

```cpp
  /// 画质预设在 Task 12 引入;本任务先落内部旋钮(默认 1.0/1,行为与现状一致)。
  float renderScale_ = 1.0f;    ///< 场景目标分辨率缩放(≤1)
  uint32_t msaa_ = 1;           ///< 场景目标 MSAA 采样数
  Format colorFormat_ = Format::RGBA8_UNORM;  ///< init 记录(endScene 目标格式须一致)
 private:
  /// 按 (目标尺寸×renderScale, msaa) 确保内部场景目标可用,参数变化时重建。
  TargetHandle ensureSceneTarget(uint32_t targetW, uint32_t targetH);
  PipelineHandle blitPipeline_;
  BufferHandle blitUbo_;        // 16B:vec4(vFlip,0,0,0)
  SamplerHandle blitSampler_;
  TargetHandle sceneTarget_;
  uint32_t sceneW_ = 0, sceneH_ = 0, sceneSamples_ = 0;
```

`core/renderer/renderer.cpp`:
1. init 内(colorFormat 记录 + blit 资源):

```cpp
  colorFormat_ = desc.colorFormat;
  // blit 管线(无顶点缓冲:gl_VertexIndex 全屏三角形)
  auto bvs = dev.createShaderModule({ShaderStage::Vertex, desc.blitVs, desc.entry});
  auto bfs = dev.createShaderModule({ShaderStage::Fragment, desc.blitFs, desc.entry});
  PipelineDesc bpd;
  bpd.vertexShader = bvs;
  bpd.fragmentShader = bfs;
  bpd.cullMode = CullMode::None;
  bpd.colorFormat = desc.colorFormat;
  blitPipeline_ = dev.createPipeline(bpd);
  dev.destroyShaderModule(bvs);
  dev.destroyShaderModule(bfs);
  blitUbo_ = dev.createBuffer({16, BufferUsage::Uniform, true, false, nullptr});
  const float vflip = dev.backend() == Backend::GLES ? 1.0f : 0.0f;
  const float params[4] = {vflip, 0.0f, 0.0f, 0.0f};
  if (blitUbo_.valid()) dev.updateBuffer(blitUbo_, params, sizeof(params), 0);
  SamplerDesc bsd;
  bsd.wrapU = WrapMode::Clamp;
  bsd.wrapV = WrapMode::Clamp;
  blitSampler_ = dev.createSampler(bsd);
  // 失败检查汇总:if (!blitPipeline_.valid() || !blitUbo_.valid() || !blitSampler_.valid()) → shutdown+false
```

2. ensureSceneTarget 实现:

```cpp
TargetHandle Renderer::ensureSceneTarget(uint32_t targetW, uint32_t targetH) {
  const uint32_t w = std::max(1u, uint32_t(float(targetW) * renderScale_));
  const uint32_t h = std::max(1u, uint32_t(float(targetH) * renderScale_));
  const uint32_t capMsaa = dev_->caps().get(Capability::msaa);
  const uint32_t samples = std::max(1u, std::min(msaa_, capMsaa));
  if (sceneTarget_.valid() && w == sceneW_ && h == sceneH_ && samples == sceneSamples_)
    return sceneTarget_;
  if (sceneTarget_.valid()) dev_->destroyTarget(sceneTarget_);
  OffscreenTargetDesc td;
  td.width = w;
  td.height = h;
  td.depth = true;
  td.sampleCount = samples;
  td.colorFormat = colorFormat_;
  sceneTarget_ = dev_->createOffscreenTarget(td);
  if (!sceneTarget_.valid()) {
    RD_LOGE("renderer", "SceneTarget 创建失败(%ux%u samples=%u)", w, h, samples);
  }
  sceneW_ = w;
  sceneH_ = h;
  sceneSamples_ = samples;
  return sceneTarget_;
}
```

(`#include <algorithm>` 如缺则补)

3. endScene 改两段(渲染循环体不变,只换目标与追加 upscale):

```cpp
void Renderer::endScene(CommandBuffer* cmd, TargetHandle target) {
  // ... ItemUBO 填充段保持原样 ...
  uint32_t tw = 0, th = 0;
  dev_->targetSize(target, tw, th);
  TargetHandle scene = ensureSceneTarget(tw, th);
  if (!scene.valid()) {  // 场景目标失败:退化为直接渲染到最终目标
    scene = target;
  }
  cmd->beginRenderPass(scene, clear_);
  RenderContext ctx;
  ctx.frameUbo = frameUbo_;
  ctx.itemUbo = itemUbo_;
  ctx.env = &env_;
  const uint32_t count = uint32_t(queue_.size());
  for (uint32_t i = 0; i < count; ++i) {
    queue_[i]->prepass(cmd);
    ctx.itemOffset = uint64_t(i) * kUboStride;
    queue_[i]->record(cmd, ctx);
  }
  cmd->endRenderPass();
  if (scene != target) {  // upscale pass
    cmd->beginRenderPass(target, clear_);
    cmd->bindPipeline(blitPipeline_);
    cmd->bindUniformBuffer(0, blitUbo_, 0, 16);
    cmd->bindTexture(0, dev_->targetColorTexture(scene), blitSampler_);
    cmd->draw(3, 0);
    cmd->endRenderPass();
  }
  queue_.clear();
  worldStack_.clear();
}
```

4. shutdown 追加:

```cpp
  if (sceneTarget_.valid()) dev_->destroyTarget(sceneTarget_);
  if (blitPipeline_.valid()) dev_->destroyPipeline(blitPipeline_);
  if (blitUbo_.valid()) dev_->destroyBuffer(blitUbo_);
  if (blitSampler_.valid()) dev_->destroySampler(blitSampler_);
  sceneTarget_ = {};
  blitPipeline_ = {};
  blitUbo_ = {};
  blitSampler_ = {};
```

- [ ] **Step 4: 调用点适配**

4 处 RendererShaderDesc 聚合初始化追加 blit 字节:
- `tools/render_test/main.cpp`:`auto blitVs = load("blit.vert"), blitFs = load("blit.frag");`
  构造改为 `rd::RendererShaderDesc sd{unlitVs.code, unlitFs.code, pbrVs.code, pbrFs.code, pfVs.code, pfFs.code, blitVs.code, blitFs.code, unlitVs.entry, rd::Format::RGBA8_UNORM};`
- `tests/renderer/renderer_test.cpp`、`box_render_test.cpp`、`pbr_test.cpp`:同模式
  (各自搜 `RendererShaderDesc` 构造点,load blit 并插入 entry 之前)。

- [ ] **Step 5: 跑测试(golden 回归关键)**

Run: `./scripts/check.sh 2>&1 | tail -15`
Expected: 全绿。**重点核对 PbrHelmet/BoxTextured/Renderer golden 未回归**——
1:1 双线性采样应恒等;若 diffRatio 微涨但 <0.02 属采样精度,可接受;
若大面积超差,检查 blit 的 v 方向(上下翻转)与 SceneTarget 格式。

- [ ] **Step 6: Commit**

```bash
git add shaders/blit.* shaders/CMakeLists.txt core/renderer core/rhi/backends/gles/gles_device.cpp core/CMakeLists.txt tools/render_test tests/renderer
git commit -m "feat(renderer): 上屏链(SceneTarget→blit upscale pass)+ blit shader + BlitUBO 名表"
```

---

### Task 12: 画质分级(QualityPreset + Renderer::setQuality + Environment 参数化)

**Files:**
- Create: `core/renderer/quality.h`、`core/renderer/quality.cpp`
- Modify: `core/renderer/environment.h`、`core/renderer/environment.cpp`(build 参数化)
- Modify: `core/renderer/renderer.h`、`core/renderer/renderer.cpp`(setQuality + env 重建 + 默认预设)
- Modify: `core/CMakeLists.txt`(quality.cpp)
- Test: `tests/renderer/quality_test.cpp`(新建)、`tests/CMakeLists.txt`(注册)
- Golden: 新增 `tests/golden/helmet_low_metal.png`、`helmet_low_vulkan.png`(本任务生成)

- [ ] **Step 1: 写失败测试**

`tests/renderer/quality_test.cpp`:

```cpp
// 画质预设表 + caps 启发式 + setQuality 渲染(Low 档 golden)。
#include <gtest/gtest.h>
#include "common/image.h"
#include "common/shader_code.h"
#include "renderer/quality.h"
#include "renderer/renderer.h"
#include "resource/gltf_loader.h"
#include "resource/mesh_render_resource.h"
#include "scene/camera.h"
#include "scene/scene.h"
#include "rhi/rhi_device.h"
#include "rd_shader_dir.h"
#include <cstdlib>
#include <glm/glm.hpp>

TEST(Quality, PresetTable) {
  const auto hi = rd::qualityPreset(rd::QualityTier::High);
  EXPECT_FLOAT_EQ(hi.renderScale, 1.0f);
  EXPECT_EQ(hi.msaa, 4u);
  EXPECT_EQ(hi.iblPrefilterSize, 256u);
  EXPECT_EQ(hi.iblPrefilterMips, 6u);
  EXPECT_EQ(hi.maxTextureDim, 4096u);
  const auto mid = rd::qualityPreset(rd::QualityTier::Mid);
  EXPECT_FLOAT_EQ(mid.renderScale, 0.75f);
  EXPECT_EQ(mid.msaa, 2u);
  EXPECT_EQ(mid.iblPrefilterSize, 128u);
  EXPECT_EQ(mid.iblPrefilterMips, 5u);
  EXPECT_EQ(mid.maxTextureDim, 2048u);
  const auto low = rd::qualityPreset(rd::QualityTier::Low);
  EXPECT_FLOAT_EQ(low.renderScale, 0.5f);
  EXPECT_EQ(low.msaa, 1u);
  EXPECT_EQ(low.iblPrefilterSize, 64u);
  EXPECT_EQ(low.iblPrefilterMips, 4u);
  EXPECT_EQ(low.maxTextureDim, 1024u);
}

TEST(Quality, CapsHeuristic) {
  EXPECT_EQ(rd::qualityFromCaps(4, 8192), rd::QualityTier::High);
  EXPECT_EQ(rd::qualityFromCaps(8, 16384), rd::QualityTier::High);
  EXPECT_EQ(rd::qualityFromCaps(2, 8192), rd::QualityTier::Mid);
  EXPECT_EQ(rd::qualityFromCaps(4, 4096), rd::QualityTier::Mid);
  EXPECT_EQ(rd::qualityFromCaps(1, 4096), rd::QualityTier::Low);
  EXPECT_EQ(rd::qualityFromCaps(0, 0), rd::QualityTier::Low);
}

namespace {
constexpr uint32_t kW = 512, kH = 512;

// Low 档渲染 helmet:0.5x 内部分辨率 → upscale;golden 感知容差比对。
void runLowGolden(rd::Backend b) {
  rd::DeviceDesc d;
  d.backend = b;
  auto device = rd::createDevice(d);
  ASSERT_NE(device, nullptr);
  auto load = [&](const char* n) { return rd::test::loadShaderCode(b, RD_SHADER_DIR, n); };
  auto unlitVs = load("unlit.vert"), unlitFs = load("unlit.frag");
  auto pbrVs = load("pbr_forward.vert"), pbrFs = load("pbr_forward.frag");
  auto pfVs = load("prefilter.vert"), pfFs = load("prefilter.frag");
  auto blitVs = load("blit.vert"), blitFs = load("blit.frag");
  rd::Renderer renderer;
  rd::RendererShaderDesc sd{unlitVs.code, unlitFs.code, pbrVs.code, pbrFs.code,
                            pfVs.code,   pfFs.code,   blitVs.code, blitFs.code,
                            unlitVs.entry, rd::Format::RGBA8_UNORM};
  rd::OffscreenTargetDesc td;
  td.width = kW;
  td.height = kH;
  td.depth = true;
  auto target = device->createOffscreenTarget(td);
  auto model = rd::loadGltf(RD_TEST_DATA_DIR "/assets/DamagedHelmet.glb");
  ASSERT_TRUE(target.valid() && model.valid());
  ASSERT_TRUE(renderer.init(*device, sd));
  renderer.setQuality(rd::qualityPreset(rd::QualityTier::Low));
  auto res = rd::MeshRenderResource::upload(*device, model);
  ASSERT_NE(res, nullptr);
  rd::scene::Scene scene;
  auto node = std::make_unique<rd::scene::MeshNode>();
  node->mesh = res;
  scene.root().addChild(std::move(node));
  rd::scene::Camera cam;
  const float dist = model.boundingRadius * 2.5f;
  glm::vec3 center(model.boundingCenter[0], model.boundingCenter[1], model.boundingCenter[2]);
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
  res->destroy(*device);
  renderer.shutdown();
  const std::string name =
      b == rd::Backend::Metal ? "helmet_low_metal.png" : "helmet_low_vulkan.png";
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

TEST(Quality, LowGoldenMetal) {
#if defined(__APPLE__)
  runLowGolden(rd::Backend::Metal);
#endif
}
TEST(Quality, LowGoldenVulkan) {
#if defined(RD_WITH_VULKAN)
  runLowGolden(rd::Backend::Vulkan);
#endif
}
```

`tests/CMakeLists.txt` 追加 `renderer/quality_test.cpp`。

- [ ] **Step 2: 跑测试确认失败**

Run: `./scripts/check.sh 2>&1 | tail -5`
Expected: 编译错误(`renderer/quality.h` 不存在)

- [ ] **Step 3: quality.h/cpp**

`core/renderer/quality.h`:

```cpp
/**
 * @file quality.h
 * @brief 画质分级:高/中/低三档预设 + caps 启发式默认档。
 * 旋钮:渲染分辨率缩放 / MSAA 档位 / IBL prefilter 尺寸 / 纹理解码尺寸上限。
 */
#pragma once
#include <cstdint>

namespace rd {

enum class QualityTier : uint32_t { High = 0, Mid = 1, Low = 2 };

struct QualityPreset {
  float renderScale;          ///< 场景目标分辨率缩放(0.5/0.75/1.0)
  uint32_t msaa;              ///< 场景目标 MSAA 采样数(超 caps 时 clamp)
  uint32_t iblPrefilterSize;  ///< prefilter cube 边长
  uint32_t iblPrefilterMips;  ///< prefilter mip 级数(roughness 粒度)
  uint32_t maxTextureDim;     ///< 纹理解码尺寸上限(等比降采样)
};

/// 三档预设表。
QualityPreset qualityPreset(QualityTier t);
/// caps 启发式默认档:msaa≥4 且 maxTextureSize≥8192 → High;msaa≥2 → Mid;否则 Low。
QualityTier qualityFromCaps(uint32_t msaa, uint32_t maxTextureSize);

} // namespace rd
```

`core/renderer/quality.cpp`:

```cpp
#include "renderer/quality.h"

namespace rd {

QualityPreset qualityPreset(QualityTier t) {
  switch (t) {
    case QualityTier::High: return {1.0f, 4, 256, 6, 4096};
    case QualityTier::Mid:  return {0.75f, 2, 128, 5, 2048};
    case QualityTier::Low:  return {0.5f, 1, 64, 4, 1024};
  }
  return {1.0f, 1, 64, 5, 4096};
}

QualityTier qualityFromCaps(uint32_t msaa, uint32_t maxTextureSize) {
  if (msaa >= 4 && maxTextureSize >= 8192) return QualityTier::High;
  if (msaa >= 2) return QualityTier::Mid;
  return QualityTier::Low;
}

} // namespace rd
```

`core/CMakeLists.txt` 源列表追加 `renderer/quality.cpp`。

- [ ] **Step 4: Environment 参数化**

`environment.h` 的 build 签名:

```cpp
  /// 生成全部资源;cubeSize/prefilterMips 控制 prefilter 精度(画质档旋钮)。
  bool build(Device& dev, const std::vector<uint8_t>& pfVsCode,
             const std::vector<uint8_t>& pfFsCode, const std::string& entry,
             Format colorFormat, uint32_t cubeSize = 64, uint32_t prefilterMips = 5);
```

`environment.cpp`:`kEnvSize`/`kPrefilterMips` 常量删除,改为 build 参数落到成员
(`uint32_t cubeSize_ = 64, prefilterMips_ = 5;`),所有引用点替换;`sz = cubeSize_ >> mip`
等计算同步。注意 `kEnvSize >> mip` 在 mip 接近 log2(size) 时为 0 的风险:
256/6 → 最小 mip 8;64/4 → 8;128/5 → 8,均安全。

- [ ] **Step 5: Renderer::setQuality**

`renderer.h` 追加:

```cpp
  /// 应用画质预设:renderScale/msaa 下次 endScene 重建 SceneTarget 生效;
  /// IBL 尺寸变化立即重建环境(GPU 预滤波链);maxTextureDim 仅记录,
  /// 由加载链(rd_engine_load_gltf)读取。
  void setQuality(const QualityPreset& q);
  /// 当前生效的 maxTextureDim(加载链用)。
  uint32_t maxTextureDim() const { return maxTextureDim_; }
```

成员追加:`uint32_t maxTextureDim_ = 4096; uint32_t iblSize_ = 64, iblMips_ = 5;`
以及 env 重建所需暂存:`std::vector<uint8_t> pfVsCode_, pfFsCode_; std::string entry_;`

`renderer.cpp`:
1. init 内暂存 prefilter 字节/entry;env_.build 调用改传 `iblSize_, iblMips_`(默认值,
   保持现状输出)。
2. setQuality 实现:

```cpp
void Renderer::setQuality(const QualityPreset& q) {
  renderScale_ = q.renderScale;
  msaa_ = q.msaa;
  maxTextureDim_ = q.maxTextureDim;
  if (q.iblPrefilterSize != iblSize_ || q.iblPrefilterMips != iblMips_) {
    iblSize_ = q.iblPrefilterSize;
    iblMips_ = q.iblPrefilterMips;
    env_.destroy(*dev_);
    if (!env_.build(*dev_, pfVsCode_, pfFsCode_, entry_, colorFormat_, iblSize_, iblMips_))
      RD_LOGE("renderer", "IBL 环境重建失败(size=%u mips=%u)", iblSize_, iblMips_);
  }
}
```

(init 时 pfVsCode_ 等在 env_.build 之前暂存;shutdown 清理 vector)

- [ ] **Step 6: 跑测试 + 生成 Low golden**

Run: `./scripts/check.sh 2>&1 | tail -8`
Expected: PresetTable/CapsHeuristic 通过;LowGolden 报 golden 缺失
Run: `RD_UPDATE_GOLDENS=1 ctest --test-dir build --output-on-failure -R "Quality.LowGolden"`
然后**像素核对** `tests/golden/helmet_low_{metal,vulkan}.png`(helmet 可辨、明显比
helmet_*.png 糊、无翻转/错位),再 `./scripts/check.sh 2>&1 | tail -8` 全绿。

- [ ] **Step 7: Commit**

```bash
git add core/renderer core/CMakeLists.txt tests/renderer/quality_test.cpp tests/CMakeLists.txt tests/golden/helmet_low_*.png
git commit -m "feat(renderer): 画质分级(三档预设 + caps 启发式 + setQuality + IBL 参数化)+ Low 档 golden"
```

---

### Task 13: engine 迁移 Renderer 链 + C API 画质

**Files:**
- Modify: `core/api/rd_api.h`(rd_quality_t + set/get + RD_ERROR_ASSET)
- Modify: `core/api/rd_api.cpp`(engine 持 Renderer/Scene/Camera;CubeScene 退役出 engine)
- Test: `tests/api/api_test.cpp`(追加画质用例)

说明:本任务后 engine 无模型时只渲清屏色(旋转立方体演示退役;CubeScene 类保留,
tests/rhi 既有用例不受影响)。移动 demo 的模型在 Task 16/17 接入。

- [ ] **Step 1: 写失败测试**

`tests/api/api_test.cpp` 追加:

```cpp
// 画质 API:AUTO 默认;设置/读取往返;非法引擎安全
TEST(Api, QualityRoundtrip) {
  rd_engine* e = rd_engine_create(RD_BACKEND_METAL);
  ASSERT_NE(e, nullptr);
  // 初始为 AUTO 解析后的有效档(host Metal:msaa=4 且 maxTextureSize=16384 → High)
  const rd_quality_t initial = rd_engine_get_quality(e);
  EXPECT_TRUE(initial == RD_QUALITY_HIGH || initial == RD_QUALITY_MID ||
              initial == RD_QUALITY_LOW);
  EXPECT_EQ(rd_engine_set_quality(e, RD_QUALITY_LOW), RD_OK);
  EXPECT_EQ(rd_engine_get_quality(e), RD_QUALITY_LOW);
  EXPECT_EQ(rd_engine_set_quality(e, RD_QUALITY_AUTO), RD_OK);
  EXPECT_EQ(rd_engine_get_quality(e), initial);  // AUTO 回到启发式
  rd_engine_destroy(e);
}
TEST(Api, QualityNullSafe) {
  EXPECT_EQ(rd_engine_set_quality(nullptr, RD_QUALITY_HIGH), RD_ERROR_INVALID_ARG);
  EXPECT_EQ(rd_engine_get_quality(nullptr), RD_QUALITY_LOW);  // 空引擎返回占位
}
```

- [ ] **Step 2: 跑测试确认失败**

Run: `./scripts/check.sh 2>&1 | tail -5`
Expected: 编译错误(`rd_quality_t` 未定义)

- [ ] **Step 3: rd_api.h 扩展**

`rd_api.h`:
1. `rd_result` 枚举追加:`RD_ERROR_ASSET = 5,  ///< 资产加载/解析失败`。
2. 追加:

```c
/// 画质档位(AUTO=caps 启发式默认,引擎初始状态)。
typedef enum rd_quality {
  RD_QUALITY_AUTO = 0,
  RD_QUALITY_HIGH = 1,
  RD_QUALITY_MID = 2,
  RD_QUALITY_LOW = 3,
} rd_quality_t;

/**
 * @brief 设置画质档位;立即生效(下一次 render_frame 应用分辨率/MSAA/IBL 变化)。
 * @note 线程约定同 render_frame。
 */
rd_result_t rd_engine_set_quality(rd_engine* engine, rd_quality_t quality);
/// 当前生效档(AUTO 时返回启发式解析结果,不会返回 AUTO);空引擎返回 RD_QUALITY_LOW。
rd_quality_t rd_engine_get_quality(rd_engine* engine);
```

- [ ] **Step 4: rd_api.cpp 迁移**

`rd_engine` 结构体重写:

```cpp
#include "renderer/renderer.h"
#include "renderer/quality.h"
#include "resource/gltf_loader.h"
#include "resource/mesh_render_resource.h"
#include "scene/camera.h"
#include "scene/scene.h"

struct rd_engine {
  std::unique_ptr<rd::Device> device;
  rd::SwapChainHandle swapChain;
  rd::Renderer renderer;                  ///< 渲染器(set_surface 首次 init)
  rd::scene::Scene scene;                 ///< 场景(load_gltf 后挂模型节点)
  rd::scene::Camera camera;               ///< 相机(Task 15 起由 OrbitController 驱动)
  std::shared_ptr<rd::MeshRenderResource> model;  ///< 当前模型(无模型仅清屏)
  bool rendererReady = false;
  uint32_t width = 0, height = 0;
  rd_quality_t quality = RD_QUALITY_AUTO; ///< 配置档(AUTO 时按 caps 解析)
  char lastError[256] = {};
};
```

1. `rd_engine_set_surface` 的场景初始化段替换为 Renderer 初始化:

```cpp
  if (!e->rendererReady) {
    const rd::Backend b = e->device->backend();
    const char* entry = (b == rd::Backend::Metal) ? "main0" : "main";
    auto get = [&](const char* name, rd::ShaderStage st, std::vector<uint8_t>& out) {
      const uint8_t* d = nullptr;
      size_t n = 0;
      if (!rd::embeddedShader(b, name, st, &d, &n)) return false;
      out.assign(d, d + n);
      return true;
    };
    rd::RendererShaderDesc sd;
    sd.entry = entry;
    sd.colorFormat = e->device->swapChainColorFormat(e->swapChain);
    if (!get("unlit", rd::ShaderStage::Vertex, sd.unlitVs) ||
        !get("unlit", rd::ShaderStage::Fragment, sd.unlitFs) ||
        !get("pbr_forward", rd::ShaderStage::Vertex, sd.pbrVs) ||
        !get("pbr_forward", rd::ShaderStage::Fragment, sd.pbrFs) ||
        !get("prefilter", rd::ShaderStage::Vertex, sd.prefilterVs) ||
        !get("prefilter", rd::ShaderStage::Fragment, sd.prefilterFs) ||
        !get("blit", rd::ShaderStage::Vertex, sd.blitVs) ||
        !get("blit", rd::ShaderStage::Fragment, sd.blitFs)) {
      setError(e, "内嵌 shader 缺失");
      return RD_ERROR_SHADER;
    }
    if (!e->renderer.init(*e->device, sd)) {
      setError(e, "渲染器初始化失败");
      return RD_ERROR_SCENE;
    }
    e->rendererReady = true;
    // 初始画质:AUTO → caps 启发式
    applyQuality(e);
    // 默认相机(模型加载后由 frameModel 重取景;Task 15 接 Orbit)
    e->camera.lookAt({0, 0, 3}, {0, 0, 0}, {0, 1, 0});
  }
```

2. 命名空间内加画质辅助:

```cpp
/// 解析配置档(AUTO→caps 启发式)并应用到 renderer。
rd::QualityTier resolveTier(rd_engine* e) {
  if (e->quality != RD_QUALITY_AUTO)
    return e->quality == RD_QUALITY_HIGH   ? rd::QualityTier::High
           : e->quality == RD_QUALITY_MID  ? rd::QualityTier::Mid
                                           : rd::QualityTier::Low;
  return rd::qualityFromCaps(e->device->caps().get(rd::Capability::msaa),
                             e->device->caps().get(rd::Capability::max_texture_size));
}
void applyQuality(rd_engine* e) {
  if (e->rendererReady) e->renderer.setQuality(rd::qualityPreset(resolveTier(e)));
}
```

3. `rd_engine_render_frame` 重写:

```cpp
void rd_engine_render_frame(rd_engine* e, float dt) {
  if (!e || !e->swapChain.valid() || !e->rendererReady) { /* 既有节流告警保留 */ return; }
  e->device->beginFrame();
  rd::TargetHandle target = e->device->acquireSwapChainTarget(e->swapChain);
  if (!target.valid()) { e->device->endFrame(); return; }
  e->camera.setPerspective(0.78539816f, float(e->width) / float(e->height), 0.1f, 100.0f);
  e->renderer.beginScene(e->camera, {0.05f, 0.05f, 0.06f, 1.0f});
  e->scene.collect(e->renderer);
  auto* cmd = e->device->acquireCommandBuffer();
  e->renderer.endScene(cmd, target);
  e->device->submit(cmd);
  e->device->present(e->swapChain);
  e->device->endFrame();
  (void)dt;  // Task 15 起驱动 Orbit 惯性
}
```

4. `rd_engine_destroy`:按依赖逆序——`if (e->model) { e->model->destroy(*e->device); }`
   → `e->renderer.shutdown()` → destroySwapChain。
5. 画质 API:

```cpp
rd_result_t rd_engine_set_quality(rd_engine* e, rd_quality_t q) {
  if (!e) return RD_ERROR_INVALID_ARG;
  if (q != RD_QUALITY_AUTO && q != RD_QUALITY_HIGH && q != RD_QUALITY_MID &&
      q != RD_QUALITY_LOW)
    return RD_ERROR_INVALID_ARG;
  e->quality = q;
  applyQuality(e);
  return RD_OK;
}
rd_quality_t rd_engine_get_quality(rd_engine* e) {
  if (!e) return RD_QUALITY_LOW;
  switch (resolveTier(e)) {
    case rd::QualityTier::High: return RD_QUALITY_HIGH;
    case rd::QualityTier::Mid: return RD_QUALITY_MID;
    case rd::QualityTier::Low: return RD_QUALITY_LOW;
  }
  return RD_QUALITY_LOW;
}
```

6. 移除 CubeScene 相关 include/成员;`angle` 成员删除。

- [ ] **Step 5: 跑测试**

Run: `./scripts/check.sh 2>&1 | tail -8`
Expected: 全绿(含 Api.* 新旧用例;api_test 不碰 set_surface 之后的渲染路径,
engine 迁移对 host 测试透明)

- [ ] **Step 6: Commit**

```bash
git add core/api tests/api
git commit -m "feat(api): engine 迁移 Renderer 链 + 画质 C API(rd_engine_set/get_quality)"
```

---

### Task 14: scene/orbit_controller + 单测

**Files:**
- Create: `core/scene/orbit_controller.h`、`core/scene/orbit_controller.cpp`
- Modify: `core/CMakeLists.txt`(源列表)
- Test: `tests/scene/orbit_test.cpp`(新建)、`tests/CMakeLists.txt`(注册)

- [ ] **Step 1: 写失败测试**

`tests/scene/orbit_test.cpp`:

```cpp
// OrbitController 合成事件单测:旋转/pinch/平移/惯性/钳制/重置/相机产出。
#include <gtest/gtest.h>
#include "scene/orbit_controller.h"
#include <cmath>

namespace {
constexpr float kEps = 1e-4f;
}

TEST(Orbit, DragRotates) {
  rd::scene::OrbitController c;
  c.frameModel((const float[]){0, 0, 0}, 1.0f);  // dist = 2.5
  const float yaw0 = c.yaw(), pitch0 = c.pitch();
  c.onPointerDown(0, 100, 100);
  c.onPointerMove(0, 200, 150);  // dx=+100, dy=+50
  c.onPointerUp(0, 200, 150);
  // 约定:右拖 yaw 减(相机向右绕),下拖 pitch 增
  EXPECT_NEAR(c.yaw(), yaw0 - 100 * 0.005f, kEps);
  EXPECT_NEAR(c.pitch(), pitch0 + 50 * 0.005f, kEps);
}

TEST(Orbit, PinchZooms) {
  rd::scene::OrbitController c;
  c.frameModel((const float[]){0, 0, 0}, 1.0f);
  const float d0 = c.distance();
  c.onPointerDown(0, 100, 100);
  c.onPointerDown(1, 200, 100);
  c.onPointerMove(1, 300, 100);  // 指距 100→200,ratio 2
  EXPECT_NEAR(c.distance(), d0 / 2.0f, d0 * 0.01f);
  c.onPointerUp(0, 100, 100);
  c.onPointerUp(1, 300, 100);
}

TEST(Orbit, TwoFingerPan) {
  rd::scene::OrbitController c;
  c.frameModel((const float[]){0, 0, 0}, 1.0f);
  c.onPointerDown(0, 100, 100);
  c.onPointerDown(1, 200, 100);
  // 双指同向移动(质心 +50,+0)→ target 平移(yaw/pitch/distance 不变)
  const float yaw0 = c.yaw();
  c.onPointerMove(0, 150, 100);
  c.onPointerMove(1, 250, 100);
  EXPECT_NEAR(c.yaw(), yaw0, kEps);
  rd::scene::Camera cam;
  c.applyTo(cam);
  // eye 不再在 (0,0,d) 轴上(发生了平移)
  EXPECT_TRUE(std::abs(cam.eye().x) > 0.01f);
  c.onPointerUp(0, 150, 100);
  c.onPointerUp(1, 250, 100);
}

TEST(Orbit, InertiaDecays) {
  rd::scene::OrbitController c;
  c.setParams(rd::scene::OrbitController::Params{.dampingTau = 0.12f});
  c.frameModel((const float[]){0, 0, 0}, 1.0f);
  c.onPointerDown(0, 0, 0);
  for (int i = 1; i <= 5; ++i) c.onPointerMove(0, float(i * 20), 0);  // 快速拖动
  c.onPointerUp(0, 100, 0);
  const float y0 = c.yaw();
  c.update(0.016f);  // 惯性继续
  const float y1 = c.yaw();
  EXPECT_LT(y1, y0) << "惯性应继续旋转方向";
  for (int i = 0; i < 600; ++i) c.update(0.016f);  // 10 秒收敛
  const float y2 = c.yaw();
  c.update(0.016f);
  EXPECT_NEAR(c.yaw(), y2, 1e-5f) << "惯性应收敛停止";
}

TEST(Orbit, PitchClamped) {
  rd::scene::OrbitController c;
  c.frameModel((const float[]){0, 0, 0}, 1.0f);
  c.onPointerDown(0, 0, 0);
  c.onPointerMove(0, 0, 100000);  // 疯狂下拖
  EXPECT_LE(c.pitch(), 1.55f);
  c.onPointerMove(0, 0, -200000);
  EXPECT_GE(c.pitch(), -1.55f);
  c.onPointerUp(0, 0, 0);
}

TEST(Orbit, DoubleTapResets) {
  rd::scene::OrbitController c;
  c.frameModel((const float[]){0, 0, 0}, 1.0f);
  const float yaw0 = c.yaw(), d0 = c.distance();
  c.onPointerDown(0, 0, 0);
  c.onPointerMove(0, 300, 200);
  c.onPointerUp(0, 300, 200);
  c.onScroll(-5);
  EXPECT_NE(c.distance(), d0);
  c.onDoubleTap();
  EXPECT_NEAR(c.yaw(), yaw0, kEps);
  EXPECT_NEAR(c.distance(), d0, kEps);
}

TEST(Orbit, ApplyToCameraDistance) {
  rd::scene::OrbitController c;
  c.frameModel((const float[]){1, 2, 3}, 2.0f);  // dist = 5
  rd::scene::Camera cam;
  c.applyTo(cam);
  const auto& e = cam.eye();
  const float d = std::sqrt((e.x - 1) * (e.x - 1) + (e.y - 2) * (e.y - 2) +
                            (e.z - 3) * (e.z - 3));
  EXPECT_NEAR(d, 5.0f, 0.01f);
}
```

`tests/CMakeLists.txt` 追加 `scene/orbit_test.cpp`。

- [ ] **Step 2: 跑测试确认失败**

Run: `./scripts/check.sh 2>&1 | tail -5`
Expected: 编译错误(`scene/orbit_controller.h` 不存在)

- [ ] **Step 3: 实现 orbit_controller**

`core/scene/orbit_controller.h`:

```cpp
/**
 * @file orbit_controller.h
 * @brief Orbit 相机控制器:单指旋转 / 双指 pinch 缩放 / 双指平移 / 双击重置,
 * 带指数阻尼惯性。输入为像素坐标指针事件(平台层喂入);纯数学,不碰 GPU。
 *
 * 方向约定:yaw=0/pitch=0 时 eye 在 target 的 +Z 方向(与默认相机 (0,0,3) 一致);
 * 右拖 yaw 减(相机向右绕),下拖 pitch 增;pitch 钳制 ±1.55 rad。
 */
#pragma once
#include "foundation/math.h"
#include "scene/camera.h"

namespace rd::scene {

class OrbitController {
public:
  struct Params {
    float rotateSpeed = 0.005f;  ///< 弧度/像素
    float panFactor = 0.0015f;   ///< 平移系数(× distance,世界单位/像素)
    float dampingTau = 0.12f;    ///< 惯性衰减时间常数(秒)
    float minDistance = 0.01f;
    float maxDistance = 1e4f;
  };

  void setParams(const Params& p) { params_ = p; }

  /// 取景:target=center,distance=radius*2.5,视角 yaw=0.65/pitch=0.35
  /// (与 render_test --pbr 的 45°/20° 取景风格一致);记录为重置基准。
  void frameModel(const float center[3], float radius);
  /// 双击:回到重置基准位。
  void resetView();

  /// 指针事件(像素坐标;id 区分多指,最多跟踪 2 个)。
  void onPointerDown(int id, float x, float y);
  void onPointerMove(int id, float x, float y);
  void onPointerUp(int id, float x, float y);
  /// host 滚轮:deltaY>0 拉近。
  void onScroll(float deltaY);
  /// Android 探测器路径:双指比例缩放(ratio>1 放大→距离拉近)。
  void onPinch(float ratio);
  void onDoubleTap() { resetView(); }

  /// 每帧积分惯性(无指针按下时生效);dt 秒。
  void update(float dt);
  /// 写相机 eye/center(up 恒 (0,1,0))。
  void applyTo(Camera& cam) const;

  float yaw() const { return yaw_; }
  float pitch() const { return pitch_; }
  float distance() const { return distance_; }

private:
  struct Pointer {
    int id = -1;
    float x = 0, y = 0;
  };
  int pointerCount() const { return (p0_.id >= 0 ? 1 : 0) + (p1_.id >= 0 ? 1 : 0); }

  Params params_;
  math::Vec3 target_{0, 0, 0};
  float yaw_ = 0.0f, pitch_ = 0.0f, distance_ = 3.0f;
  // 重置基准
  math::Vec3 homeTarget_{0, 0, 0};
  float homeYaw_ = 0, homePitch_ = 0, homeDistance_ = 3.0f;
  // 惯性速度(EMA,弧度/秒级)
  float vyaw_ = 0, vpitch_ = 0;
  // 双指状态
  float lastPinchDist_ = 0;
  float lastCentroidX_ = 0, lastCentroidY_ = 0;
  Pointer p0_, p1_;
};

} // namespace rd::scene
```

`core/scene/orbit_controller.cpp`:

```cpp
#include "scene/orbit_controller.h"
#include <algorithm>
#include <cmath>

namespace rd::scene {
namespace {
constexpr float kPitchLimit = 1.55f;
float clampPitch(float p) { return std::max(-kPitchLimit, std::min(kPitchLimit, p)); }
} // namespace

void OrbitController::frameModel(const float center[3], float radius) {
  target_ = homeTarget_ = math::Vec3(center[0], center[1], center[2]);
  distance_ = homeDistance_ = std::max(radius * 2.5f, params_.minDistance);
  yaw_ = homeYaw_ = 0.65f;
  pitch_ = homePitch_ = 0.35f;
  vyaw_ = vpitch_ = 0;
  p0_ = p1_ = Pointer{};
}

void OrbitController::resetView() {
  target_ = homeTarget_;
  yaw_ = homeYaw_;
  pitch_ = homePitch_;
  distance_ = homeDistance_;
  vyaw_ = vpitch_ = 0;
}

void OrbitController::onPointerDown(int id, float x, float y) {
  if (p0_.id < 0) p0_ = {id, x, y};
  else if (p1_.id < 0) {
    p1_ = {id, x, y};
    // 进入双指:初始化 pinch/质心基准,清惯性
    lastPinchDist_ = std::hypot(p1_.x - p0_.x, p1_.y - p0_.y);
    lastCentroidX_ = (p0_.x + p1_.x) * 0.5f;
    lastCentroidY_ = (p0_.y + p1_.y) * 0.5f;
  }
  vyaw_ = vpitch_ = 0;
}

void OrbitController::onPointerMove(int id, float x, float y) {
  Pointer* p = p0_.id == id ? &p0_ : p1_.id == id ? &p1_ : nullptr;
  if (!p) return;
  if (pointerCount() == 1) {
    // 单指旋转
    const float dx = x - p->x, dy = y - p->y;
    p->x = x;
    p->y = y;
    const float dyaw = -dx * params_.rotateSpeed;
    const float dpitch = dy * params_.rotateSpeed;
    yaw_ += dyaw;
    pitch_ = clampPitch(pitch_ + dpitch);
    // 惯性 EMA(按 60Hz 事件节奏折算为速度量纲)
    vyaw_ = vyaw_ * 0.8f + dyaw * 60.0f * 0.2f;
    vpitch_ = vpitch_ * 0.8f + dpitch * 60.0f * 0.2f;
    return;
  }
  // 双指:先更新位置
  p->x = x;
  p->y = y;
  const float pinchDist = std::hypot(p1_.x - p0_.x, p1_.y - p0_.y);
  const float cx = (p0_.x + p1_.x) * 0.5f, cy = (p0_.y + p1_.y) * 0.5f;
  if (lastPinchDist_ > 1e-3f && pinchDist > 1e-3f)
    onPinch(pinchDist / lastPinchDist_);
  // 质心平移:沿相机 right/up 移动 target
  const float ddx = cx - lastCentroidX_, ddy = cy - lastCentroidY_;
  if (ddx != 0 || ddy != 0) {
    const float s = distance_ * params_.panFactor;
    const float cy_ = std::cos(yaw_), sy_ = std::sin(yaw_);
    // 相机 right = (cy_, 0, -sy_);up 近似取 (0,1,0) 分量方向
    target_.x -= ddx * s * cy_;
    target_.z += ddx * s * sy_;
    target_.y += ddy * s;
  }
  lastPinchDist_ = pinchDist;
  lastCentroidX_ = cx;
  lastCentroidY_ = cy;
  vyaw_ = vpitch_ = 0;
}

void OrbitController::onPointerUp(int id, float, float) {
  if (p0_.id == id) p0_.id = -1;
  if (p1_.id == id) p1_.id = -1;
  lastPinchDist_ = 0;
}

void OrbitController::onScroll(float deltaY) {
  distance_ = std::max(params_.minDistance,
                       std::min(params_.maxDistance,
                                distance_ * std::exp(-deltaY * 0.002f)));
}

void OrbitController::onPinch(float ratio) {
  if (ratio <= 1e-3f) return;
  distance_ = std::max(params_.minDistance,
                       std::min(params_.maxDistance, distance_ / ratio));
}

void OrbitController::update(float dt) {
  if (pointerCount() > 0) return;  // 拖拽中不积分惯性
  if (vyaw_ == 0 && vpitch_ == 0) return;
  yaw_ += vyaw_ * dt;
  pitch_ = clampPitch(pitch_ + vpitch_ * dt);
  const float decay = std::exp(-dt / params_.dampingTau);
  vyaw_ *= decay;
  vpitch_ *= decay;
  if (std::abs(vyaw_) < 1e-3f) vyaw_ = 0;
  if (std::abs(vpitch_) < 1e-3f) vpitch_ = 0;
}

void OrbitController::applyTo(Camera& cam) const {
  const float cp = std::cos(pitch_), sp = std::sin(pitch_);
  const float cy = std::cos(yaw_), sy = std::sin(yaw_);
  // yaw=0/pitch=0 → +Z;yaw>0 向 -X 绕(右手)
  const math::Vec3 dir(cp * sy, sp, cp * cy);
  cam.lookAt(target_ + dir * distance_, target_, math::Vec3(0, 1, 0));
}

} // namespace rd::scene
```

单测数值自检:DragRotates 中 dx=+100 → dyaw=-0.5;PinchZooms ratio=2 → d/2;
InertiaDecays 拖动向右(dyaw<0)→ vyaw<0 → yaw 继续减 ✓。
**注意 InertiaDecays 的 Params 聚合初始化**:`setParams({.dampingTau=0.12f})` 的
指派初始化在 C++17 不可用——测试里改为:

```cpp
  rd::scene::OrbitController::Params prm;
  prm.dampingTau = 0.12f;
  c.setParams(prm);
```

`core/CMakeLists.txt` 源列表追加 `scene/orbit_controller.cpp`。

- [ ] **Step 4: 跑测试**

Run: `./scripts/check.sh 2>&1 | tail -8`
Expected: 全绿(Orbit.* 7 个用例)

- [ ] **Step 5: Commit**

```bash
git add core/scene/orbit_controller.* core/CMakeLists.txt tests/scene/orbit_test.cpp tests/CMakeLists.txt
git commit -m "feat(scene): OrbitController(旋转/pinch/平移/双击重置/惯性阻尼)+ 单测"
```

---

### Task 15: C API 输入事件 + rd_engine_load_gltf + engine Orbit 集成

**Files:**
- Modify: `core/api/rd_api.h`、`core/api/rd_api.cpp`
- Test: `tests/api/api_test.cpp`(追加)

- [ ] **Step 1: 写失败测试**

`tests/api/api_test.cpp` 追加:

```cpp
// 输入事件 API:无 surface 也安全(相机状态更新);空引擎拒绝
TEST(Api, PointerEventsSafe) {
  rd_engine* e = rd_engine_create(RD_BACKEND_METAL);
  ASSERT_NE(e, nullptr);
  rd_engine_on_pointer(e, RD_POINTER_DOWN, 0, 100, 100);
  rd_engine_on_pointer(e, RD_POINTER_MOVE, 0, 200, 150);
  rd_engine_on_pointer(e, RD_POINTER_UP, 0, 200, 150);
  rd_engine_on_scroll(e, 1.0f);
  rd_engine_on_pinch(e, 1.5f);
  rd_engine_on_double_tap(e, 0, 0);
  rd_engine_render_frame(e, 0.016f);  // 无 surface 安全 no-op
  rd_engine_destroy(e);
  rd_engine_on_pointer(nullptr, RD_POINTER_DOWN, 0, 0, 0);  // 不崩
}

// 模型加载:不存在路径返回 RD_ERROR_ASSET;有效 glb 返回 OK
TEST(Api, LoadGltf) {
  rd_engine* e = rd_engine_create(RD_BACKEND_METAL);
  ASSERT_NE(e, nullptr);
  EXPECT_EQ(rd_engine_load_gltf(e, "/nonexistent/x.glb"), RD_ERROR_ASSET);
  EXPECT_STRNE(rd_get_last_error(e), "");
  EXPECT_EQ(rd_engine_load_gltf(e, RD_TEST_DATA_DIR "/assets/TetraU32.glb"), RD_OK);
  rd_engine_destroy(e);
}
```

- [ ] **Step 2: 跑测试确认失败**

Run: `./scripts/check.sh 2>&1 | tail -5`
Expected: 编译错误(rd_engine_on_pointer 等未声明)

- [ ] **Step 3: rd_api.h 扩展**

追加:

```c
/// 指针动作(触摸/鼠标)。
typedef enum rd_pointer_action {
  RD_POINTER_DOWN = 0,
  RD_POINTER_MOVE = 1,
  RD_POINTER_UP = 2,
  RD_POINTER_CANCEL = 3,
} rd_pointer_action_t;

/**
 * @brief 指针事件(像素坐标,origin 左上;id 区分多指,引擎跟踪前 2 个)。
 * 单指=旋转;双指=pinch 缩放+平移。空引擎安全忽略。
 */
void rd_engine_on_pointer(rd_engine* engine, rd_pointer_action_t action,
                          int32_t pointer_id, float x, float y);
/// host 滚轮缩放(deltaY>0 拉近)。
void rd_engine_on_scroll(rd_engine* engine, float delta_y);
/// 双指比例缩放(Android 探测器路径;ratio>1 放大→拉近)。
void rd_engine_on_pinch(rd_engine* engine, float ratio);
/// 双击重置取景。
void rd_engine_on_double_tap(rd_engine* engine, float x, float y);

/**
 * @brief 同步加载 glb/gltf 模型并替换场景内容(成功后 Orbit 自动取景)。
 * 纹理解码按当前画质档的尺寸上限与设备压缩格式 caps 自动选择。
 * @return RD_OK / RD_ERROR_INVALID_ARG(空参)/ RD_ERROR_ASSET(解析/上传失败,
 *         细节见 rd_get_last_error)。v1 为同步加载(异步留 P2)。
 */
rd_result_t rd_engine_load_gltf(rd_engine* engine, const char* path);
```

- [ ] **Step 4: rd_api.cpp 集成**

1. 结构体追加:

```cpp
#include "scene/orbit_controller.h"
  // rd_engine 内:
  rd::scene::OrbitController orbit;
```

2. render_frame:dt 驱动惯性 + 写相机:

```cpp
  e->orbit.update(dt);
  e->orbit.applyTo(e->camera);
  e->camera.setPerspective(0.78539816f, float(e->width) / float(e->height),
                           std::max(0.01f, e->orbit.distance() * 0.02f),
                           e->orbit.distance() * 20.0f);
```

(near/far 随取景距离自适应;删除 Task 13 的固定 lookAt 默认相机段——
engine create 时改为 `e->orbit.frameModel((const float[]){0,0,0}, 1.2f)` 给初始位)
3. 输入 API:

```cpp
void rd_engine_on_pointer(rd_engine* e, rd_pointer_action_t a, int32_t id, float x,
                          float y) {
  if (!e) return;
  switch (a) {
    case RD_POINTER_DOWN: e->orbit.onPointerDown(int(id), x, y); break;
    case RD_POINTER_MOVE: e->orbit.onPointerMove(int(id), x, y); break;
    case RD_POINTER_UP:
    case RD_POINTER_CANCEL: e->orbit.onPointerUp(int(id), x, y); break;
  }
}
void rd_engine_on_scroll(rd_engine* e, float dy) { if (e) e->orbit.onScroll(dy); }
void rd_engine_on_pinch(rd_engine* e, float r) { if (e) e->orbit.onPinch(r); }
void rd_engine_on_double_tap(rd_engine* e, float, float) {
  if (e) e->orbit.onDoubleTap();
}
```

4. load_gltf:

```cpp
rd_result_t rd_engine_load_gltf(rd_engine* e, const char* path) {
  if (!e || !path) return RD_ERROR_INVALID_ARG;
  // 纹理偏好:压缩目标按 caps,尺寸上限按当前画质档
  rd::TextureLoadPref pref;
  pref.ktx2Target = rd::pickTranscodeTarget(
      e->device->caps().supports(rd::Capability::texture_compression_astc),
      e->device->caps().supports(rd::Capability::texture_compression_etc2));
  pref.maxDim = e->rendererReady ? e->renderer.maxTextureDim() : 4096;
  auto model = rd::loadGltf(path, pref);
  if (!model.valid()) {
    setError(e, (std::string("glTF 加载失败: ") + path).c_str());
    return RD_ERROR_ASSET;
  }
  e->device->waitIdle();  // 防旧模型在飞引用
  auto res = rd::MeshRenderResource::upload(*e->device, model);
  if (!res) {
    setError(e, "模型 GPU 上传失败");
    return RD_ERROR_ASSET;
  }
  if (e->model) e->model->destroy(*e->device);
  e->model = res;
  // 重建场景:单 MeshNode
  e->scene = rd::scene::Scene();
  auto node = std::make_unique<rd::scene::MeshNode>();
  node->mesh = res;
  e->scene.root().addChild(std::move(node));
  e->orbit.frameModel(model.boundingCenter, model.boundingRadius);
  return RD_OK;
}
```

(`#include <string>`;scene 重新赋值要求 Scene 可移动赋值——若 Node 不可移动,
改为 `e->scene = std::move(*newScene)` 模式或给 engine 存
`std::unique_ptr<rd::scene::Scene>`;以实现时编译为准,首选 unique_ptr。)

- [ ] **Step 5: 跑测试**

Run: `./scripts/check.sh 2>&1 | tail -8`
Expected: 全绿(Api.LoadGltf/PointerEventsSafe 通过)

- [ ] **Step 6: Commit**

```bash
git add core/api tests/api
git commit -m "feat(api): 输入事件 C API + rd_engine_load_gltf + Orbit 取景集成"
```

---

### Task 16: iOS 手势接线 + demo 换 PBR 模型

**Files:**
- Modify: `platform/ios/RenderView.swift`(触摸事件 → C API)
- Modify: `samples/ios/`(DamagedHelmet.glb 入 bundle + 启动加载;具体工程文件以现状为准)

- [ ] **Step 1: RenderView.swift 触摸转发**

`RenderView.swift` 追加(raw touches 路径,1:1 映射 C API):

```swift
    // ---- 触摸 → Orbit(rd_engine 主线程约定,直接调用)----
    /// UITouch → 稳定指针 id(按 touch 对象地址散列,跟踪期内稳定)
    private func touchId(_ touch: UITouch) -> Int32 {
        Int32(truncatingIfNeeded: ObjectIdentifier(touch).hashValue)
    }
    private func forwardTouches(_ touches: Set<UITouch>, action: rd_pointer_action_t) {
        guard let engine else { return }
        let scale = contentScaleFactor  // 逻辑点 → 物理像素
        for t in touches {
            let p = t.location(in: self)
            rd_engine_on_pointer(engine, action, touchId(t),
                                 Float(p.x * scale), Float(p.y * scale))
        }
    }
    override public func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent?) {
        forwardTouches(touches, action: RD_POINTER_DOWN)
    }
    override public func touchesMoved(_ touches: Set<UITouch>, with event: UIEvent?) {
        forwardTouches(touches, action: RD_POINTER_MOVE)
    }
    override public func touchesEnded(_ touches: Set<UITouch>, with event: UIEvent?) {
        forwardTouches(touches, action: RD_POINTER_UP)
    }
    override public func touchesCancelled(_ touches: Set<UITouch>, with event: UIEvent?) {
        forwardTouches(touches, action: RD_POINTER_CANCEL)
    }
```

init 两处(frame/coder)追加 `isMultipleTouchEnabled = true`;
双击用手势识别器(startEngine 里挂):

```swift
        let doubleTap = UITapGestureRecognizer(target: self, action: #selector(onDoubleTap(_:)))
        doubleTap.numberOfTapsRequired = 2
        addGestureRecognizer(doubleTap)
...
    @objc private func onDoubleTap(_ g: UITapGestureRecognizer) {
        guard let engine else { return }
        let p = g.location(in: self)
        rd_engine_on_double_tap(engine, Float(p.x), Float(p.y))
    }
```

(`#import` C API:桥接头已暴露 rd_api.h 则直接用;枚举名在 Swift 为
`RD_POINTER_DOWN` 等——若 Swift 导入名为 `rd_pointer_action` 前缀形式,按 Xcode 补全修正)

- [ ] **Step 2: demo 加载模型**

- 把 `tests/assets/DamagedHelmet.glb` 拷入 iOS sample 的 bundle resources
  (Xcode 工程 Copy Bundle Resources;若 sample 是 SwiftPM/工程文件组织,按现状加入)。
- sample 的视图控制器在 RenderView 上屏后调用:

```swift
if let path = Bundle.main.path(forResource: "DamagedHelmet", ofType: "glb") {
    rd_engine_load_gltf(engine, path)
}
```

(engine 句柄暴露:RenderView 加 `public var enginePtr: OpaquePointer? { engine }`,
或在 RenderView 内加 `loadModel(path:)` 方法包装——选后者,保持 engine 私有:

```swift
    /// 加载 glTF 模型(主线程;启动后调用一次)。
    public func loadModel(_ path: String) {
        guard let engine else { return }
        let r = rd_engine_load_gltf(engine, path)
        print("RD: load_gltf -> \(r)")
    }
```
)

- [ ] **Step 3: 构建验证**

Run: `cmake -S . -B build-ios -G Xcode -DCMAKE_TOOLCHAIN_FILE=cmake/ios.toolchain.cmake -DRD_IOS_SDK=iphonesimulator && cmake --build build-ios --config Debug 2>&1 | tail -5`
Expected: BUILD SUCCEEDED(含 libktx 的 iOS 编译;若 KTX-Software iOS 配置报错,
按其 CMake 选项关闭平台相关特性后重试)
模拟器运行+截图:`xcrun simctl` 流程与 P0 一致,截图 `img_check --min-coverage 0.03` 通过。

- [ ] **Step 4: Commit**

```bash
git add platform/ios samples/ios
git commit -m "feat(platform): iOS 手势接线(touches→C API)+ demo 加载 DamagedHelmet"
```

---

### Task 17: Android 手势接线 + demo 换 PBR 模型

**Files:**
- Modify: `platform/android/src/main/java/com/rd/renderer/RenderView.kt`
- Modify: `platform/android/jni/rd_jni.cpp`(新 native 方法)
- Modify: `samples/android/`(DamagedHelmet.glb 入 assets + 启动加载)

- [ ] **Step 1: JNI 方法**

`platform/android/jni/rd_jni.cpp` 追加(命名对齐既有 `Java_com_rd_renderer_RenderView_native*`):

```cpp
JNIEXPORT void JNICALL Java_com_rd_renderer_RenderView_nativeOnPointer(
    JNIEnv*, jobject, jlong ptr, jint action, jint id, jfloat x, jfloat y) {
  rd_engine_on_pointer(reinterpret_cast<rd_engine*>(ptr),
                       static_cast<rd_pointer_action_t>(action), id, x, y);
}
JNIEXPORT void JNICALL Java_com_rd_renderer_RenderView_nativeOnPinch(JNIEnv*, jobject,
                                                                     jlong ptr, jfloat ratio) {
  rd_engine_on_pinch(reinterpret_cast<rd_engine*>(ptr), ratio);
}
JNIEXPORT void JNICALL Java_com_rd_renderer_RenderView_nativeOnDoubleTap(JNIEnv*, jobject,
                                                                         jlong ptr, jfloat x,
                                                                         jfloat y) {
  rd_engine_on_double_tap(reinterpret_cast<rd_engine*>(ptr), x, y);
}
JNIEXPORT jint JNICALL Java_com_rd_renderer_RenderView_nativeLoadGltf(JNIEnv* env, jobject,
                                                                      jlong ptr, jstring path) {
  const char* p = env->GetStringUTFChars(path, nullptr);
  const jint r = jint(rd_engine_load_gltf(reinterpret_cast<rd_engine*>(ptr), p));
  env->ReleaseStringUTFChars(path, p);
  return r;
}
```

- [ ] **Step 2: RenderView.kt 手势**

类内追加(native 声明 + 手势探测器;**事件须经现有渲染线程投递机制**——
参照该类 renderFrame 的投递方式包一层):

```kotlin
    private external fun nativeOnPointer(engine: Long, action: Int, id: Int, x: Float, y: Float)
    private external fun nativeOnPinch(engine: Long, ratio: Float)
    private external fun nativeOnDoubleTap(engine: Long, x: Float, y: Float)
    private external fun nativeLoadGltf(engine: Long, path: String): Int

    private val scaleDetector = ScaleGestureDetector(context,
        object : ScaleGestureDetector.SimpleOnScaleGestureListener() {
            override fun onScale(d: ScaleGestureDetector): Boolean {
                postToRenderThread { nativeOnPinch(enginePtr, d.scaleFactor) }  // 机制名以现状为准
                return true
            }
        })

    @SuppressLint("ClickableViewAccessibility")
    override fun onTouchEvent(e: MotionEvent): Boolean {
        scaleDetector.onTouchEvent(e)
        val action = when (e.actionMasked) {
            MotionEvent.ACTION_DOWN, MotionEvent.ACTION_POINTER_DOWN -> 0
            MotionEvent.ACTION_MOVE -> 1
            MotionEvent.ACTION_UP, MotionEvent.ACTION_POINTER_UP -> 2
            MotionEvent.ACTION_CANCEL -> 3
            else -> -1
        }
        if (action >= 0) {
            val i = e.actionIndex
            val id = e.getPointerId(i)
            // ACTION_MOVE 需遍历全部指针(每个都算 MOVE)
            postToRenderThread {
                if (action == 1) {
                    for (k in 0 until e.pointerCount)
                        nativeOnPointer(enginePtr, 1, e.getPointerId(k), e.getX(k), e.getY(k))
                } else {
                    nativeOnPointer(enginePtr, action, id, e.getX(i), e.getY(i))
                }
            }
        }
        return true
    }

    /// 双击重置(GestureDetector 或手动两次 DOWN 间隔 <300ms)
    fun loadModel(path: String) {
        postToRenderThread { nativeLoadGltf(enginePtr, path) }
    }
```

双击:加 `GestureDetector.onDoubleTap` → `nativeOnDoubleTap`。
坐标:Android 事件坐标为逻辑像素——C API 约定像素坐标,须乘 density 或直接传
逻辑像素?**与 surface 尺寸(物理像素)对齐:乘 `resources.displayMetrics.density`**。
(`enginePtr`/`postToRenderThread` 以 RenderView.kt 现有成员名为准适配)

- [ ] **Step 3: demo 资产 + 加载**

- `tests/assets/DamagedHelmet.glb` 拷到 `samples/android/app/src/main/assets/DamagedHelmet.glb`。
- MainActivity(或现状入口)在 surface 就绪后:

```kotlin
// assets → filesDir(内核 v1 只支持文件路径)
val dst = File(filesDir, "DamagedHelmet.glb")
if (!dst.exists()) assets.open("DamagedHelmet.glb").use { it.copyTo(dst.outputStream()) }
renderView.loadModel(dst.absolutePath)
```

- [ ] **Step 4: 构建验证**

Run: `source /tmp/rd_env.sh && cd samples/android && ./gradlew :app:assembleDebug 2>&1 | tail -5`
Expected: BUILD SUCCESSFUL;模拟器运行截图 `img_check --min-coverage 0.03` 通过
(GLES 与 Vulkan 各跑一次——GLES 路径含 MSAA blit/blit vFlip 真机首验)

- [ ] **Step 5: Commit**

```bash
git add platform/android samples/android
git commit -m "feat(platform): Android 手势接线(探测器→JNI→C API)+ demo 加载 DamagedHelmet"
```

---

### Task 18: render_test --interactive(GLFW 窗口,Metal)

**Files:**
- Modify: `cmake/Deps.cmake`(host-only GLFW)
- Create: `tools/render_test/interactive.h`、`tools/render_test/interactive.mm`
- Modify: `tools/render_test/main.cpp`(--interactive 分支)
- Modify: `tools/render_test/CMakeLists.txt`

说明:macOS host 走 Metal swapchain(CAMetalLayer),与 iOS 同路径;
GLFW 只出窗口/事件(NO_API 模式),渲染全走 rd_engine C API(顺带验证 C API)。

- [ ] **Step 1: GLFW 接入**

`cmake/Deps.cmake` host 区(googletest 之后)追加:

```cmake
  # GLFW:render_test --interactive 的窗口/事件(host only,NO_API 模式)
  set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(glfw
    URL https://github.com/glfw/glfw/archive/refs/tags/3.4.tar.gz)
  FetchContent_MakeAvailable(glfw)
```

- [ ] **Step 2: interactive.h/.mm**

`tools/render_test/interactive.h`:

```cpp
// render_test --interactive:GLFW 窗口 + Metal swapchain + C API 鼠标交互(macOS)。
#pragma once
namespace rd::tool {
/// 打开交互窗口;modelPath 非空则加载模型。窗口关闭返回 0;初始化失败返回 1。
/// 环境变量 RD_INTERACTIVE_FRAMES=N:渲 N 帧后自动退出(冒烟用)。
int runInteractive(const char* modelPath);
} // namespace rd::tool
```

`tools/render_test/interactive.mm`:

```objc
#include "tools/render_test/interactive.h"
#include "api/rd_api.h"
#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3native.h>
#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace {
struct Ctx {
  rd_engine* engine = nullptr;
  bool dragging = false;
  double lastClickTime = 0;
};

void onMouseButton(GLFWwindow* w, int button, int action, int) {
  Ctx* c = static_cast<Ctx*>(glfwGetWindowUserPointer(w));
  if (button != GLFW_MOUSE_BUTTON_LEFT) return;
  double x, y;
  glfwGetCursorPos(w, &x, &y);
  float sx, sy;
  glfwGetWindowContentScale(w, &sx, &sy);
  const float px = float(x * sx), py = float(y * sy);
  if (action == GLFW_PRESS) {
    c->dragging = true;
    rd_engine_on_pointer(c->engine, RD_POINTER_DOWN, 0, px, py);
    // 手动双击检测(<0.3s)
    const double now = glfwGetTime();
    if (now - c->lastClickTime < 0.3) rd_engine_on_double_tap(c->engine, px, py);
    c->lastClickTime = now;
  } else {
    c->dragging = false;
    rd_engine_on_pointer(c->engine, RD_POINTER_UP, 0, px, py);
  }
}
void onCursorPos(GLFWwindow* w, double x, double y) {
  Ctx* c = static_cast<Ctx*>(glfwGetWindowUserPointer(w));
  if (!c->dragging) return;
  float sx, sy;
  glfwGetWindowContentScale(w, &sx, &sy);
  rd_engine_on_pointer(c->engine, RD_POINTER_MOVE, 0, float(x * sx), float(y * sy));
}
void onScroll(GLFWwindow* w, double, double dy) {
  Ctx* c = static_cast<Ctx*>(glfwGetWindowUserPointer(w));
  rd_engine_on_scroll(c->engine, float(dy * 120.0));  // 滚轮刻度 → 像素量纲
}
} // namespace

int rd::tool::runInteractive(const char* modelPath) {
  if (!glfwInit()) {
    fprintf(stderr, "glfwInit 失败(headless 环境?)\n");
    return 1;
  }
  glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);  // 渲染走 rd_engine,不需 GL 上下文
  GLFWwindow* win = glfwCreateWindow(960, 720, "render_test --interactive", nullptr, nullptr);
  if (!win) {
    glfwTerminate();
    return 1;
  }
  NSWindow* nswin = glfwGetCocoaWindow(win);
  NSView* view = [nswin contentView];
  [view setWantsLayer:YES];
  CAMetalLayer* layer = [CAMetalLayer layer];
  [view setLayer:layer];

  rd_engine* engine = rd_engine_create(RD_BACKEND_METAL);
  if (!engine) {
    glfwDestroyWindow(win);
    glfwTerminate();
    return 1;
  }
  int fbw = 0, fbh = 0;
  glfwGetFramebufferSize(win, &fbw, &fbh);
  layer.drawableSize = CGSizeMake(fbw, fbh);
  if (rd_engine_set_surface(engine, (__bridge void*)layer, uint32_t(fbw),
                            uint32_t(fbh)) != RD_OK) {
    fprintf(stderr, "set_surface 失败: %s\n", rd_get_last_error(engine));
    rd_engine_destroy(engine);
    glfwDestroyWindow(win);
    glfwTerminate();
    return 1;
  }
  if (modelPath && modelPath[0] &&
      rd_engine_load_gltf(engine, modelPath) != RD_OK)
    fprintf(stderr, "load_gltf 失败: %s\n", rd_get_last_error(engine));

  Ctx ctx;
  ctx.engine = engine;
  glfwSetWindowUserPointer(win, &ctx);
  glfwSetMouseButtonCallback(win, onMouseButton);
  glfwSetCursorPosCallback(win, onCursorPos);
  glfwSetScrollCallback(win, onScroll);

  const char* framesEnv = getenv("RD_INTERACTIVE_FRAMES");
  const long maxFrames = framesEnv ? atol(framesEnv) : 0;  // 0 = 不限
  auto last = std::chrono::steady_clock::now();
  long frame = 0;
  while (!glfwWindowShouldClose(win)) {
    glfwPollEvents();
    glfwGetFramebufferSize(win, &fbw, &fbh);
    CGSize cur = layer.drawableSize;
    if (int(cur.width) != fbw || int(cur.height) != fbh) {
      layer.drawableSize = CGSizeMake(fbw, fbh);
      rd_engine_resize(engine, uint32_t(fbw), uint32_t(fbh));
    }
    const auto now = std::chrono::steady_clock::now();
    const float dt = std::chrono::duration<float>(now - last).count();
    last = now;
    rd_engine_render_frame(engine, dt);
    if (maxFrames > 0 && ++frame >= maxFrames) break;
  }
  rd_engine_clear_surface(engine);
  rd_engine_destroy(engine);
  glfwDestroyWindow(win);
  glfwTerminate();
  return 0;
}
```

- [ ] **Step 3: main.cpp 分支 + CMake**

`main.cpp` 参数解析追加:

```cpp
    } else if (!strcmp(argv[i], "--interactive")) {
      interactive = true;
    }
```

(`bool interactive = false;` 声明;usage 注释更新)
main 开头分支:

```cpp
  if (interactive) return rd::tool::runInteractive(model.empty() ? nullptr : model.c_str());
```

(`#include "tools/render_test/interactive.h"`;头文件路径按 target_include_directories
现状决定——render_test 的 include 根含项目根/tests,照 main.cpp 既有 include 风格写)

`tools/render_test/CMakeLists.txt`:

```cmake
if(APPLE)
  target_sources(render_test PRIVATE interactive.mm)
  target_link_libraries(render_test PRIVATE glfw "-framework Cocoa" "-framework QuartzCore")
endif()
```

(target 名以该文件现状为准)

- [ ] **Step 4: 验证**

Run: `./scripts/check.sh 2>&1 | tail -5`(全绿,无回归)
Run: `RD_INTERACTIVE_FRAMES=30 ./build/tools/render_test/render_test --interactive --model tests/assets/DamagedHelmet.glb`
Expected: 窗口弹出演染 30 帧后自动退出,退出码 0;日志无报错
手动:不带环境变量运行,鼠标拖拽旋转/滚轮缩放/双击重置符合预期。

- [ ] **Step 5: Commit**

```bash
git add cmake/Deps.cmake tools/render_test
git commit -m "feat(tools): render_test --interactive(GLFW 窗口 + Metal swapchain + C API 鼠标交互)"
```

---

### Task 19: KTX2 golden + 双端 demo 验证

**Files:**
- Create: `tests/renderer/ktx2_render_test.cpp`
- Modify: `tests/CMakeLists.txt`(注册)
- Golden: 新增 `tests/golden/ktx2_tri_metal.png`、`ktx2_tri_vulkan.png`

- [ ] **Step 1: 写测试 + 生成 golden**

`tests/renderer/ktx2_render_test.cpp`:

```cpp
// KTX2 渲染 golden:运行时生成 basisu gltf(tri.ktx2 纹理)→ 按 caps 转码上传
// → Renderer 渲染 512x512 → golden 感知容差比对。验证转码+压缩上传+采样全链稳定。
#include <gtest/gtest.h>
#include "common/image.h"
#include "common/ktx2_gen.h"
#include "common/shader_code.h"
#include "renderer/renderer.h"
#include "resource/gltf_loader.h"
#include "resource/mesh_render_resource.h"
#include "scene/camera.h"
#include "scene/scene.h"
#include "rhi/rhi_device.h"
#include "rd_shader_dir.h"
#include <cstdlib>
#include <cstring>
#include <filesystem>

namespace {
constexpr uint32_t kW = 512, kH = 512;

// 与 gltf_test 的 BasisuExternalUri 同构:pos|uv 双 bufferView 三角形
std::string writeTriGltf(const std::filesystem::path& dir) {
  const float pos[9] = {-0.8f, -0.8f, 0, 0.8f, -0.8f, 0, 0, 0.8f, 0};
  const float uv[6] = {0, 0, 1, 0, 0.5f, 1};
  const uint16_t idx[3] = {0, 1, 2};
  const std::string binPath = (dir / "tri.bin").string();
  FILE* f = fopen(binPath.c_str(), "wb");
  fwrite(pos, 4, 9, f);
  fwrite(uv, 4, 6, f);
  fwrite(idx, 2, 3, f);
  fclose(f);
  const char* json = R"({
    "asset": {"version": "2.0"},
    "extensionsUsed": ["KHR_texture_basisu"],
    "scenes": [{"nodes": [0]}], "scene": 0,
    "nodes": [{"mesh": 0}],
    "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "TEXCOORD_0": 1},
                                "indices": 2, "material": 0}]}],
    "materials": [{"pbrMetallicRoughness": {"baseColorTexture": {"index": 0},
      "metallicFactor": 0.0, "roughnessFactor": 0.9}}],
    "textures": [{"extensions": {"KHR_texture_basisu": {"source": 0}}}],
    "images": [{"uri": "tex.ktx2"}],
    "buffers": [{"uri": "tri.bin", "byteLength": 66}],
    "bufferViews": [
      {"buffer": 0, "byteOffset": 0, "byteLength": 36},
      {"buffer": 0, "byteOffset": 36, "byteLength": 24},
      {"buffer": 0, "byteOffset": 60, "byteLength": 6}],
    "accessors": [
      {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3"},
      {"bufferView": 1, "componentType": 5126, "count": 3, "type": "VEC2"},
      {"bufferView": 2, "componentType": 5123, "count": 3, "type": "SCALAR"}]
  })";
  const std::string gltfPath = (dir / "tri.gltf").string();
  f = fopen(gltfPath.c_str(), "wb");
  fwrite(json, 1, strlen(json), f);
  fclose(f);
  return gltfPath;
}

void runGolden(rd::Backend b) {
  rd::DeviceDesc d;
  d.backend = b;
  auto device = rd::createDevice(d);
  ASSERT_NE(device, nullptr);
  const auto dir = std::filesystem::temp_directory_path() / "rd_ktx2_golden";
  std::filesystem::create_directories(dir);
  ASSERT_TRUE(rd::test::writeTestKtx2((dir / "tex.ktx2").string().c_str(), 64));
  const std::string gltfPath = writeTriGltf(dir);

  rd::TextureLoadPref pref;
  pref.ktx2Target = rd::pickTranscodeTarget(
      device->caps().supports(rd::Capability::texture_compression_astc),
      device->caps().supports(rd::Capability::texture_compression_etc2));
  auto model = rd::loadGltf(gltfPath.c_str(), pref);
  ASSERT_TRUE(model.valid());

  auto load = [&](const char* n) { return rd::test::loadShaderCode(b, RD_SHADER_DIR, n); };
  auto unlitVs = load("unlit.vert"), unlitFs = load("unlit.frag");
  auto pbrVs = load("pbr_forward.vert"), pbrFs = load("pbr_forward.frag");
  auto pfVs = load("prefilter.vert"), pfFs = load("prefilter.frag");
  auto blitVs = load("blit.vert"), blitFs = load("blit.frag");
  rd::Renderer renderer;
  rd::RendererShaderDesc sd{unlitVs.code, unlitFs.code, pbrVs.code, pbrFs.code,
                            pfVs.code,   pfFs.code,   blitVs.code, blitFs.code,
                            unlitVs.entry, rd::Format::RGBA8_UNORM};
  ASSERT_TRUE(renderer.init(*device, sd));
  rd::OffscreenTargetDesc td;
  td.width = kW;
  td.height = kH;
  td.depth = true;
  auto target = device->createOffscreenTarget(td);
  ASSERT_TRUE(target.valid());
  auto res = rd::MeshRenderResource::upload(*device, model);
  ASSERT_NE(res, nullptr);
  rd::scene::Scene scene;
  auto node = std::make_unique<rd::scene::MeshNode>();
  node->mesh = res;
  scene.root().addChild(std::move(node));
  rd::scene::Camera cam;
  cam.lookAt({0, 0, 2}, {0, 0, 0}, {0, 1, 0});
  cam.setPerspective(0.78539816f, 1.0f, 0.1f, 100.0f);

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
  res->destroy(*device);
  renderer.shutdown();
  const std::string name =
      b == rd::Backend::Metal ? "ktx2_tri_metal.png" : "ktx2_tri_vulkan.png";
  const std::string path = std::string(RD_TEST_DATA_DIR) + "/golden/" + name;
  if (std::getenv("RD_UPDATE_GOLDENS")) {
    ASSERT_TRUE(rd::test::savePNG(path, kW, kH, px.data()));
    return;
  }
  auto golden = rd::test::loadPNG(path);
  ASSERT_EQ(golden.pixels.size(), px.size()) << "golden 缺失: " << path;
  // KTX2 有损 + ASTC/ETC2 解码差:容差放宽(10/0.05)
  auto cmp = rd::test::compareRGBA8(px.data(), golden.pixels.data(), kW, kH, 10, 0.05);
  EXPECT_TRUE(cmp.pass) << "diffRatio=" << cmp.diffRatio;
}
} // namespace

TEST(Ktx2Render, MetalGolden) {
#if defined(__APPLE__)
  runGolden(rd::Backend::Metal);
#endif
}
TEST(Ktx2Render, VulkanGolden) {
#if defined(RD_WITH_VULKAN)
  runGolden(rd::Backend::Vulkan);
#endif
}
```

`tests/CMakeLists.txt` 追加 `renderer/ktx2_render_test.cpp`。

- [ ] **Step 2: 生成并核对 golden**

Run: `./scripts/check.sh 2>&1 | tail -8`(Ktx2Render 报 golden 缺失)
Run: `RD_UPDATE_GOLDENS=1 ctest --test-dir build --output-on-failure -R Ktx2Render`
**像素核对** `tests/golden/ktx2_tri_{metal,vulkan}.png`:红绿棋盘三角清晰、无翻转、
无双后端结构性差异(有损压缩的轻微色偏正常)。
Run: `./scripts/check.sh 2>&1 | tail -8` 全绿。

- [ ] **Step 3: 双端 demo 验证(手动确认点)**

- iOS 模拟器:build-ios 运行 sample,DamagedHelmet 显示正常,单指旋转/双指缩放/
  双击重置生效;截图 `img_check --min-coverage 0.03` 通过。
- Android 模拟器:GLES 与 Vulkan 各跑一次,同上手势验证 + 截图。
- host 交互:`render_test --interactive --model tests/assets/DamagedHelmet.glb` 鼠标验证。
- **任一不过**:回查对应 Task(GLES 问题优先看 blit vFlip 与 MSAA blit)。

- [ ] **Step 4: Commit**

```bash
git add tests/renderer/ktx2_render_test.cpp tests/CMakeLists.txt tests/golden/ktx2_tri_*.png
git commit -m "test(renderer): KTX2 渲染 golden(basisu 生成→转码→采样全链,双后端)"
```

---

### Task 20: AGENTS.md 收尾 + 全量回归

**Files:**
- Modify: `AGENTS.md`

- [ ] **Step 1: AGENTS.md 更新**

- 「当前状态」阶段二c 行改为完成态:

```markdown
- 阶段二 c 完成:Orbit 手势(旋转/pinch/平移/双击重置+惯性)+ 画质分级(三档预设
  + caps 启发式 + C API)+ KTX2(libktx,ASTC/ETC2/RGBA32 兜底)+ 上屏链
  (SceneTarget→blit upscale)+ engine 迁移 Renderer 链 + render_test --interactive
- 下一步:P2(阴影+多光源+后处理链+骨骼动画)
```

- 「代码约定」追加:

```markdown
- 压缩纹理:Format::ASTC_4x4_UNORM/ETC2_RGBA8_UNORM;caps
  texture_compression_astc/etc2 门控;上传按 formatMipBytes(block 上取整)计算
- 离屏目标可采样:Device::targetColorTexture(target)(MSAA 目标返回 resolve 纹理);
  targetSize 查询尺寸;OffscreenTargetDesc.sampleCount>1 创建 MSAA+resolve
- 上屏链:Renderer::endScene 两段(场景→内部 SceneTarget(renderScale/MSAA 按画质档)
  → blit upscale pass→最终目标);blit 纹理槽 0,UBO 块名 BlitUBO→slot0;
  GLES 渲染到纹理的 v 方向由 BlitUBO.params.x 翻转吸收
- 画质:C API rd_engine_set/get_quality(AUTO/HIGH/MID/LOW);AUTO=caps 启发式
- 输入 C API:rd_engine_on_pointer/on_scroll/on_pinch/on_double_tap(像素坐标);
  rd_engine_load_gltf 同步加载并 Orbit 自动取景
- KTX2:gltf KHR_texture_basisu + 外链 URI;转码目标 astc>etc2>rgba32(caps 推导);
  测试资产运行时生成(tests/common/ktx2_gen)
```

- 「下一步」P2 描述里删掉已过时的部分保持简洁(只改上面两处)。

- [ ] **Step 2: 全量回归**

Run: `./scripts/check.sh 2>&1 | tail -10`
Expected: 全绿
Run: `source /tmp/rd_env.sh && cd samples/android && ./gradlew :app:assembleDebug 2>&1 | tail -3`
Expected: BUILD SUCCESSFUL
Run: `cmake --build build-ios --config Debug 2>&1 | tail -3`
Expected: BUILD SUCCEEDED

- [ ] **Step 3: Commit**

```bash
git add AGENTS.md
git commit -m "docs: 阶段二c 收尾(AGENTS.md 新约定)"
```

---

## 附:已知风险与决策记录

| 风险/决策 | 处理 |
|---|---|
| ASTC 测试数据无编码器 | 契约测试只验创建/上传/链路;像素正确性由 KTX2 golden(libktx 转码)兜底 |
| GLES 无 host 测试 | GLES MSAA/vFlip 靠 Android 模拟器 demo + 截图验证(Task 17/19) |
| KTX-Software iOS 编译 | Task 16 验证;报错则按其 CMake 选项裁剪(保持 host/Android 先行) |
| engine 退役旋转立方体 | Task 13 起 demo 无模型只渲清屏色,Task 16/17 接模型;CubeScene 类保留给 RHI 测试 |
| 1:1 upscale 恒等性 | 双线性采样 texel 中心对齐恒等;golden 容差 3/0.02 兜底;翻转问题查 BlitUBO vFlip |
| Vulkan swapchain finalLayout 遗留 | 本阶段不动(现状可用);MSAA 只用于离屏 SceneTarget |
| Renderer 单目标格式假设 | 一个 Renderer 实例绑定 init 的 colorFormat;engine 用 swapChainColorFormat 初始化 |
| spec §5 "压缩格式失败→RGBA32 重转码" | 实际不会触发:转码目标由 device caps 推导,createTexture 不会拒;uploadOr 的占位回退是最后保险(不重转码,省一次 CPU 往返) |
| spec §5 "MSAA 失败→samples=1 重建" | 实现为更简单的 endScene 直接渲染到最终目标(scene==target 分支),并记错误日志 |


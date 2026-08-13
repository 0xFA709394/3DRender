# 第一阶段 RHI 底座强化 实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 按 `docs/superpowers/specs/2026-08-13-rhi-foundation-hardening-design.md` 重构 RHI 层：能力表、内存模型 flag 化、N 帧资源退休、管线缓存、instancing、cube face/mip 渲染目标、GLES 纹理补全与延迟回放，为 P1(glTF+PBR/IBL）打好底座。

**Architecture:** 保留 `Handle<Tag>` 强类型句柄与三后端绑定约定；在 `rhi::Device`/`CommandBuffer` 接口面上扩展；三后端（Vulkan/Metal/GLES）各自实现，能力差异经 `DeviceCaps` 上报。只允许破坏性重构窗口期内完成，验收 = `./scripts/check.sh` 全绿 + 三后端 golden image 一致。

**Tech Stack:** C++17、CMake、googletest、Vulkan(MoltenVK)/Metal/GLES3、glslang+spirv-cross 离线 shader 管线。

**通用约定（每个 task 都适用）:**
- 构建+测试命令：`./scripts/check.sh`（配置+构建+全部 ctest)
- 只跑新增测试：`ctest --test-dir build --output-on-failure -R <测试名正则>`
- 更新 golden:`RD_UPDATE_GOLDENS=1 ctest --test-dir build -R Cube` 后目视核对 `tests/golden/*.png`
- host 可测后端：Metal(macOS)+ Vulkan(MoltenVK);GLES 仅 Android，验证方式见 Task 6/8
- commit 规范：`<type>(<scope>): 描述`(feat/fix/build/test/chore/docs)
- 已知约束：当前 `CommandBuffer` 单缓冲串行模型，**每帧至多一次 submit**;`beginFrame/endFrame` 由渲染循环调用，离屏一次性渲染可靠 `waitIdle()` 兜底退休队列

---

### Task 1: 能力表系统（X-macro 单源 + DeviceCaps + 三后端上报）

**Files:**
- Create: `core/rhi/rhi_constants.inc.h`
- Create: `core/rhi/rhi_capability.h`
- Create: `core/rhi/rhi_capability.cpp`
- Modify: `core/rhi/rhi_device.h`(Device 加 `caps()`)
- Modify: `core/rhi/backends/vulkan/vulkan_device.cpp`(init 内上报）
- Modify: `core/rhi/backends/metal/metal_device.mm`(init 内上报）
- Modify: `core/rhi/backends/gles/gles_device.cpp`(init 内上报）
- Modify: `core/CMakeLists.txt`（加 rhi_capability.cpp)
- Test: `tests/rhi/caps_test.cpp`
- Modify: `tests/CMakeLists.txt`（加 caps_test.cpp)

- [ ] **Step 1: 写失败测试 `tests/rhi/caps_test.cpp`**

```cpp
// 能力表测试：三后端上报值健全性 + 名称表完整。
// Metal 用例在 __APPLE__ 下启用；Vulkan 用例在 RD_WITH_VULKAN 下启用（同 texture_test 模式）。
#include <gtest/gtest.h>
#include "rhi/rhi_device.h"
#include "rhi/rhi_capability.h"

namespace {
std::unique_ptr<rd::Device> make(rd::Backend b) {
  rd::DeviceDesc d;
  d.backend = b;
  return rd::createDevice(d);
}
} // namespace

// 名称表：每个枚举都有非空名字（X-macro 三处展开一致性）
TEST(Caps, NameTableComplete) {
  for (uint32_t i = 0; i < static_cast<uint32_t>(rd::Capability::kCount); ++i) {
    auto c = static_cast<rd::Capability>(i);
    EXPECT_STRNE(rd::DeviceCaps::name(c), "");
  }
}

TEST(Caps, MetalReports) {
#if defined(__APPLE__)
  auto dev = make(rd::Backend::Metal);
  ASSERT_NE(dev, nullptr);
  EXPECT_EQ(dev->caps().get(rd::Capability::instancing), 1u);
  EXPECT_GE(dev->caps().get(rd::Capability::max_texture_size), 4096u);
  EXPECT_TRUE(dev->caps().supports(rd::Capability::cube_render_target));
  EXPECT_TRUE(dev->caps().supports(rd::Capability::generate_mipmap));
#endif
}

TEST(Caps, VulkanReports) {
#if defined(RD_WITH_VULKAN)
  auto dev = make(rd::Backend::Vulkan);
  ASSERT_NE(dev, nullptr);
  EXPECT_EQ(dev->caps().get(rd::Capability::instancing), 1u);
  EXPECT_GE(dev->caps().get(rd::Capability::max_texture_size), 2048u);
  EXPECT_TRUE(dev->caps().supports(rd::Capability::cube_render_target));
#endif
}
```

`tests/CMakeLists.txt` 的 `add_executable(rd_tests ...)` 列表中加一行 `rhi/caps_test.cpp`。

- [ ] **Step 2: 跑测试确认编译失败**

Run: `cmake -S . -B build && cmake --build build -j8 2>&1 | grep -m3 "rhi_capability"`
Expected: 编译失败，`rhi/rhi_capability.h file not found`

- [ ] **Step 3: 创建能力表三件套**

`core/rhi/rhi_constants.inc.h`:

```cpp
// 能力枚举单源定义(X-macro):被 rhi_capability.h/cpp 多处展开。
// 值语义:0 = 不支持;非 0 为级别(如 msaa=4 表示最大 4x)。
#ifndef RD_CAPABILITY
#define RD_CAPABILITY(name)
#endif
RD_CAPABILITY(max_texture_size)         // 2D 纹理像素上限
RD_CAPABILITY(max_texture_slots)        // 纹理槽数(绑定约定上限 8)
RD_CAPABILITY(max_uniform_buffer_slots) // uniform 槽数(绑定约定上限 4)
RD_CAPABILITY(instancing)               // 实例化绘制
RD_CAPABILITY(msaa)                     // 最大 sample count(预留,P2 使用)
RD_CAPABILITY(depth_texture)            // 可采样深度附件(预留,P2 阴影)
RD_CAPABILITY(cube_render_target)       // 渲染到 cube 指定 face/mip(IBL 预滤波)
RD_CAPABILITY(generate_mipmap)          // 运行时 mip 生成
RD_CAPABILITY(anisotropy)               // 最大各向异性等级(0/1 = 不支持)
#undef RD_CAPABILITY
```

`core/rhi/rhi_capability.h`:

```cpp
/**
 * @file rhi_capability.h
 * @brief 设备能力表:能力枚举(X-macro 单源)+ DeviceCaps 查询容器。
 *
 * 能力探测集中收敛在各后端 init 内,上层只查表不直接碰扩展/特性位。
 * 缺省值 0 = 不支持;非 0 为级别(如 msaa=4)。
 */
#pragma once
#include <array>
#include <cstdint>

namespace rd {

/// 能力枚举;kCount 用于数组维度与遍历。
enum class Capability : uint32_t {
#define RD_CAPABILITY(name) name,
#include "rhi/rhi_constants.inc.h"
  kCount
};

/// 能力值容器:定长数组,缺省 0。后端 init 时 set 上报,之后只读。
class DeviceCaps {
public:
  /// 查询能力级别;0 = 不支持。
  uint32_t get(Capability c) const { return values_[static_cast<uint32_t>(c)]; }
  /// 能力是否支持(get != 0)。
  bool supports(Capability c) const { return get(c) != 0; }
  /// 能力名(日志/诊断);枚举与名表同源,不会越界。
  static const char* name(Capability c);

  /// 上报能力值(仅后端 init 内调用)。
  void set(Capability c, uint32_t v) { values_[static_cast<uint32_t>(c)] = v; }

private:
  std::array<uint32_t, static_cast<size_t>(Capability::kCount)> values_{};
};

} // namespace rd
```

`core/rhi/rhi_capability.cpp`:

```cpp
#include "rhi/rhi_capability.h"

namespace rd {

const char* DeviceCaps::name(Capability c) {
  static const char* kNames[] = {
#define RD_CAPABILITY(name) #name,
#include "rhi/rhi_constants.inc.h"
  };
  return kNames[static_cast<uint32_t>(c)];
}

} // namespace rd
```

`core/CMakeLists.txt` 的 `add_library(rd_core STATIC ...)` 列表中加一行 `rhi/rhi_capability.cpp`。

- [ ] **Step 4: Device 接口加 caps() + 三后端上报**

`core/rhi/rhi_device.h`:`#include "rhi/rhi_capability.h"`,`Device` 内加：

```cpp
  /// 返回本设备能力表(init 时上报,之后只读;缺省 0 = 不支持)。
  virtual const DeviceCaps& caps() const = 0;
```

Vulkan(`vulkan_device.cpp`):类内加成员 `DeviceCaps caps_;` 与 override;在 `init()` 选定 `phys_` 之后加：

```cpp
  // ---- 能力上报(能力探测集中在这里,上层只查表)----
  VkPhysicalDeviceProperties physProps;
  vkGetPhysicalDeviceProperties(phys_, &physProps);
  VkPhysicalDeviceFeatures physFeats;
  vkGetPhysicalDeviceFeatures(phys_, &physFeats);
  caps_.set(Capability::max_texture_size, physProps.limits.maxImageDimension2D);
  caps_.set(Capability::max_texture_slots, 8);
  caps_.set(Capability::max_uniform_buffer_slots, kMaxUniformSlots);
  caps_.set(Capability::instancing, 1);  // Vulkan 核心能力
  {
    // framebufferColorSampleCounts 是位掩码,取不超过 4 的最高档
    VkSampleCountFlags counts = physProps.limits.framebufferColorSampleCounts;
    caps_.set(Capability::msaa,
              counts & VK_SAMPLE_COUNT_4_BIT ? 4 : counts & VK_SAMPLE_COUNT_2_BIT ? 2 : 1);
  }
  caps_.set(Capability::depth_texture, 1);
  caps_.set(Capability::cube_render_target, 1);
  caps_.set(Capability::generate_mipmap, 1);
  caps_.set(Capability::anisotropy,
            physFeats.samplerAnisotropy
                ? static_cast<uint32_t>(physProps.limits.maxSamplerAnisotropy)
                : 0);
```

Metal(`metal_device.mm`)：类内加 `DeviceCaps caps_;` 与 override;`init()` 成功路径加：

```cpp
  // ---- 能力上报(Apple GPU 家族判定)----
  caps_.set(Capability::max_texture_size,
            [device_ supportsFamily:MTLGPUFamilyApple3] ? 16384u : 8192u);
  caps_.set(Capability::max_texture_slots, 8);
  caps_.set(Capability::max_uniform_buffer_slots, 4);
  caps_.set(Capability::instancing, 1);
  caps_.set(Capability::msaa, 4);   // Apple 全家族支持 4x MSAA
  caps_.set(Capability::depth_texture, 1);
  caps_.set(Capability::cube_render_target, 1);
  caps_.set(Capability::generate_mipmap, 1);
  caps_.set(Capability::anisotropy, 16);  // Apple GPU 实际支持 16
```

GLES(`gles_device.cpp`)：类内加 `DeviceCaps caps_;` 与 override;`init()` 末尾（`makeCurrent(pbuffer_)` 成功后）加：

```cpp
  // ---- 能力上报(ES3 核心能力 + 扩展位)----
  GLint maxTex = 0;
  glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTex);
  caps_.set(Capability::max_texture_size, static_cast<uint32_t>(maxTex));
  caps_.set(Capability::max_texture_slots, 8);
  caps_.set(Capability::max_uniform_buffer_slots, 4);
  caps_.set(Capability::instancing, 1);  // ES3 核心
  GLint maxSamples = 0;
  glGetIntegerv(GL_MAX_SAMPLES, &maxSamples);
  caps_.set(Capability::msaa,
            static_cast<uint32_t>(maxSamples >= 4 ? 4 : maxSamples >= 2 ? 2 : 1));
  caps_.set(Capability::depth_texture, 1);  // ES3 核心
  caps_.set(Capability::cube_render_target, 1);
  caps_.set(Capability::generate_mipmap, 1);
  const char* exts = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
  if (exts && strstr(exts, "GL_EXT_texture_filter_anisotropic")) {
    GLfloat maxAniso = 0;
    glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &maxAniso);
    caps_.set(Capability::anisotropy, static_cast<uint32_t>(maxAniso));
  }
```

（GLES 文件需 `#include <GLES3/gl3.h>` 已有；`GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT` 来自 `<GLES2/gl2ext.h>`，在 include 区加 `#include <GLES2/gl2ext.h>`。)

- [ ] **Step 5: 跑测试确认通过**

Run: `./scripts/check.sh 2>&1 | tail -5`
Expected: `100% tests passed`（含 `Caps.*` 3 个新用例）

- [ ] **Step 6: Commit**

```bash
git add core/rhi/rhi_constants.inc.h core/rhi/rhi_capability.h core/rhi/rhi_capability.cpp \
        core/rhi/rhi_device.h core/rhi/backends core/CMakeLists.txt \
        tests/rhi/caps_test.cpp tests/CMakeLists.txt
git commit -m "feat(rhi): 能力表系统(X-macro 单源 + DeviceCaps + 三后端上报)"
```

---

### Task 2: 内存模型 flag 化（host_write/host_read + TextureUsage + 各向异性）

**Files:**
- Modify: `core/rhi/rhi_types.h`(BufferDesc/TextureDesc/SamplerDesc)
- Modify: `core/rhi/backends/vulkan/vulkan_device.cpp`
- Modify: `core/rhi/backends/metal/metal_device.mm`
- Modify: `core/rhi/backends/gles/gles_device.cpp`
- Modify: `core/scene/cube_scene.cpp`(ubo 改 hostWrite)
- Modify: `tests/rhi/texture_quad_test.cpp`(vbo 初始化式适配）
- Modify: 其他 `createBuffer({` 调用点（grep 找出）
- Test: `tests/rhi/buffer_memory_test.cpp`（新）
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: 写失败测试 `tests/rhi/buffer_memory_test.cpp`**

```cpp
// 内存模型 flag 化测试:device-local 静态缓冲(带初始数据)可渲染;
// updateBuffer 作用于非 hostWrite 缓冲时拒绝并记日志(不生效)。
#include <gtest/gtest.h>
#include "rhi/rhi_device.h"

namespace {
std::unique_ptr<rd::Device> make(rd::Backend b) {
  rd::DeviceDesc d;
  d.backend = b;
  return rd::createDevice(d);
}
} // namespace

TEST(BufferMemory, DeviceLocalCreateWithData) {
#if defined(__APPLE__)
  auto dev = make(rd::Backend::Metal);
  ASSERT_NE(dev, nullptr);
  float data[4] = {1, 2, 3, 4};
  // hostWrite=false(默认)→ device-local + 内部 staging 上传
  auto buf = dev->createBuffer({sizeof(data), rd::BufferUsage::Vertex, false, false, data});
  EXPECT_TRUE(buf.valid());
  dev->destroyBuffer(buf);
#endif
}

TEST(BufferMemory, HostWriteUpdateRoundTrip) {
#if defined(__APPLE__)
  auto dev = make(rd::Backend::Metal);
  ASSERT_NE(dev, nullptr);
  auto buf = dev->createBuffer({64, rd::BufferUsage::Uniform, true, false, nullptr});
  ASSERT_TRUE(buf.valid());
  float v[16] = {};
  v[0] = 42.0f;
  dev->updateBuffer(buf, v, sizeof(v), 0);  // hostWrite 缓冲可更新
  dev->destroyBuffer(buf);
#endif
}

TEST(BufferMemory, VulkanDeviceLocalCreateWithData) {
#if defined(RD_WITH_VULKAN)
  auto dev = make(rd::Backend::Vulkan);
  ASSERT_NE(dev, nullptr);
  float data[4] = {1, 2, 3, 4};
  auto buf = dev->createBuffer({sizeof(data), rd::BufferUsage::Vertex, false, false, data});
  EXPECT_TRUE(buf.valid());
  dev->destroyBuffer(buf);
#endif
}
```

`tests/CMakeLists.txt` 加 `rhi/buffer_memory_test.cpp`。

- [ ] **Step 2: 跑测试确认编译失败**

Run: `cmake --build build -j8 2>&1 | grep -m3 error`
Expected: 编译失败（`createBuffer` 初始化式字段不匹配，新 Desc 尚未引入）

- [ ] **Step 3: 修改 `rhi_types.h` 三个 Desc**

`BufferDesc` 替换为（字段顺序变了，所有 braced init 调用点必须同步改——有意为之，强制显式迁移）:

```cpp
/// 缓冲创建参数。
struct BufferDesc {
  uint64_t size = 0;                          ///< 字节数
  BufferUsage usage = BufferUsage::Vertex;    ///< 用途位标志
  /// CPU 频繁写(动态 uniform/顶点)。false → device-local(渲染最快),
  /// 初始数据经内部 staging 上传,且之后 updateBuffer 会被拒绝(记日志)。
  bool hostWrite = false;
  bool hostRead = false;                      ///< CPU 回读(staging/截图用途)
  const void* data = nullptr;                 ///< 非空则创建时随带上传(大小须等于 size)
};
```

`TextureDesc` 加 usage flags（新增枚举放 `BufferUsage` 之后）:

```cpp
/// 纹理用途位标志。
enum class TextureUsage : uint32_t {
  Sampled = 1u << 0,                 ///< 可被 shader 采样(默认)
  RenderTargetAttachment = 1u << 1,  ///< 可作为渲染目标附件(cube face/mip 渲染)
};
constexpr TextureUsage operator|(TextureUsage a, TextureUsage b) {
  return static_cast<TextureUsage>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
constexpr bool hasFlag(TextureUsage value, TextureUsage flag) {
  return (static_cast<uint32_t>(value) & static_cast<uint32_t>(flag)) != 0;
}
```

`TextureDesc` 内加字段：`TextureUsage usage = TextureUsage::Sampled;`(type 字段之后）。

`SamplerDesc` 内加字段：`uint32_t maxAnisotropy = 1;  ///< >1 且 caps().anisotropy 支持时启用各向异性过滤`。

- [ ] **Step 4: Vulkan 后端适配**

`BufferRec` 加字段：`bool hostVisible = false;`

`createBuffer` 替换主体（usage 映射保持，内存属性与上传路径分轨）:

```cpp
BufferHandle VulkanDevice::createBuffer(const BufferDesc& desc) {
  VkBufferUsageFlags usage = 0;
  if (hasFlag(desc.usage, BufferUsage::Vertex)) usage |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
  if (hasFlag(desc.usage, BufferUsage::Index)) usage |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
  if (hasFlag(desc.usage, BufferUsage::Uniform)) usage |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
  const bool hostVisible = desc.hostWrite || desc.hostRead;
  if (!hostVisible) usage |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;  // staging 拷贝目标

  VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bci.size = desc.size;
  bci.usage = usage;
  bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  VkBuffer buffer;
  if (vkCreateBuffer(device_, &bci, nullptr, &buffer) != VK_SUCCESS) return {};

  VkMemoryRequirements req;
  vkGetBufferMemoryRequirements(device_, buffer, &req);
  VkMemoryPropertyFlags props =
      hostVisible ? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
                  : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
  VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  mai.allocationSize = req.size;
  mai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, props);
  VkDeviceMemory memory;
  if (mai.memoryTypeIndex == UINT32_MAX ||
      vkAllocateMemory(device_, &mai, nullptr, &memory) != VK_SUCCESS ||
      vkBindBufferMemory(device_, buffer, memory, 0) != VK_SUCCESS) {
    vkDestroyBuffer(device_, buffer, nullptr);
    return {};
  }
  BufferHandle h(nextId_++);
  buffers_.emplace(h, BufferRec{buffer, memory, hostVisible});
  if (desc.data) {
    if (hostVisible) {
      updateBuffer(h, desc.data, desc.size, 0);
    } else if (!stagingUploadBuffer(h, desc.data, desc.size)) {
      destroyBuffer(h);
      return {};
    }
  }
  return h;
}
```

新增私有方法（模式复用 createTexture 的 staging 流程：单 cmd + queueWaitIdle 串行）:

```cpp
/// device-local 缓冲的 staging 上传:临时 host 缓冲 → 单命令拷贝 → 等队列空闲。
bool VulkanDevice::stagingUploadBuffer(BufferHandle dst, const void* data, uint64_t size) {
  auto it = buffers_.find(dst);
  if (it == buffers_.end()) return false;
  VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bci.size = size;
  bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  VkBuffer staging = VK_NULL_HANDLE;
  VkDeviceMemory stagingMem = VK_NULL_HANDLE;
  bool ok = vkCreateBuffer(device_, &bci, nullptr, &staging) == VK_SUCCESS;
  if (ok) {
    VkMemoryRequirements sreq;
    vkGetBufferMemoryRequirements(device_, staging, &sreq);
    VkMemoryAllocateInfo smai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    smai.allocationSize = sreq.size;
    smai.memoryTypeIndex = findMemoryType(sreq.memoryTypeBits,
                                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    ok = smai.memoryTypeIndex != UINT32_MAX &&
         vkAllocateMemory(device_, &smai, nullptr, &stagingMem) == VK_SUCCESS &&
         vkBindBufferMemory(device_, staging, stagingMem, 0) == VK_SUCCESS;
  }
  if (ok) {
    void* mapped = nullptr;
    vkMapMemory(device_, stagingMem, 0, size, 0, &mapped);
    memcpy(mapped, data, size);
    vkUnmapMemory(device_, stagingMem);
    vkResetCommandBuffer(cmd_, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd_, &bi);
    VkBufferCopy copy{0, 0, size};
    vkCmdCopyBuffer(cmd_, staging, it->second.buffer, 1, &copy);
    vkEndCommandBuffer(cmd_);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd_;
    vkQueueSubmit(queue_, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue_);
  }
  if (staging) vkDestroyBuffer(device_, staging, nullptr);
  if (stagingMem) vkFreeMemory(device_, stagingMem, nullptr);
  if (!ok) RD_LOGE("rhi.vk", "stagingUploadBuffer 失败");
  return ok;
}
```

`updateBuffer` 加守卫：

```cpp
  if (!it->second.hostVisible) {
    RD_LOGE("rhi.vk", "updateBuffer 作用于 device-local 缓冲(须 hostWrite=true 创建)");
    return;
  }
```

各向异性（init 中 `VkDeviceCreateInfo` 之前查询并启用）:

```cpp
  VkPhysicalDeviceFeatures supportedFeats;
  vkGetPhysicalDeviceFeatures(phys_, &supportedFeats);
  VkPhysicalDeviceFeatures enableFeats{};
  enableFeats.samplerAnisotropy = supportedFeats.samplerAnisotropy;
  dci.pEnabledFeatures = &enableFeats;
```

`createSampler` 末尾 `sci.maxLod = ...` 之后加：

```cpp
  if (desc.maxAnisotropy > 1 && caps_.supports(Capability::anisotropy)) {
    sci.anisotropyEnable = VK_TRUE;
    sci.maxAnisotropy = std::min<float>(float(desc.maxAnisotropy),
                                        float(caps_.get(Capability::anisotropy)));
  }
```
（文件顶部 `#include <cmath>` 已有；`std::min` 需 `<algorithm>`，补 include。)

- [ ] **Step 5: Metal 后端适配**

`BufferRec` 加 `bool hostVisible = false;`。`createBuffer` 替换：

```cpp
  BufferHandle createBuffer(const BufferDesc& desc) override {
    const bool hostVisible = desc.hostWrite || desc.hostRead;
    id<MTLBuffer> b =
        [device_ newBufferWithLength:desc.size
                             options:hostVisible ? MTLResourceStorageModeShared
                                                 : MTLResourceStorageModePrivate];
    if (!b) return {};
    if (desc.data) {
      if (hostVisible) {
        memcpy(b.contents, desc.data, desc.size);
      } else {
        // Private 存储:经临时 Shared 缓冲 blit 上传(串行等完成,P0 风格)
        id<MTLBuffer> staging = [device_ newBufferWithBytes:desc.data
                                                     length:desc.size
                                                    options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> cb = [queue_ commandBuffer];
        id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
        [blit copyFromBuffer:staging sourceOffset:0 toBuffer:b destinationOffset:0 size:desc.size];
        [blit endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
      }
    }
    BufferHandle h(nextId_++);
    buffers_.emplace(h, BufferRec{b, hostVisible});
    return h;
  }
```

`updateBuffer` 加守卫：`if (!it->second.hostVisible) { RD_LOGE("rhi.metal", "updateBuffer 作用于 Private 缓冲(须 hostWrite=true 创建)"); return; }`

`createSampler` 在 `sd.rAddressMode = ...` 之后加：

```cpp
    if (desc.maxAnisotropy > 1 && caps_.supports(Capability::anisotropy)) {
      sd.maxAnisotropy = NSUInteger(std::min(desc.maxAnisotropy,
                                             caps_.get(Capability::anisotropy)));
    }
```

- [ ] **Step 6: GLES 后端适配 + 全部调用点迁移**

`gles_device.cpp` `createBuffer` 的 `glBufferData` usage 参数改为：

```cpp
  GLenum hint = desc.hostWrite ? GL_DYNAMIC_DRAW : GL_STATIC_DRAW;
  if (desc.hostRead)
    RD_LOGW("rhi.gles", "hostRead 缓冲在 GLES 无直接支持,按 hostWrite 处理");
  glBufferData(target, GLsizeipiptr(desc.size), desc.data, hint);
```
（注意修正笔误为 `GLsizeiptr`。)

调用点迁移（`grep -rn "createBuffer({" core/ tests/ tools/ platform/` 逐个核对）:
- `core/scene/cube_scene.cpp`:
  ```cpp
  vbo_ = device.createBuffer({sizeof(kVertices), BufferUsage::Vertex, false, false, kVertices});
  ibo_ = device.createBuffer({sizeof(kIndices), BufferUsage::Index, false, false, kIndices});
  ubo_ = device.createBuffer({64, BufferUsage::Uniform, true, false, nullptr});
  ```
- `tests/rhi/texture_quad_test.cpp`:vbo 为静态顶点 → `{sizeof(verts), BufferUsage::Vertex, false, false, verts}`（按现场变量名适配）。
- 其余命中的测试/工具：静态数据 → `false, false, data`；创建后还要 `updateBuffer` 的 → `true, false, ...`。

- [ ] **Step 7: 跑全部测试（含 golden 回归）**

Run: `./scripts/check.sh 2>&1 | tail -5`
Expected: `100% tests passed`（新用例 + Cube/TextureQuad golden 全绿；若有 golden 差异先排查是否渲染行为真的变了，不要直接更新 golden)

- [ ] **Step 8: Commit**

```bash
git add core/rhi core/scene/cube_scene.cpp tests/ tools/ platform/
git commit -m "feat(rhi): 内存模型 flag 化(host_write/host_read + TextureUsage + 各向异性采样)"
```

---

### Task 3: 帧括号 + N 帧资源退休

**Files:**
- Create: `core/rhi/retire_queue.h`
- Modify: `core/rhi/rhi_device.h`(Device 加 beginFrame/endFrame)
- Modify: `core/rhi/backends/vulkan/vulkan_device.cpp`
- Modify: `core/rhi/backends/metal/metal_device.mm`
- Modify: `core/rhi/backends/gles/gles_device.cpp`
- Modify: `core/api/rd_api.cpp`（渲染循环接帧括号）
- Modify: `core/CMakeLists.txt`(retire_queue.h 为纯头文件，无需加；仅确认）
- Test: `tests/rhi/retire_test.cpp`（新）
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: 写失败测试 `tests/rhi/retire_test.cpp`**

```cpp
// 资源退休测试:帧循环内高频 create/destroy(模拟每帧重建场景),
// 数千帧无崩溃;最后渲染一帧验证设备状态健康。
#include <gtest/gtest.h>
#include "rhi/rhi_device.h"

namespace {
std::unique_ptr<rd::Device> make(rd::Backend b) {
  rd::DeviceDesc d;
  d.backend = b;
  return rd::createDevice(d);
}
void churn(rd::Device& dev, int frames) {
  auto target = dev.createOffscreenTarget({64, 64});
  ASSERT_TRUE(target.valid());
  for (int i = 0; i < frames; ++i) {
    dev.beginFrame();
    float data[16] = {};
    auto buf = dev.createBuffer({sizeof(data), rd::BufferUsage::Vertex, true, false, data});
    EXPECT_TRUE(buf.valid());
    dev.destroyBuffer(buf);  // 本帧 destroy:句柄立即失效,底层资源退休延迟释放
    dev.endFrame();
  }
  dev.destroyTarget(target);
  dev.waitIdle();  // waitIdle 清空退休队列
}
} // namespace

TEST(Retire, MetalChurn) {
#if defined(__APPLE__)
  auto dev = make(rd::Backend::Metal);
  ASSERT_NE(dev, nullptr);
  churn(*dev, 2000);
#endif
}

TEST(Retire, VulkanChurn) {
#if defined(RD_WITH_VULKAN)
  auto dev = make(rd::Backend::Vulkan);
  ASSERT_NE(dev, nullptr);
  churn(*dev, 2000);
#endif
}
```

`tests/CMakeLists.txt` 加 `rhi/retire_test.cpp`。

- [ ] **Step 2: 跑测试确认编译失败**

Run: `cmake --build build -j8 2>&1 | grep -m3 error`
Expected: 编译失败（`beginFrame`/`endFrame` 不是 Device 成员）

- [ ] **Step 3: 创建 `core/rhi/retire_queue.h`**

```cpp
/**
 * @file retire_queue.h
 * @brief 资源退休队列:修复"destroy 时 GPU 可能仍在读"的隐患。
 *
 * 语义:destroyXxx 使句柄立即失效(调用方不得再用),底层资源的释放闭包
 * 打上当前帧序号入队;待该帧被后端的帧完成机制确认后才真正执行释放。
 * waitIdle()/设备析构 会清空队列(flushAll)。
 */
#pragma once
#include <cstdint>
#include <deque>
#include <functional>

namespace rd {

class RetireQueue {
public:
  /// 标记帧完成(≤ frame 的退休项全部执行);由后端帧完成机制驱动。
  void onFrameComplete(uint64_t frame) {
    if (frame > completedFrame_) completedFrame_ = frame;
    while (!queue_.empty() && queue_.front().first <= completedFrame_) {
      queue_.front().second();
      queue_.pop_front();
    }
  }
  /// 提交退休释放动作;frame 为当前帧序号。
  void retire(uint64_t frame, std::function<void()> fn) {
    queue_.emplace_back(frame, std::move(fn));
  }
  /// 强制全部执行(waitIdle/析构路径);帧序号单调不回退。
  void flushAll() {
    while (!queue_.empty()) {
      queue_.front().second();
      queue_.pop_front();
    }
  }
  /// 待退休数量(诊断/测试用)。
  size_t pending() const { return queue_.size(); }

private:
  uint64_t completedFrame_ = 0;
  std::deque<std::pair<uint64_t, std::function<void()>>> queue_;
};

} // namespace rd
```

`rhi_device.h` Device 加：

```cpp
  /// @name 帧括号
  /// @{
  /// 渲染循环每帧开始调用一次:帧序号推进。离屏一次性渲染可不调用。
  virtual void beginFrame() = 0;
  /// 每帧结束调用一次(present 之后):按帧完成机制推进资源退休。
  virtual void endFrame() = 0;
  /// @}
```

- [ ] **Step 4: Vulkan 接入（fence 驱动帧完成）**

类内加成员：

```cpp
  RetireQueue retire_;
  uint64_t frameIndex_ = 0;        ///< 当前帧序号(beginFrame 推进)
  uint64_t lastCompleted_ = 0;     ///< 已确认完成的帧序号
  VkFence frameFence_ = VK_NULL_HANDLE;  ///< 每帧 submit 的信号 fence
  bool fencePending_ = false;      ///< 本帧是否已 submit
```

`init()` 中（创建 acquireFence_ 之后）以同样方式创建 `frameFence_`。析构中销毁它，且在 `vkDeviceWaitIdle` 之后立刻 `retire_.flushAll();`。

新增/修改：

```cpp
void VulkanDevice::beginFrame() { ++frameIndex_; }

void VulkanDevice::endFrame() {
  if (fencePending_) {  // 本帧有 submit:等其完成(P0 串行模型下立即返回)
    vkWaitForFences(device_, 1, &frameFence_, VK_TRUE, UINT64_MAX);
    vkResetFences(device_, 1, &frameFence_);
    fencePending_ = false;
    lastCompleted_ = frameIndex_;
  }
  retire_.onFrameComplete(lastCompleted_);
}
```

`submit()` 的 `vkQueueSubmit` 第三个参数由 `VK_NULL_HANDLE` 改为 `frameFence_`，调用前置 `fencePending_ = true;`（注释：每帧至多一次 submit 的既有约束）。

`waitIdle()` 改为：

```cpp
void VulkanDevice::waitIdle() {
  vkQueueWaitIdle(queue_);
  lastCompleted_ = frameIndex_;
  retire_.flushAll();
}
```

所有 `destroyXxx` 改为退休模式（以 destroyBuffer 为例，其余 pipeline/shader/target/texture/sampler 同构）:

```cpp
void VulkanDevice::destroyBuffer(BufferHandle buffer) {
  auto it = buffers_.find(buffer);
  if (it == buffers_.end()) return;
  VkBuffer b = it->second.buffer;
  VkDeviceMemory m = it->second.memory;
  buffers_.erase(it);  // 句柄立即失效
  retire_.retire(frameIndex_, [this, b, m] {
    vkDestroyBuffer(device_, b, nullptr);
    vkFreeMemory(device_, m, nullptr);
  });
}
```

注意 `destroyTarget`/`destroySwapChain`/`resizeSwapChain`/`createSwapChain` 内已有 `vkDeviceWaitIdle` 的路径保持直接销毁不变（已安全）。

- [ ] **Step 5: Metal 接入（completedHandler 驱动）**

类内加成员：

```cpp
  RetireQueue retire_;
  uint64_t frameIndex_ = 0;
  std::atomic<uint64_t> completedFrame_{0};  ///< completedHandler 异步写
```
（`#include <atomic>`;`#include "rhi/retire_queue.h"`。)

`submit()` 改为：

```cpp
  void submit(CommandBuffer*) override {
    uint64_t f = frameIndex_;
    [cmdBuf_.cmd_ addCompletedHandler:^(id<MTLCommandBuffer>) {
      completedFrame_.store(f);  // 只写序号;释放在 endFrame 渲染线程执行
    }];
    [cmdBuf_.cmd_ commit];
    lastCmd_ = cmdBuf_.cmd_;
  }
```

新增：

```cpp
  void beginFrame() override { ++frameIndex_; }
  void endFrame() override { retire_.onFrameComplete(completedFrame_.load()); }
```

`waitIdle()` 末尾加 `retire_.flushAll();`。析构 `~MetalDevice()` 已有 `waitIdle()`，天然清空。

所有 `destroyXxx` 改退休模式（ARC 语义：lambda 持强引用，执行后释放）:

```cpp
  void destroyBuffer(BufferHandle buffer) override {
    auto it = buffers_.find(buffer);
    if (it == buffers_.end()) return;
    id<MTLBuffer> b = it->second.buffer;
    buffers_.erase(it);
    retire_.retire(frameIndex_, [b] { (void)b; });
  }
```
（pipeline/shader/target/texture/sampler 同理；swapchain 销毁路径保持直接释放——调用方已先 waitIdle。)

- [ ] **Step 6: GLES 接入（GL 规范保证删除安全）**

GLES 的 `glDelete*` 本身即"标记删除、不再被引用时释放"，单线程顺序执行下 destroy 立即生效是安全的。因此：

```cpp
  void beginFrame() override { ++frameIndex_; }
  void endFrame() override { retire_.onFrameComplete(frameIndex_); }  // 顺序执行,立即可退休
```
（类内加 `RetireQueue retire_; uint64_t frameIndex_ = 0;`;destroy 路径保持现状——记录这一点到文件头注释：GL 对象删除语义使退休队列为形式统一。)

`waitIdle()`(`glFinish`）末尾加 `retire_.flushAll();`。

- [ ] **Step 7: 渲染循环接入帧括号**

`core/api/rd_api.cpp` 的 `rd_engine_render_frame`:`acquireSwapChainTarget` 之前加 `e->device->beginFrame();`,`present` 之后加 `e->device->endFrame();`（两个 early-return 分支不需要——未 begin)。把 begin 放在所有 early return 之后、acquire 之前。

- [ ] **Step 8: 跑全部测试**

Run: `./scripts/check.sh 2>&1 | tail -5`
Expected: `100% tests passed`（含 `Retire.*`;golden 无变化）

- [ ] **Step 9: Commit**

```bash
git add core/rhi core/api/rd_api.cpp tests/
git commit -m "feat(rhi): beginFrame/endFrame 帧括号 + N 帧资源退休队列"
```

---

### Task 4: PipelineDesc 扩展（blend/depthWrite 拆分）+ 管线缓存

**Files:**
- Modify: `core/rhi/rhi_types.h`(BlendFactor/BlendDesc/PipelineDesc)
- Modify: `core/rhi/backends/vulkan/vulkan_device.cpp`（全局 pipeline layout + blend + 缓存）
- Modify: `core/rhi/backends/metal/metal_device.mm`(blend + 缓存）
- Modify: `core/rhi/backends/gles/gles_device.cpp`(blend 状态记录 + 缓存）
- Create: `shaders/blend.vert`、`shaders/blend.frag`
- Modify: `shaders/CMakeLists.txt`
- Test: `tests/rhi/blend_test.cpp`（新）
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: 新 shader + 失败测试**

`shaders/blend.vert`（全屏三角形，无顶点输入）:

```glsl
// blend.vert:无输入全屏三角形(gl_VertexIndex 技巧)。
#version 450
void main() {
  vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
```

`shaders/blend.frag`（常量半透明绿）:

```glsl
// blend.frag:输出常量半透明绿,用于 blend 契约测试。
#version 450
layout(location = 0) out vec4 outColor;
void main() { outColor = vec4(0.0, 1.0, 0.0, 0.5); }
```

`shaders/CMakeLists.txt` 加两行：`rd_compile_shader(blend.vert)` / `rd_compile_shader(blend.frag)`。

`tests/rhi/blend_test.cpp`:

```cpp
// blend 契约测试:清屏底色 (0.2,0,0,1) 上画半透明绿 (0,1,0,0.5)。
// blend 开 → (0.1, 0.5, 0);blend 关 → (0, 1, 0)。Metal/Vulkan 双后端。
#include <gtest/gtest.h>
#include "common/image.h"
#include "common/shader_code.h"
#include "rhi/rhi_device.h"
#include "rd_shader_dir.h"

namespace {
constexpr uint32_t kW = 64, kH = 64;

rd::test::Image renderQuad(rd::Backend b, bool blendEnable) {
  rd::DeviceDesc d;
  d.backend = b;
  auto device = rd::createDevice(d);
  if (!device) return {};
  auto vs = rd::test::loadShaderCode(b, RD_SHADER_DIR, "blend.vert");
  auto fs = rd::test::loadShaderCode(b, RD_SHADER_DIR, "blend.frag");
  auto vsm = device->createShaderModule({rd::ShaderStage::Vertex, vs.code, vs.entry});
  auto fsm = device->createShaderModule({rd::ShaderStage::Fragment, fs.code, fs.entry});
  rd::PipelineDesc pd;
  pd.vertexShader = vsm;
  pd.fragmentShader = fsm;
  pd.blend.enable = blendEnable;
  auto pipeline = device->createPipeline(pd);
  auto target = device->createOffscreenTarget({kW, kH});
  if (!pipeline.valid() || !target.valid()) return {};

  auto* cmd = device->acquireCommandBuffer();
  cmd->beginRenderPass(target, {0.2f, 0.0f, 0.0f, 1.0f});
  cmd->bindPipeline(pipeline);
  cmd->draw(3, 0);
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

// 中心像素(图像为顶向下行优先)
void expectCenter(const rd::test::Image& img, uint8_t r, uint8_t g, uint8_t b) {
  size_t idx = ((kH / 2) * kW + (kW / 2)) * 4;
  EXPECT_NEAR(img.pixels[idx + 0], r, 3);
  EXPECT_NEAR(img.pixels[idx + 1], g, 3);
  EXPECT_NEAR(img.pixels[idx + 2], b, 3);
}
} // namespace

TEST(Blend, MetalEnable) {
#if defined(__APPLE__)
  auto img = renderQuad(rd::Backend::Metal, true);
  ASSERT_EQ(img.pixels.size(), kW * kH * 4);
  expectCenter(img, 26, 128, 0);  // 0.2*0.5=0.1→26;1.0*0.5→128
#endif
}

TEST(Blend, MetalDisable) {
#if defined(__APPLE__)
  auto img = renderQuad(rd::Backend::Metal, false);
  ASSERT_EQ(img.pixels.size(), kW * kH * 4);
  expectCenter(img, 0, 255, 0);  // 直接覆盖
#endif
}

TEST(Blend, VulkanEnable) {
#if defined(RD_WITH_VULKAN)
  auto img = renderQuad(rd::Backend::Vulkan, true);
  ASSERT_EQ(img.pixels.size(), kW * kH * 4);
  expectCenter(img, 26, 128, 0);
#endif
}
```

`tests/CMakeLists.txt` 加 `rhi/blend_test.cpp`。

先确认 `tests/common/image.h` 的 `Image` 结构字段名（`pixels`/`width`/`height`)——若现场不同按现场适配。

- [ ] **Step 2: 跑测试确认编译失败**

Run: `cmake -S . -B build && cmake --build build -j8 2>&1 | grep -m3 error`
Expected: 编译失败（`PipelineDesc` 无 `blend` 成员）

- [ ] **Step 3: `rhi_types.h` 扩展**

`CullMode` 之后加：

```cpp
/// 混合因子(最小完备集;PBR 透明与常见 UI 合成足够)。
enum class BlendFactor { Zero, One, SrcAlpha, OneMinusSrcAlpha, DstAlpha, OneMinusDstAlpha };

/// 混合状态(默认关闭,经典 src-alpha 混合预设)。
struct BlendDesc {
  bool enable = false;
  BlendFactor srcColor = BlendFactor::SrcAlpha;
  BlendFactor dstColor = BlendFactor::OneMinusSrcAlpha;
  BlendFactor srcAlpha = BlendFactor::One;
  BlendFactor dstAlpha = BlendFactor::OneMinusDstAlpha;
};
```

`PipelineDesc` 修改：

```cpp
  bool depthTest = false;               ///< 深度测试(深度附件 P1 引入;当前三后端拒绝 true)
  bool depthWrite = false;              ///< 深度写入(与 depthTest 拆分;同样暂拒绝 true)
  BlendDesc blend;                      ///< 颜色混合
  uint32_t sampleCount = 1;             ///< MSAA 采样数(预留;>1 需 caps().msaa 支持,当前拒绝)
```

- [ ] **Step 4: Vulkan 实现（全局 layout + blend + 缓存）**

a) init 中（setLayout_ 创建之后）创建全局 pipeline layout:

```cpp
  VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  plci.setLayoutCount = 1;
  plci.pSetLayouts = &setLayout_;
  VK_CHECK(vkCreatePipelineLayout(device_, &plci, nullptr, &pipelineLayout_));
```
成员 `VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;`，析构中销毁。`PipelineRec` 改为持有共享缓存对象（见 c)。

b) createPipeline:depthTest/depthWrite/sampleCount 守卫：

```cpp
  if (desc.depthTest || desc.depthWrite) {
    RD_LOGE("rhi.vk", "深度附件 P1 引入,当前拒绝 depthTest/depthWrite");
    return {};
  }
  if (desc.sampleCount != 1) {
    RD_LOGE("rhi.vk", "MSAA 为 P2 预留,当前拒绝 sampleCount != 1");
    return {};
  }
```

blend 状态由 `desc.blend` 填充（新增映射函数）:

```cpp
VkBlendFactor toVkBlendFactor(BlendFactor f) {
  switch (f) {
    case BlendFactor::Zero: return VK_BLEND_FACTOR_ZERO;
    case BlendFactor::One: return VK_BLEND_FACTOR_ONE;
    case BlendFactor::SrcAlpha: return VK_BLEND_FACTOR_SRC_ALPHA;
    case BlendFactor::OneMinusSrcAlpha: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case BlendFactor::DstAlpha: return VK_BLEND_FACTOR_DST_ALPHA;
    case BlendFactor::OneMinusDstAlpha: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
  }
  return VK_BLEND_FACTOR_ONE;
}
```

`blendAttachment` 改为：

```cpp
  VkPipelineColorBlendAttachmentState blendAttachment{};
  blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                   VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  blendAttachment.blendEnable = desc.blend.enable ? VK_TRUE : VK_FALSE;
  blendAttachment.srcColorBlendFactor = toVkBlendFactor(desc.blend.srcColor);
  blendAttachment.dstColorBlendFactor = toVkBlendFactor(desc.blend.dstColor);
  blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
  blendAttachment.srcAlphaBlendFactor = toVkBlendFactor(desc.blend.srcAlpha);
  blendAttachment.dstAlphaBlendFactor = toVkBlendFactor(desc.blend.dstAlpha);
  blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
```

c) 管线缓存（文件匿名命名空间内）:

```cpp
struct PipelineKey {
  uint32_t vs = 0, fs = 0;
  uint32_t topology = 0, cull = 0;
  bool depthTest = false, depthWrite = false;
  bool blendEnable = false;
  uint32_t srcColor = 0, dstColor = 0, srcAlpha = 0, dstAlpha = 0;
  uint32_t colorFormat = 0;
  uint32_t sampleCount = 1;
  std::vector<VertexBinding> bindings;
  std::vector<VertexAttribute> attribs;
  bool operator==(const PipelineKey& o) const {
    return vs == o.vs && fs == o.fs && topology == o.topology && cull == o.cull &&
           depthTest == o.depthTest && depthWrite == o.depthWrite &&
           blendEnable == o.blendEnable && srcColor == o.srcColor && dstColor == o.dstColor &&
           srcAlpha == o.srcAlpha && dstAlpha == o.dstAlpha && colorFormat == o.colorFormat &&
           sampleCount == o.sampleCount && bindings == o.bindings && attribs == o.attribs;
  }
};
struct PipelineKeyHash {
  size_t operator()(const PipelineKey& k) const {
    size_t h = std::hash<uint64_t>()((uint64_t(k.vs) << 32) | k.fs);
    auto mix = [&h](size_t v) { h ^= v + 0x9e3779b9 + (h << 6) + (h >> 2); };
    mix(k.topology); mix(k.cull); mix(k.colorFormat); mix(k.sampleCount);
    mix(k.depthTest); mix(k.depthWrite); mix(k.blendEnable);
    mix(k.srcColor); mix(k.dstColor); mix(k.srcAlpha); mix(k.dstAlpha);
    for (const auto& b : k.bindings) mix((size_t(b.binding) << 8) | b.stride);
    for (const auto& a : k.attribs)
      mix((size_t(a.location) << 24) ^ (size_t(a.offset) << 8) ^ uint32_t(a.format) ^ a.binding);
    return h;
  }
};
```
（`VertexBinding`/`VertexAttribute` 需要 `operator==`——C++17 无默认比较，在 rhi_types.h 给两个 struct 各加手写 `bool operator==(const X& o) const { return ...; }`。)

`PipelineRec` 改为：

```cpp
struct CachedPipeline { VkPipeline pipeline; VkPrimitiveTopology topology; VkCullModeFlags cull; };
struct PipelineRec { std::shared_ptr<CachedPipeline> cached; };
```

成员 `std::unordered_map<PipelineKey, std::shared_ptr<CachedPipeline>, PipelineKeyHash> pipelineCache_;`;createPipeline 先查缓存（key 由 desc 构造：shader 句柄值 `.value()`、枚举转 uint32)，命中则直接包句柄返回；未命中走原创建路径（layout 用全局 `pipelineLayout_`）并把结果入缓存。`destroyPipeline` 只 erase 句柄表（缓存持有底层对象，设备析构时统一 vkDestroyPipeline)。`VulkanCommandBuffer::bindPipeline` 内 `rec.pipeline`→`rec.cached->pipeline`、`currentLayout_` 用 `device_->pipelineLayout()`。

- [ ] **Step 5: Metal 实现（blend + 缓存）**

createPipeline 加同样的 depthTest/depthWrite/sampleCount 守卫。blend:

```cpp
    auto toMTLBlend = [](BlendFactor f) {
      switch (f) {
        case BlendFactor::Zero: return MTLBlendFactorZero;
        case BlendFactor::One: return MTLBlendFactorOne;
        case BlendFactor::SrcAlpha: return MTLBlendFactorSourceAlpha;
        case BlendFactor::OneMinusSrcAlpha: return MTLBlendFactorOneMinusSourceAlpha;
        case BlendFactor::DstAlpha: return MTLBlendFactorDestinationAlpha;
        case BlendFactor::OneMinusDstAlpha: return MTLBlendFactorOneMinusDestinationAlpha;
      }
      return MTLBlendFactorOne;
    };
    pd.colorAttachments[0].blendingEnabled = desc.blend.enable;
    pd.colorAttachments[0].sourceRGBBlendFactor = toMTLBlend(desc.blend.srcColor);
    pd.colorAttachments[0].destinationRGBBlendFactor = toMTLBlend(desc.blend.dstColor);
    pd.colorAttachments[0].sourceAlphaBlendFactor = toMTLBlend(desc.blend.srcAlpha);
    pd.colorAttachments[0].destinationAlphaBlendFactor = toMTLBlend(desc.blend.dstAlpha);
```

缓存：与 Vulkan 同款的 `PipelineKey/Hash`（放各自文件匿名空间）;`std::unordered_map<PipelineKey, id<MTLRenderPipelineState>, PipelineKeyHash> pipelineCache_;`——id 由 ARC 强引用，命中时 `pipelines_.emplace(h, PipelineRec{cachedState, topology, cull})` 即可；句柄表 erase 不影响缓存。析构前清空 cache（在 waitIdle 之后）。

- [ ] **Step 6: GLES 实现（blend 状态记录 + 缓存）**

同样的守卫（depthTest 原拒绝逻辑扩展为 depthTest||depthWrite;sampleCount!=1 拒绝）。`PipelineRec` 加 `BlendDesc blend;`。`bindPipeline` 内落地：

```cpp
  if (rec.blend.enable) {
    glEnable(GL_BLEND);
    glBlendFuncSeparate(toGLBlendFactor(rec.blend.srcColor), toGLBlendFactor(rec.blend.dstColor),
                        toGLBlendFactor(rec.blend.srcAlpha), toGLBlendFactor(rec.blend.dstAlpha));
  } else {
    glDisable(GL_BLEND);
  }
```
（`toGLBlendFactor` 映射：Zero→GL_ZERO、One→GL_ONE、SrcAlpha→GL_SRC_ALPHA、OneMinusSrcAlpha→GL_ONE_MINUS_SRC_ALPHA、DstAlpha→GL_DST_ALPHA、OneMinusDstAlpha→GL_ONE_MINUS_DST_ALPHA。)

缓存：`PipelineKey` 同款；`std::unordered_map<PipelineKey, GLuint, PipelineKeyHash> programCache_;`；命中直接包句柄；device 析构（`shutdownEGL` 前）遍历 `glDeleteProgram` 并 `ensureOffscreenCurrent()` 前置。

- [ ] **Step 7: 跑全部测试**

Run: `./scripts/check.sh 2>&1 | tail -5`
Expected: `100% tests passed`（含 `Blend.*`;golden 无变化）

- [ ] **Step 8: Commit**

```bash
git add core/rhi shaders/ tests/
git commit -m "feat(rhi): PipelineDesc 扩展(blend/depthWrite 拆分/sampleCount 预留)+ 三后端管线缓存"
```

---

### Task 5: instancing 绘制（instance-rate 顶点绑定 + drawInstanced)

**Files:**
- Modify: `core/rhi/rhi_types.h`(VertexStepRate + VertexBinding.stepRate)
- Modify: `core/rhi/rhi_device.h`(CommandBuffer 加两个 draw）
- Modify: `core/rhi/backends/vulkan/vulkan_device.cpp`
- Modify: `core/rhi/backends/metal/metal_device.mm`
- Modify: `core/rhi/backends/gles/gles_device.cpp`
- Create: `shaders/inst.vert`、`shaders/inst.frag`
- Modify: `shaders/CMakeLists.txt`
- Test: `tests/rhi/instancing_test.cpp`（新）
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: 新 shader + 失败测试**

`shaders/inst.vert`:

```glsl
// inst.vert:实例化测试——小三角形按 per-instance 偏移平铺。
// location0=pos(vec2,binding0 每顶点);location1=offset(vec2,binding1 每实例)。
#version 450
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aOffset;
void main() { gl_Position = vec4(aPos + aOffset, 0.0, 1.0); }
```

`shaders/inst.frag`:

```glsl
#version 450
layout(location = 0) out vec4 outColor;
void main() { outColor = vec4(1.0, 0.0, 0.0, 1.0); }  // 常量红
```

`shaders/CMakeLists.txt` 加 `rd_compile_shader(inst.vert)` / `rd_compile_shader(inst.frag)`。

`tests/rhi/instancing_test.cpp`:

```cpp
// instancing 契约测试:per-instance 偏移把同一小三角形画到四个象限,
// readback 验证四象限中心为红、画面中心为背景色。Metal/Vulkan 双后端。
#include <gtest/gtest.h>
#include "common/image.h"
#include "common/shader_code.h"
#include "rhi/rhi_device.h"
#include "rd_shader_dir.h"

namespace {
constexpr uint32_t kW = 128, kH = 128;

rd::test::Image render(rd::Backend b) {
  rd::DeviceDesc d;
  d.backend = b;
  auto device = rd::createDevice(d);
  if (!device) return {};
  auto vs = rd::test::loadShaderCode(b, RD_SHADER_DIR, "inst.vert");
  auto fs = rd::test::loadShaderCode(b, RD_SHADER_DIR, "inst.frag");
  auto vsm = device->createShaderModule({rd::ShaderStage::Vertex, vs.code, vs.entry});
  auto fsm = device->createShaderModule({rd::ShaderStage::Fragment, fs.code, fs.entry});

  const float tri[] = {-0.15f, -0.15f, 0.15f, -0.15f, 0.0f, 0.15f};  // 小三角形
  const float offs[] = {-0.5f, -0.5f, 0.5f, -0.5f, -0.5f, 0.5f, 0.5f, 0.5f};  // 4 实例
  auto vbo = device->createBuffer({sizeof(tri), rd::BufferUsage::Vertex, false, false, tri});
  auto ibo = device->createBuffer({sizeof(offs), rd::BufferUsage::Vertex, false, false, offs});

  rd::PipelineDesc pd;
  pd.vertexShader = vsm;
  pd.fragmentShader = fsm;
  pd.vertexBindings = {{0, 8, rd::VertexStepRate::Vertex}, {1, 8, rd::VertexStepRate::Instance}};
  pd.attributes = {{0, rd::Format::R32G32_FLOAT, 0, 0}, {1, rd::Format::R32G32_FLOAT, 0, 1}};
  auto pipeline = device->createPipeline(pd);
  auto target = device->createOffscreenTarget({kW, kH});
  if (!pipeline.valid() || !target.valid() || !vbo.valid() || !ibo.valid()) return {};

  auto* cmd = device->acquireCommandBuffer();
  cmd->beginRenderPass(target, {0.0f, 0.0f, 0.0f, 1.0f});
  cmd->bindPipeline(pipeline);
  cmd->bindVertexBuffer(0, vbo, 0);
  cmd->bindVertexBuffer(1, ibo, 0);
  cmd->drawInstanced(3, 0, 4, 0);
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

bool isRed(const rd::test::Image& img, uint32_t x, uint32_t y) {
  size_t i = (size_t(y) * img.width + x) * 4;
  return img.pixels[i] > 150 && img.pixels[i + 1] < 60;
}
} // namespace

TEST(Instancing, MetalFourInstances) {
#if defined(__APPLE__)
  auto img = render(rd::Backend::Metal);
  ASSERT_EQ(img.pixels.size(), kW * kH * 4);
  // NDC(±0.5,±0.5) → 像素(32,96),(96,96),(32,32),(96,32)(顶向下)
  EXPECT_TRUE(isRed(img, 96, 32));   // (+0.5,+0.5) → 右下
  EXPECT_TRUE(isRed(img, 32, 32));   // (-0.5,+0.5) → 左下
  EXPECT_TRUE(isRed(img, 96, 96));   // (+0.5,-0.5) → 右上
  EXPECT_TRUE(isRed(img, 32, 96));   // (-0.5,-0.5) → 左上
  EXPECT_FALSE(isRed(img, 64, 64));  // 中心为背景
#endif
}

TEST(Instancing, VulkanFourInstances) {
#if defined(RD_WITH_VULKAN)
  auto img = render(rd::Backend::Vulkan);
  ASSERT_EQ(img.pixels.size(), kW * kH * 4);
  EXPECT_TRUE(isRed(img, 96, 32));
  EXPECT_TRUE(isRed(img, 32, 96));
  EXPECT_FALSE(isRed(img, 64, 64));
#endif
}
```

`tests/CMakeLists.txt` 加 `rhi/instancing_test.cpp`。

- [ ] **Step 2: 跑测试确认编译失败**

Run: `cmake -S . -B build && cmake --build build -j8 2>&1 | grep -m3 error`
Expected: 编译失败（`VertexStepRate`/`drawInstanced` 未定义）

- [ ] **Step 3: 接口扩展**

`rhi_types.h`:

```cpp
/// 顶点缓冲步进频率:每顶点 / 每实例。
enum class VertexStepRate : uint32_t { Vertex, Instance };

struct VertexBinding {
  uint32_t binding = 0;
  uint32_t stride = 0;
  VertexStepRate stepRate = VertexStepRate::Vertex;  ///< Instance = 每实例步进
  bool operator==(const VertexBinding& o) const {
    return binding == o.binding && stride == o.stride && stepRate == o.stepRate;
  }
};
```

（若 Task 4 已给 VertexBinding 加过 operator==，替换为含 stepRate 的版本。)

`rhi_device.h` CommandBuffer 加：

```cpp
  /// 实例化非索引绘制。firstInstance 在 GLES(ES3.0) 不受支持,非 0 时记警告按 0 处理。
  virtual void drawInstanced(uint32_t vertexCount, uint32_t firstVertex,
                             uint32_t instanceCount, uint32_t firstInstance) = 0;
  /// 实例化索引绘制;firstInstance 同上 GLES 限制。
  virtual void drawIndexedInstanced(uint32_t indexCount, uint32_t firstIndex,
                                    int32_t vertexOffset, uint32_t instanceCount,
                                    uint32_t firstInstance) = 0;
```

- [ ] **Step 4: 三后端实现**

Vulkan:`createPipeline` 顶点绑定行改为 `{b.binding, b.stride, b.stepRate == VertexStepRate::Instance ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX}`;CommandBuffer 加：

```cpp
void VulkanCommandBuffer::drawInstanced(uint32_t vertexCount, uint32_t firstVertex,
                                        uint32_t instanceCount, uint32_t firstInstance) {
  vkCmdDraw(cmd_, vertexCount, instanceCount, firstVertex, firstInstance);
}
void VulkanCommandBuffer::drawIndexedInstanced(uint32_t indexCount, uint32_t firstIndex,
                                               int32_t vertexOffset, uint32_t instanceCount,
                                               uint32_t firstInstance) {
  vkCmdDrawIndexed(cmd_, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
}
```

Metal:`createPipeline` 的 vertex layout:`vd.layouts[b.binding + 1].stepFunction = b.stepRate == VertexStepRate::Instance ? MTLVertexStepFunctionPerInstance : MTLVertexStepFunctionPerVertex;`;CommandBuffer 加：

```cpp
void MetalCommandBuffer::drawInstanced(uint32_t vertexCount, uint32_t firstVertex,
                                       uint32_t instanceCount, uint32_t firstInstance) {
  [encoder_ drawPrimitives:pipeline_.topology
               vertexStart:firstVertex
               vertexCount:vertexCount
             instanceCount:instanceCount
              baseInstance:firstInstance];
}
void MetalCommandBuffer::drawIndexedInstanced(uint32_t indexCount, uint32_t firstIndex,
                                              int32_t vertexOffset, uint32_t instanceCount,
                                              uint32_t firstInstance) {
  const bool u16 = indexType_ == IndexType::UInt16;
  [encoder_ drawIndexedPrimitives:pipeline_.topology
                       indexCount:indexCount
                        indexType:u16 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32
                      indexBuffer:device_->buffer(indexBuffer_)
                indexBufferOffset:indexOffset_ + firstIndex * (u16 ? 2 : 4)
                    instanceCount:instanceCount
                       baseVertex:vertexOffset
                     baseInstance:firstInstance];
}
```

GLES：`applyVertexState` 内在 `glVertexAttribPointer` 之后按属性所在 binding 的 stepRate 显式设置 divisor（状态有粘性，必须每次都设）:

```cpp
    // 在 bindings 中查找 stepRate(与 stride 同一次线性查找)
    uint32_t stride = 0;
    VertexStepRate rate = VertexStepRate::Vertex;
    for (const auto& b : pipeline_.bindings) {
      if (b.binding == a.binding) { stride = b.stride; rate = b.stepRate; }
    }
    ...
    glVertexAttribDivisor(a.location, rate == VertexStepRate::Instance ? 1 : 0);
```
（`PipelineRec` 的 `bindings` 已是 `std::vector<VertexBinding>`，自动携带新字段。)CommandBuffer 加：

```cpp
void GLESCommandBuffer::drawInstanced(uint32_t vertexCount, uint32_t firstVertex,
                                      uint32_t instanceCount, uint32_t firstInstance) {
  if (firstInstance != 0)
    RD_LOGW("rhi.gles", "ES3.0 不支持 firstInstance,按 0 处理");
  applyVertexState();
  glDrawArraysInstanced(pipeline_.topology, GLint(firstVertex), GLsizei(vertexCount),
                        GLsizei(instanceCount));
}
void GLESCommandBuffer::drawIndexedInstanced(uint32_t indexCount, uint32_t firstIndex,
                                             int32_t vertexOffset, uint32_t instanceCount,
                                             uint32_t firstInstance) {
  if (vertexOffset != 0 || firstInstance != 0)
    RD_LOGW("rhi.gles", "ES3.0 不支持 baseVertex/baseInstance,按 0 处理");
  applyVertexState();
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, device_->buffer(indexBuffer_));
  const bool u16 = indexType_ == IndexType::UInt16;
  glDrawElementsInstanced(pipeline_.topology, GLsizei(indexCount),
                          u16 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT,
                          reinterpret_cast<const void*>(uintptr_t(indexOffset_ + firstIndex * (u16 ? 2 : 4))),
                          GLsizei(instanceCount));
}
```

- [ ] **Step 5: 跑全部测试**

Run: `./scripts/check.sh 2>&1 | tail -5`
Expected: `100% tests passed`（含 `Instancing.*`)

- [ ] **Step 6: Commit**

```bash
git add core/rhi shaders/ tests/
git commit -m "feat(rhi): instancing 绘制(instance-rate 顶点绑定 + drawInstanced 三后端)"
```

---

### Task 6: GLES 纹理/采样器实现（spec 补充项）

> 计划阶段发现的 spec 缺口：GLES 后端的 `createTexture/createSampler/bindTexture` 在 P0 是桩（返回无效句柄/空操作）,P1 PBR 需要三后端纹理能力对齐。本节补齐，spec §6 提交清单同步更新。

**Files:**
- Modify: `core/rhi/backends/gles/gles_device.cpp`
- Modify: `core/rhi/rhi_types.h`（绑定约定注释补 GLES sampler 命名约定）
- 验证：Android 构建 + 模拟器截图（host 无法跑 GLES)

- [ ] **Step 1: 实现 GLES 纹理/采样器**

`gles_device.cpp` 资源记录区加：

```cpp
struct TextureRec {
  GLuint tex = 0;
  GLenum target = GL_TEXTURE_2D;  // 或 GL_TEXTURE_CUBE_MAP
  uint32_t width = 0, height = 0, mipLevels = 1;
};
struct SamplerRec { GLuint sampler = 0; };
```

`GLESDevice` 加 `std::unordered_map<TextureHandle, TextureRec> textures_;` 与 `std::unordered_map<SamplerHandle, SamplerRec> samplers_;`，加查询辅助（与 buffer() 同模式）。原桩函数替换：

```cpp
TextureHandle GLESDevice::createTexture(const TextureDesc& desc) {
  if (desc.width == 0 || desc.height == 0 || desc.mipLevels == 0) return {};
  if (desc.type == TextureType::Cube && desc.width != desc.height) {
    RD_LOGE("rhi.gles", "createTexture: cube 纹理必须方形");
    return {};
  }
  const uint32_t maxDim = desc.width > desc.height ? desc.width : desc.height;
  const uint32_t maxMip = uint32_t(std::floor(std::log2(double(maxDim)))) + 1;
  if (desc.mipLevels > maxMip) return {};
  ensureOffscreenCurrent();
  TextureRec rec;
  rec.target = desc.type == TextureType::Cube ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D;
  rec.width = desc.width; rec.height = desc.height; rec.mipLevels = desc.mipLevels;
  glGenTextures(1, &rec.tex);
  glBindTexture(rec.target, rec.tex);
  // 数据布局:面 × mip 紧凑排列(与其他后端一致);无数据则分配空存储
  const uint32_t fmtSize = formatSize(desc.format);
  const bool rgba8 = desc.format == Format::RGBA8_UNORM;
  const uint8_t* src = static_cast<const uint8_t*>(desc.data);
  uint64_t offset = 0;
  const uint32_t faces = desc.type == TextureType::Cube ? 6 : 1;
  for (uint32_t face = 0; face < faces; ++face) {
    uint32_t w = desc.width, h = desc.height;
    for (uint32_t mip = 0; mip < desc.mipLevels; ++mip) {
      const uint64_t bytes = uint64_t(w) * h * fmtSize;
      if (src && offset + bytes > desc.dataSize) {
        RD_LOGE("rhi.gles", "createTexture: 数据越界");
        glDeleteTextures(1, &rec.tex);
        return {};
      }
      GLenum faceTarget = rec.target == GL_TEXTURE_CUBE_MAP
                              ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + face : GL_TEXTURE_2D;
      glTexImage2D(faceTarget, GLint(mip), rgba8 ? GL_RGBA8 : GL_RGBA32F, GLsizei(w), GLsizei(h),
                   0, GL_RGBA, rgba8 ? GL_UNSIGNED_BYTE : GL_FLOAT,
                   src ? src + offset : nullptr);
      offset += bytes;
      w = w > 1 ? w / 2 : 1;
      h = h > 1 ? h / 2 : 1;
    }
  }
  glBindTexture(rec.target, 0);
  TextureHandle h(nextId_++);
  textures_.emplace(h, rec);
  return h;
}

void GLESDevice::destroyTexture(TextureHandle texture) {
  auto it = textures_.find(texture);
  if (it == textures_.end()) return;
  ensureOffscreenCurrent();
  glDeleteTextures(1, &it->second.tex);
  textures_.erase(it);
}

SamplerHandle GLESDevice::createSampler(const SamplerDesc& desc) {
  ensureOffscreenCurrent();
  GLuint s = 0;
  glGenSamplers(1, &s);
  // minFilter 综合 min+mip(ES 枚举是联合形式):线性 min + 线性 mip → 三线性
  GLint minFilter = desc.minFilter == Filter::Linear
                        ? (desc.mipFilter == Filter::Linear ? GL_LINEAR_MIPMAP_LINEAR
                                                            : GL_LINEAR_MIPMAP_NEAREST)
                        : (desc.mipFilter == Filter::Linear ? GL_NEAREST_MIPMAP_LINEAR
                                                            : GL_NEAREST_MIPMAP_NEAREST);
  glSamplerParameteri(s, GL_TEXTURE_MIN_FILTER, minFilter);
  glSamplerParameteri(s, GL_TEXTURE_MAG_FILTER,
                      desc.magFilter == Filter::Linear ? GL_LINEAR : GL_NEAREST);
  auto toWrap = [](WrapMode m) {
    return m == WrapMode::Repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE;
  };
  glSamplerParameteri(s, GL_TEXTURE_WRAP_S, toWrap(desc.wrapU));
  glSamplerParameteri(s, GL_TEXTURE_WRAP_T, toWrap(desc.wrapV));
  glSamplerParameteri(s, GL_TEXTURE_WRAP_R, toWrap(desc.wrapW));
  if (desc.maxAnisotropy > 1 && caps_.supports(Capability::anisotropy)) {
    GLfloat a = GLfloat(std::min(desc.maxAnisotropy, caps_.get(Capability::anisotropy)));
    glSamplerParameterf(s, GL_TEXTURE_MAX_ANISOTROPY_EXT, a);
  }
  SamplerHandle h(nextId_++);
  samplers_.emplace(h, SamplerRec{s});
  return h;
}

void GLESDevice::destroySampler(SamplerHandle sampler) {
  auto it = samplers_.find(sampler);
  if (it == samplers_.end()) return;
  ensureOffscreenCurrent();
  glDeleteSamplers(1, &it->second.sampler);
  samplers_.erase(it);
}
```
（`std::min` 需 `<algorithm>`；文件需 `#include <cmath>` + `<algorithm>`。)

`GLESCommandBuffer::bindTexture` 空操作替换为：

```cpp
void GLESCommandBuffer::bindTexture(uint32_t slot, TextureHandle texture, SamplerHandle sampler) {
  const TextureRec* tex = device_->textureRec(texture);
  GLuint sam = device_->samplerGl(sampler);
  if (!tex || sam == 0) return;
  glActiveTexture(GL_TEXTURE0 + slot);
  glBindTexture(tex->target, tex->tex);
  glBindSampler(slot, sam);
  // sampler uniform 命名约定:texN ↔ slot N(见 rhi_types.h 绑定约定)
  char name[8];
  snprintf(name, sizeof(name), "tex%u", slot);
  GLint loc = glGetUniformLocation(pipeline_.program, name);
  if (loc >= 0) glUniform1i(loc, GLint(slot));
}
```

`rhi_types.h` 绑定约定注释的 GLES 行更新为：`↔ GLES 纹理单元 N(sampler uniform 名:texN)`。

- [ ] **Step 2: Android 构建验证（GLES 仅 Android 可编）**

Run: `source /tmp/rd_env.sh && cd samples/android && ./gradlew :app:assembleDebug 2>&1 | tail -3`
Expected: `BUILD SUCCESSFUL`

- [ ] **Step 3: 模拟器运行时验证**

运行 Android 模拟器 demo（现有 RenderView 链路），`img_check` 截图校验立方体正常渲染（GLES 路径不经过新纹理代码，本步骤验证无回归）:

Run: `./build/tools/img_check/img_check <截图>.png --min-coverage 0.03`
Expected: 通过

- [ ] **Step 4: host 回归**

Run: `./scripts/check.sh 2>&1 | tail -3`
Expected: `100% tests passed`(GLES 代码不参编 host，确认 Metal/Vulkan 无回归）

- [ ] **Step 5: Commit**

```bash
git add core/rhi
git commit -m "feat(rhi): GLES 纹理/采样器实现(createTexture/createSampler/bindTexture)"
```

---

### Task 7: cube face/mip 渲染目标 + updateTexture + generateMipmaps

**Files:**
- Modify: `core/rhi/rhi_types.h`(OffscreenTargetDesc 扩展）
- Modify: `core/rhi/rhi_device.h`(Device 加 updateTexture/generateMipmaps)
- Modify: `core/rhi/backends/vulkan/vulkan_device.cpp`(per-subresource layout 追踪）
- Modify: `core/rhi/backends/metal/metal_device.mm`
- Modify: `core/rhi/backends/gles/gles_device.cpp`
- Create: `shaders/texcube.vert`、`shaders/texcube.frag`
- Modify: `shaders/CMakeLists.txt`
- Test: `tests/rhi/cube_target_test.cpp`（新）
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: 新 shader + 失败测试**

`shaders/texcube.vert`（全屏三角形透传）:

```glsl
#version 450
void main() {
  vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
```

`shaders/texcube.frag`:

```glsl
// texcube.frag:按 uniform 方向采样 cubemap 输出。
// UBO binding0 ↔ uniform slot 0;samplerCube binding4 ↔ texture slot 0。
#version 450
layout(binding = 0) uniform UBO { vec3 dir; };
layout(binding = 4) uniform samplerCube tex0;
layout(location = 0) out vec4 outColor;
void main() { outColor = texture(tex0, dir); }
```

`shaders/CMakeLists.txt` 加 `rd_compile_shader(texcube.vert)` / `rd_compile_shader(texcube.frag)`。

`tests/rhi/cube_target_test.cpp`:

```cpp
// cube face 渲染目标契约测试:向 cube 每个 face 清屏不同颜色,
// 再用 texcube 采样 +X 面验证内容正确。同时覆盖 generateMipmaps 返回值。
#include <gtest/gtest.h>
#include <cstring>
#include "common/image.h"
#include "common/shader_code.h"
#include "rhi/rhi_device.h"
#include "rd_shader_dir.h"

namespace {
constexpr uint32_t kCube = 64;
constexpr uint32_t kW = 64, kH = 64;

// 向六个面分别清屏(r,g,b 按面序递增),返回设备与纹理(out 参数)
void renderFaces(rd::Device& dev, rd::TextureHandle tex) {
  for (uint32_t face = 0; face < 6; ++face) {
    rd::OffscreenTargetDesc td;
    td.width = kCube;
    td.height = kCube;
    td.colorFromTexture = tex;
    td.face = face;
    td.mipLevel = 0;
    auto target = dev.createOffscreenTarget(td);
    if (!target.valid()) continue;
    auto* cmd = dev.acquireCommandBuffer();
    cmd->beginRenderPass(target,
                         {float(40 * (face + 1)) / 255.0f, 0.1f, 0.2f, 1.0f});
    cmd->endRenderPass();
    dev.submit(cmd);
    dev.waitIdle();
    dev.destroyTarget(target);
  }
}
} // namespace

TEST(CubeTarget, MetalRenderToFaceAndSample) {
#if defined(__APPLE__)
  rd::DeviceDesc d;
  d.backend = rd::Backend::Metal;
  auto dev = rd::createDevice(d);
  ASSERT_NE(dev, nullptr);
  ASSERT_TRUE(dev->caps().supports(rd::Capability::cube_render_target));

  rd::TextureDesc td;
  td.type = rd::TextureType::Cube;
  td.width = kCube;
  td.height = kCube;
  td.usage = rd::TextureUsage::Sampled | rd::TextureUsage::RenderTargetAttachment;
  auto tex = dev->createTexture(td);
  ASSERT_TRUE(tex.valid());
  renderFaces(*dev, tex);

  // 采样 +X 面(face0,清屏色 (40,26,51,255) ≈ 0.157,0.1,0.2)
  auto vs = rd::test::loadShaderCode(rd::Backend::Metal, RD_SHADER_DIR, "texcube.vert");
  auto fs = rd::test::loadShaderCode(rd::Backend::Metal, RD_SHADER_DIR, "texcube.frag");
  auto vsm = dev->createShaderModule({rd::ShaderStage::Vertex, vs.code, vs.entry});
  auto fsm = dev->createShaderModule({rd::ShaderStage::Fragment, fs.code, fs.entry});
  auto ubo = dev->createBuffer({16, rd::BufferUsage::Uniform, true, false, nullptr});
  const float dir[4] = {1.0f, 0.0f, 0.0f, 0.0f};
  dev->updateBuffer(ubo, dir, sizeof(dir), 0);
  auto sampler = dev->createSampler({});
  rd::PipelineDesc pd;
  pd.vertexShader = vsm;
  pd.fragmentShader = fsm;
  auto pipeline = dev->createPipeline(pd);
  auto target = dev->createOffscreenTarget({kW, kH});
  ASSERT_TRUE(pipeline.valid() && target.valid());

  auto* cmd = dev->acquireCommandBuffer();
  cmd->beginRenderPass(target, {0, 0, 0, 1});
  cmd->bindPipeline(pipeline);
  cmd->bindUniformBuffer(0, ubo, 0, 16);
  cmd->bindTexture(0, tex, sampler);
  cmd->draw(3, 0);
  cmd->endRenderPass();
  dev->submit(cmd);
  dev->waitIdle();

  rd::test::Image img;
  img.width = kW;
  img.height = kH;
  img.pixels.resize(kW * kH * 4);
  ASSERT_TRUE(dev->readbackTarget(target, img.pixels.data(), img.pixels.size()));
  size_t i = ((kH / 2) * kW + kW / 2) * 4;
  EXPECT_NEAR(img.pixels[i], 40, 4);    // r = 40/255
  EXPECT_NEAR(img.pixels[i + 1], 26, 4);
  EXPECT_TRUE(dev->generateMipmaps(tex));
#endif
}

TEST(CubeTarget, VulkanRenderToFaceAndSample) {
#if defined(RD_WITH_VULKAN)
  rd::DeviceDesc d;
  d.backend = rd::Backend::Vulkan;
  auto dev = rd::createDevice(d);
  ASSERT_NE(dev, nullptr);
  ASSERT_TRUE(dev->caps().supports(rd::Capability::cube_render_target));
  // 同 Metal 用例流程(后端换 Vulkan)
  rd::TextureDesc td;
  td.type = rd::TextureType::Cube;
  td.width = kCube;
  td.height = kCube;
  td.usage = rd::TextureUsage::Sampled | rd::TextureUsage::RenderTargetAttachment;
  auto tex = dev->createTexture(td);
  ASSERT_TRUE(tex.valid());
  renderFaces(*dev, tex);

  auto vs = rd::test::loadShaderCode(rd::Backend::Vulkan, RD_SHADER_DIR, "texcube.vert");
  auto fs = rd::test::loadShaderCode(rd::Backend::Vulkan, RD_SHADER_DIR, "texcube.frag");
  auto vsm = dev->createShaderModule({rd::ShaderStage::Vertex, vs.code, vs.entry});
  auto fsm = dev->createShaderModule({rd::ShaderStage::Fragment, fs.code, fs.entry});
  auto ubo = dev->createBuffer({16, rd::BufferUsage::Uniform, true, false, nullptr});
  const float dir[4] = {1.0f, 0.0f, 0.0f, 0.0f};
  dev->updateBuffer(ubo, dir, sizeof(dir), 0);
  auto sampler = dev->createSampler({});
  rd::PipelineDesc pd;
  pd.vertexShader = vsm;
  pd.fragmentShader = fsm;
  auto pipeline = dev->createPipeline(pd);
  auto target = dev->createOffscreenTarget({kW, kH});
  ASSERT_TRUE(pipeline.valid() && target.valid());

  auto* cmd = dev->acquireCommandBuffer();
  cmd->beginRenderPass(target, {0, 0, 0, 1});
  cmd->bindPipeline(pipeline);
  cmd->bindUniformBuffer(0, ubo, 0, 16);
  cmd->bindTexture(0, tex, sampler);
  cmd->draw(3, 0);
  cmd->endRenderPass();
  dev->submit(cmd);
  dev->waitIdle();

  rd::test::Image img;
  img.width = kW;
  img.height = kH;
  img.pixels.resize(kW * kH * 4);
  ASSERT_TRUE(dev->readbackTarget(target, img.pixels.data(), img.pixels.size()));
  size_t i = ((kH / 2) * kW + kW / 2) * 4;
  EXPECT_NEAR(img.pixels[i], 40, 4);
  EXPECT_TRUE(dev->generateMipmaps(tex));
#endif
}
```

`tests/CMakeLists.txt` 加 `rhi/cube_target_test.cpp`。

- [ ] **Step 2: 跑测试确认编译失败**

Run: `cmake -S . -B build && cmake --build build -j8 2>&1 | grep -m3 error`
Expected: 编译失败（`colorFromTexture`/`generateMipmaps` 不存在）

- [ ] **Step 3: 接口扩展**

`rhi_types.h` 的 `OffscreenTargetDesc` 替换为：

```cpp
/// 离屏渲染目标创建参数。
struct OffscreenTargetDesc {
  uint32_t width = 0;                         ///< 像素宽
  uint32_t height = 0;                        ///< 像素高
  Format colorFormat = Format::RGBA8_UNORM;   ///< 自建颜色附件格式
  bool depth = false;                         ///< 附带深度附件(P1 引入实现)
  /// 非空:不自建颜色附件,改为挂载该纹理的指定子资源(face/mip)。
  /// 需纹理创建时带 TextureUsage::RenderTargetAttachment;
  /// 能力门控 Capability::cube_render_target(cube 纹理)。
  TextureHandle colorFromTexture;
  uint32_t face = 0;                          ///< cube 面 0..5(+X,-X,+Y,-Y,+Z,-Z);2D 传 0
  uint32_t mipLevel = 0;                      ///< 挂载的 mip 级
};
```

`rhi_device.h` Device 加：

```cpp
  /// 更新纹理子资源(2D 时 face 传 0);数据为整层紧凑 RGBA8(或对应格式)。
  virtual void updateTexture(TextureHandle tex, uint32_t mipLevel, uint32_t face,
                             const void* data, uint64_t size) = 0;
  /// 运行时生成全部 mip 链;能力门控,不支持/纹理无效返回 false。
  virtual bool generateMipmaps(TextureHandle tex) = 0;
```

- [ ] **Step 4: Vulkan 实现（per-subresource layout 追踪）**

`TextureRec` 扩展：

```cpp
struct TextureRec {
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;   ///< 全图视图(采样用)
  bool isCube = false;
  uint32_t width = 0, height = 0, mipLevels = 1;
  Format format = Format::RGBA8_UNORM;
  uint32_t faces = 1;
  std::vector<VkImageLayout> subLayouts;  ///< 每子资源 layout(face*mip 展平)
};
```

createTexture:image usage 由 `desc.usage` 推导：

```cpp
  ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
              VK_IMAGE_USAGE_TRANSFER_SRC_BIT;  // TRANSFER_SRC:mip 生成 blit 源
  if (hasFlag(desc.usage, TextureUsage::RenderTargetAttachment))
    ici.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
```
（宽进不影响正确性；`rec` 各字段填充，`subLayouts.assign(faces * mipLevels, VK_IMAGE_LAYOUT_UNDEFINED)`；上传路径完成后全部置 `SHADER_READ_ONLY_OPTIMAL`——即在上传成功分支末尾统一赋值。)

createOffscreenTarget 支持 `colorFromTexture`：在函数开头分支：

```cpp
  if (desc.colorFromTexture.valid()) {
    auto tit = textures_.find(desc.colorFromTexture);
    if (tit == textures_.end()) return {};
    const TextureRec& tr = tit->second;
    if (!caps_.supports(Capability::cube_render_target) && tr.isCube) return {};
    TargetRec rec{};
    rec.width = desc.width;
    rec.height = desc.height;
    rec.textureBacked = true;
    rec.srcTexture = desc.colorFromTexture;
    rec.srcFace = desc.face;
    rec.srcMip = desc.mipLevel;
    // 视图:2D 类型视图指向指定 face/mip
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = tr.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = toVkFormat(tr.format);
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, desc.mipLevel, 1, desc.face, 1};
    if (vkCreateImageView(device_, &vci, nullptr, &rec.view) != VK_SUCCESS) return {};
    VkFramebufferCreateInfo fbci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fbci.renderPass = renderPass_;
    fbci.attachmentCount = 1;
    fbci.pAttachments = &rec.view;
    fbci.width = desc.width;
    fbci.height = desc.height;
    fbci.layers = 1;
    if (vkCreateFramebuffer(device_, &fbci, nullptr, &rec.fb) != VK_SUCCESS) return {};
    TargetHandle h(nextId_++);
    targets_.emplace(h, rec);
    return h;
  }
  // …原自建附件路径不变…
```

`TargetRec` 加 `bool textureBacked = false; TextureHandle srcTexture; uint32_t srcFace = 0, srcMip = 0;`。`destroyTarget` 对 textureBacked 只销毁 fb/view（不销毁纹理）。`readbackTarget` 对 textureBacked 记警告返回 false。

`endRenderPass` 开头分支：textureBacked → 不做 staging 拷贝，改为把该子资源 TRANSFER_SRC→SHADER_READ:

```cpp
  TargetRec t;
  if (!device_->target(currentTarget_, t)) return;
  if (t.textureBacked) {
    device_->transitionTextureSubresource(t.srcTexture, t.srcFace, t.srcMip,
                                          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return;
  }
  // …原 staging 拷贝路径…
```

`VulkanDevice` 新增（用单 cmd_ 立即提交，P0 串行风格）:

```cpp
/// 子资源 layout 转换(立即记录并提交;更新 subLayouts 追踪)。
void VulkanDevice::transitionTextureSubresource(TextureHandle tex, uint32_t face, uint32_t mip,
                                                VkImageLayout from, VkImageLayout to) {
  auto it = textures_.find(tex);
  if (it == textures_.end()) return;
  TextureRec& tr = it->second;
  vkResetCommandBuffer(cmd_, 0);
  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(cmd_, &bi);
  VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  barrier.oldLayout = from;
  barrier.newLayout = to;
  barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  barrier.image = tr.image;
  barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, face, 1};
  vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                       &barrier);
  vkEndCommandBuffer(cmd_);
  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cmd_;
  vkQueueSubmit(queue_, 1, &si, VK_NULL_HANDLE);
  vkQueueWaitIdle(queue_);
  tr.subLayouts[face * tr.mipLevels + mip] = to;
}
```

注意：渲染进 texture-backed target 时，render pass 的 initialLayout=UNDEFINED 会丢弃内容——每次渲染该子资源前应已被视为"内容无效"。`beginRenderPass` 路径无需改（fb 正常绑）。如果子资源当前是 SHADER_READ，从 SHADER_READ 进入 initialLayout=UNDEFINED 的 pass 是合法的（内容丢弃即正确语义）。

`updateTexture`:

```cpp
void VulkanDevice::updateTexture(TextureHandle tex, uint32_t mipLevel, uint32_t face,
                                 const void* data, uint64_t size) {
  auto it = textures_.find(tex);
  if (it == textures_.end() || !data) return;
  TextureRec& tr = it->second;
  if (mipLevel >= tr.mipLevels || face >= tr.faces) return;
  uint32_t w = tr.width >> mipLevel, hgt = tr.height >> mipLevel;
  if (w == 0) w = 1;
  if (hgt == 0) hgt = 1;
  const uint64_t need = uint64_t(w) * hgt * formatSize(tr.format);
  if (size < need) { RD_LOGE("rhi.vk", "updateTexture: 数据不足"); return; }
  // staging 缓冲
  VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bci.size = need;
  bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  VkBuffer staging; VkDeviceMemory stagingMem;
  if (vkCreateBuffer(device_, &bci, nullptr, &staging) != VK_SUCCESS) return;
  VkMemoryRequirements sreq;
  vkGetBufferMemoryRequirements(device_, staging, &sreq);
  VkMemoryAllocateInfo smai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  smai.allocationSize = sreq.size;
  smai.memoryTypeIndex = findMemoryType(sreq.memoryTypeBits,
                                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  bool ok = smai.memoryTypeIndex != UINT32_MAX &&
            vkAllocateMemory(device_, &smai, nullptr, &stagingMem) == VK_SUCCESS &&
            vkBindBufferMemory(device_, staging, stagingMem, 0) == VK_SUCCESS;
  if (ok) {
    void* mapped;
    vkMapMemory(device_, stagingMem, 0, need, 0, &mapped);
    memcpy(mapped, data, need);
    vkUnmapMemory(device_, stagingMem);
    const VkImageLayout cur = tr.subLayouts[face * tr.mipLevels + mipLevel];
    vkResetCommandBuffer(cmd_, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd_, &bi);
    VkImageMemoryBarrier toDst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toDst.oldLayout = cur;
    toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toDst.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toDst.image = tr.image;
    toDst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, mipLevel, 1, face, 1};
    vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toDst);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mipLevel, face, 1};
    copy.imageExtent = {w, hgt, 1};
    vkCmdCopyBufferToImage(cmd_, staging, tr.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    VkImageMemoryBarrier toRead{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    toRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    toRead.image = tr.image;
    toRead.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, mipLevel, 1, face, 1};
    vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &toRead);
    vkEndCommandBuffer(cmd_);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd_;
    vkQueueSubmit(queue_, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue_);
    tr.subLayouts[face * tr.mipLevels + mipLevel] = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  }
  vkDestroyBuffer(device_, staging, nullptr);
  if (ok) vkFreeMemory(device_, stagingMem, nullptr);
}
```

`generateMipmaps`（逐 face 逐级 blit):

```cpp
bool VulkanDevice::generateMipmaps(TextureHandle tex) {
  auto it = textures_.find(tex);
  if (it == textures_.end() || it->second.mipLevels < 2) return false;
  TextureRec& tr = it->second;
  vkResetCommandBuffer(cmd_, 0);
  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(cmd_, &bi);
  for (uint32_t face = 0; face < tr.faces; ++face) {
    for (uint32_t mip = 1; mip < tr.mipLevels; ++mip) {
      // src mip-1 → TRANSFER_SRC;dst mip → TRANSFER_DST;blit;src → SHADER_READ
      VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
      b.image = tr.image;
      b.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
      b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
      b.oldLayout = tr.subLayouts[face * tr.mipLevels + mip - 1];
      b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
      b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, mip - 1, 1, face, 1};
      vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
      b.oldLayout = tr.subLayouts[face * tr.mipLevels + mip];
      b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      b.srcAccessMask = 0;
      b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, face, 1};
      vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
      VkImageBlit blit{};
      blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip - 1, face, 1};
      blit.srcOffsets[1] = {int32_t(tr.width >> (mip - 1) ? tr.width >> (mip - 1) : 1),
                            int32_t(tr.height >> (mip - 1) ? tr.height >> (mip - 1) : 1), 1};
      blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, face, 1};
      blit.dstOffsets[1] = {int32_t(tr.width >> mip ? tr.width >> mip : 1),
                            int32_t(tr.height >> mip ? tr.height >> mip : 1), 1};
      vkCmdBlitImage(cmd_, tr.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, tr.image,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
      // 两级都回 SHADER_READ
      b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
      b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
      b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
      b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, mip - 1, 1, face, 1};
      vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
      tr.subLayouts[face * tr.mipLevels + mip - 1] = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, face, 1};
      vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
      tr.subLayouts[face * tr.mipLevels + mip] = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
  }
  vkEndCommandBuffer(cmd_);
  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cmd_;
  vkQueueSubmit(queue_, 1, &si, VK_NULL_HANDLE);
  vkQueueWaitIdle(queue_);
  return true;
}
```

`bindTexture` 内 `VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL` 的写死值与追踪一致（全部子资源渲染/上传后都回到该值），无需改。

- [ ] **Step 5: Metal 实现**

`TextureRec` 加 `uint32_t width = 0, height = 0, mipLevels = 1; Format format; bool isCube = false;`(createTexture 填充）。`createTexture` 的 `td.usage` 由 desc.usage 推导：

```cpp
    td.usage = MTLTextureUsageShaderRead;
    if (hasFlag(desc.usage, TextureUsage::RenderTargetAttachment))
      td.usage |= MTLTextureUsageRenderTarget;
```

`createOffscreenTarget` 开头分支（texture-backed):

```cpp
  if (desc.colorFromTexture.valid()) {
    auto it = textures_.find(desc.colorFromTexture);
    if (it == textures_.end()) return {};
    TargetHandle h(nextId_++);
    TargetRec rec;
    rec.color = it->second.texture;       // 共享底层纹理
    rec.width = desc.width;
    rec.height = desc.height;
    rec.textureBacked = true;
    rec.face = desc.face;
    rec.mip = desc.mipLevel;
    targets_.emplace(h, rec);
    return h;
  }
```

`TargetRec` 加 `bool textureBacked = false; uint32_t face = 0, mip = 0;`。`MetalCommandBuffer::beginRenderPass` 在 textureBacked 时：

```cpp
  rp.colorAttachments[0].texture = t.color;
  if (t.textureBacked) {
    rp.colorAttachments[0].slice = t.face;    // cube 面(2D 传 0)
    rp.colorAttachments[0].level = t.mip;     // mip 级
  }
```

`readbackTarget` 对 textureBacked:Shared 存储可直接 `getBytes`（指定 slice/mipmapLevel):

```cpp
    if (t.textureBacked) {
      if (t.color.storageMode == MTLStorageModePrivate) return false;
      [t.color getBytes:outRGBA8 bytesPerRow:t.width * 4
             fromRegion:MTLRegionMake2D(0, 0, t.width, t.height)
            mipmapLevel:t.mip
                  slice:t.face];
      return true;
    }
```

`updateTexture`:

```cpp
  void updateTexture(TextureHandle tex, uint32_t mipLevel, uint32_t face, const void* data,
                     uint64_t size) override {
    auto it = textures_.find(tex);
    if (it == textures_.end() || !data) return;
    const TextureRec& tr = it->second;
    uint32_t w = tr.width >> mipLevel, h = tr.height >> mipLevel;
    if (w == 0) w = 1;
    if (h == 0) h = 1;
    const uint64_t need = uint64_t(w) * h * formatSize(tr.format);
    if (size < need) { RD_LOGE("rhi.metal", "updateTexture: 数据不足"); return; }
    [tr.texture replaceRegion:MTLRegionMake2D(0, 0, w, h)
                  mipmapLevel:mipLevel
                        slice:face
                    withBytes:data
                  bytesPerRow:w * formatSize(tr.format)
                bytesPerImage:tr.isCube ? need : 0];
  }

  bool generateMipmaps(TextureHandle tex) override {
    auto it = textures_.find(tex);
    if (it == textures_.end() || it->second.mipLevels < 2) return false;
    id<MTLCommandBuffer> cb = [queue_ commandBuffer];
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    [blit generateMipmapsForTexture:it->second.texture];
    [blit endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    return true;
  }
```

- [ ] **Step 6: GLES 实现**

`createOffscreenTarget` texture-backed 分支：

```cpp
  if (desc.colorFromTexture.valid()) {
    auto it = textures_.find(desc.colorFromTexture);
    if (it == textures_.end()) return {};
    ensureOffscreenCurrent();
    TargetRec rec;
    rec.width = desc.width;
    rec.height = desc.height;
    glGenFramebuffers(1, &rec.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, rec.fbo);
    GLenum attachmentTarget = it->second.target == GL_TEXTURE_CUBE_MAP
                                  ? GL_TEXTURE_CUBE_MAP_POSITIVE_X + desc.face : GL_TEXTURE_2D;
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, attachmentTarget,
                           it->second.tex, GLint(desc.mipLevel));
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
      glDeleteFramebuffers(1, &rec.fbo);
      return {};
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    TargetHandle h(nextId_++);
    targets_.emplace(h, rec);
    return h;
  }
```

`updateTexture`:`glTexSubImage2D(faceTarget, mip, 0, 0, w, h, GL_RGBA, ..., data)`（格式映射同 createTexture);`generateMipmaps`:`glBindTexture` + `glGenerateMipmap(target)`，成功返回 true。

- [ ] **Step 7: 跑全部测试**

Run: `./scripts/check.sh 2>&1 | tail -5`
Expected: `100% tests passed`（含 `CubeTarget.*`;golden 无变化）

- [ ] **Step 8: Commit**

```bash
git add core/rhi shaders/ tests/
git commit -m "feat(rhi): cube face/mip 渲染目标 + updateTexture + generateMipmaps"
```

---

### Task 8: GLES CommandBuffer 延迟回放

**Files:**
- Modify: `core/rhi/backends/gles/gles_device.cpp`

背景：GL 是立即执行模型，无命令缓冲；把录制改为命令对象列表，submit 时统一回放——与 Vulkan/Metal 的"录制/提交"两阶段语义对齐，错误检查收敛到 submit 边界，为阶段二 pass 排序/去重打底。

- [ ] **Step 1: 重构 GLESCommandBuffer 为闭包列表**

`GLESCommandBuffer` 加成员 `std::vector<std::function<void()>> cmds_;`(`#include <functional>`)。各命令改为入队闭包（按值捕获所需状态）:

```cpp
void GLESCommandBuffer::beginRenderPass(TargetHandle target, const ClearColor& clear) {
  TargetRec t;
  if (!device_->target(target, t)) return;
  current_ = t;  // 仍记录,供录制期状态查询
  cmds_.emplace_back([this, t, clear] {
    if (t.isSwapchain) device_->makeCurrent(t.surface);
    else device_->ensureOffscreenCurrent();
    glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
    glViewport(0, 0, GLsizei(t.width), GLsizei(t.height));
    glClearColor(clear.r, clear.g, clear.b, clear.a);
    glClear(GL_COLOR_BUFFER_BIT);
  });
}
```

`bindPipeline`/`bindUniformBuffer`/`bindTexture`/`draw*` 同理改为闭包入队；`draw` 闭包捕获当时的绑定快照（vertexBuffers_/offsets/indexBuffer_/pipeline_ 按值拷贝进 lambda 捕获）。`applyVertexState` 改为接受快照参数的静态式辅助。回放：

```cpp
void GLESDevice::submit(CommandBuffer*) {
  for (auto& c : cmdBuf_.cmds_) c();
  cmdBuf_.cmds_.clear();
  // 错误检查收敛到 submit 边界(debug 构建)
#ifndef NDEBUG
  GLenum err = glGetError();
  if (err != GL_NO_ERROR) RD_LOGE("rhi.gles", "GL 错误 0x%x @submit", err);
#endif
}
```

注意：`acquireCommandBuffer` 时清空 `cmds_`（防未 submit 的残留）;`makeCurrent`/surface 切换全部移到回放期（渲染线程）。

- [ ] **Step 2: Android 构建 + 模拟器验证**

Run: `source /tmp/rd_env.sh && cd samples/android && ./gradlew :app:assembleDebug 2>&1 | tail -3`
Expected: `BUILD SUCCESSFUL`；模拟器 demo 立方体渲染正常（`img_check` 通过）

- [ ] **Step 3: host 回归 + Commit**

Run: `./scripts/check.sh 2>&1 | tail -3`
Expected: `100% tests passed`

```bash
git add core/rhi/backends/gles/
git commit -m "refactor(rhi): GLES CommandBuffer 延迟回放(录制/提交两阶段语义对齐)"
```

---

### Task 9: 收尾——文档同步 + 全平台验证

**Files:**
- Modify: `AGENTS.md`
- Modify: `docs/superpowers/specs/2026-08-13-rhi-foundation-hardening-design.md`（提交清单补 GLES 纹理项）

- [ ] **Step 1: 更新 AGENTS.md「代码约定」追加**

```markdown
- 帧括号:渲染循环每帧 beginFrame()/endFrame()(present 之后);destroy 的底层资源
  延迟到帧完成后回收(退休队列),句柄 destroy 后立即失效
- 能力查询:只经 `device.caps()`(rhi_capability.h),不直接查后端扩展
- BufferDesc:`{size, usage, hostWrite, hostRead, data}`;非 hostWrite 缓冲为
  device-local,updateBuffer 会被拒绝(动态数据须 hostWrite=true)
- 纹理可作为渲染目标:TextureUsage::RenderTargetAttachment +
  OffscreenTargetDesc.colorFromTexture(face/mip);GLES sampler uniform 命名 texN ↔ slot N
- 每帧至多一次 submit(单缓冲串行模型)
```

- [ ] **Step 2: spec §6 提交清单更新**（在条目 5 后插入 `feat(rhi): GLES 纹理/采样器实现`，原条目顺移）

- [ ] **Step 3: 全平台验证**

Run: `./scripts/check.sh 2>&1 | tail -3` → `100% tests passed`
Run: `source /tmp/rd_env.sh && cd samples/android && ./gradlew :app:assembleDebug 2>&1 | tail -3` → `BUILD SUCCESSFUL`
Run: `cmake -S . -B build-ios -G Xcode -DCMAKE_TOOLCHAIN_FILE=cmake/ios.toolchain.cmake -DRD_IOS_SDK=iphonesimulator && cmake --build build-ios --config Debug 2>&1 | tail -3` → `BUILD SUCCEEDED`
双端模拟器 demo 截图 + `img_check` 通过。

- [ ] **Step 4: Commit**

```bash
git add AGENTS.md docs/superpowers/specs/2026-08-13-rhi-foundation-hardening-design.md
git commit -m "docs: RHI 底座强化收尾(AGENTS.md 新约定 + spec 提交清单同步)"
```

---

## Self-Review 记录

- **Spec 覆盖**:spec §2.1 能力表→Task 1;§2.2 内存 flag→Task 2;§2.3 退休→Task 3;§2.4 管线扩展+缓存→Task 4;§2.5 GLES 回放→Task 8;§2.6 厚默认实现→并入 Task 2(staging 路径抽象为后端私有辅助，未单独立项——三后端公共骨架不足，遵循"不为抽象而抽象");§2.7 instancing/cube 目标/updateTexture/generateMipmaps→Task 5/7;§5 测试→各 Task 内嵌契约测试 + Task 9 全平台验证。**spec 缺口补记**:GLES 纹理桩 → Task 6。
- **有意偏离说明**: Vulkan 纹理 layout 采用 per-subresource 追踪（比 Taichi 的"lambda 注入计算流"简单，因为无 compute 语义）;GLES 退休依赖 GL 删除语义而非真队列（已注释）。
- **已知遗留（不在本阶段）**:深度附件实现（P1)、MSAA(P2)、异步帧重叠（去 waitIdle 化 + acquire/present 信号量链，性能专项）。

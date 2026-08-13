# 第一阶段:RHI 底座强化设计文档

- **日期**: 2026-08-13
- **状态**: 待评审
- **前置文档**: `docs/taichi-desigin.md`(Taichi 设计解析)、`docs/superpowers/specs/2026-08-09-mobile-3d-renderer-design.md`(总体设计)
- **目标平台**: iOS / Android(鸿蒙维持预埋策略,本阶段不写鸿蒙专属代码)

## 1. 背景与定位

总体路线图(全栈分阶段优化,参考 Taichi GGUI/RHI 设计):

| 阶段 | 内容 | 与特性里程碑的关系 |
|---|---|---|
| **阶段一:RHI 底座强化(本文档)** | 能力表、内存模型、N 帧资源退休、管线缓存、GLES 延迟回放、厚默认实现;补齐 P1 刚需接口 | P1 开工前完成,三后端 golden image 全绿为验收 |
| 阶段二:渲染框架层 | 借鉴 GGUI 五元组/帧流程设计 renderer/scene/resource 层 | 设计先行,与 P1(glTF+PBR/IBL)合并实施 |
| 阶段三:平台层与鸿蒙预埋验证 | C API 增强、三线程模型落地、鸿蒙 toolchain 编译验证 | 与 P2/P3 对齐 |

### 已确认决策

| 决策点 | 结论 |
|---|---|
| 重构尺度 | **允许破坏性重构**(P1 未开工,是最便宜的重构窗口) |
| 方案 | **选择性吸收 + 保留现有约定**:保留 `Handle<Tag>` 强类型句柄与统一绑定约定,吸收 Taichi RHI 精华 |
| 鸿蒙 | 维持预埋扩展点策略,本阶段零鸿蒙专属代码 |
| GLES 底线 | GLES 3.0 + 能力降级:接口层不暴露 compute/SSBO 语义 |

### 非目标(YAGNI,有意不吸收的 Taichi 设计)

- **不引入 Stream/Semaphore 公共抽象**:GLES 3.0 无 compute、移动端单队列,同步面越简单越稳;swapchain acquire/present 同步收敛在后端内部。
- **不用类型擦除句柄**(Taichi `DeviceAllocation`):`Handle<Tag>` 编译期强类型正好根治 Taichi 自承的 image/buffer 同柄之病。
- **不暴露 compute/dispatch**:GLES 3.0 底线不支持;GPU 预处理类需求在阶段二以 prepass 钩子形式预留,GLES 上退化为 CPU/变换反馈路径。
- **不做 AOT/互操作/external memory**:无此产品线需求。

## 2. 接口面新骨架

```
core/rhi/
  rhi_types.h          # 句柄/枚举/Desc(扩展)
  rhi_device.h         # Device + CommandBuffer(重构)
  rhi_capability.h     # DeviceCaps 查询(新)
  rhi_constants.inc.h  # 能力枚举 X-macro 单源(新)
  backends/{vulkan,metal,gles}/
```

### 2.1 能力表(X-macro 单源)

`rhi_constants.inc.h`:

```cpp
#ifndef RD_CAPABILITY
#define RD_CAPABILITY(name)
#endif
// 值语义:0 = 不支持;非 0 为级别(如 msaa=4 表示最大 4x)
RD_CAPABILITY(max_texture_size)         // 像素上限
RD_CAPABILITY(max_texture_slots)        // 单阶段纹理槽数
RD_CAPABILITY(max_uniform_buffer_slots) // uniform 槽数
RD_CAPABILITY(instancing)               // 实例化绘制
RD_CAPABILITY(msaa)                     // 最大 sample count
RD_CAPABILITY(depth_texture)            // 可采样深度附件(P2 阴影)
RD_CAPABILITY(cube_render_target)       // 渲染到 cube 指定 face/mip(IBL 预滤波)
RD_CAPABILITY(generate_mipmap)          // 运行时 mip 生成
RD_CAPABILITY(anisotropy)               // 最大各向异性等级
```

`rhi_capability.h`:X-macro 三处展开(枚举、名表、数组大小),`DeviceCaps` 用 `std::array<uint32_t, kCount>` 存储——比 Taichi 的 `std::map` 更紧凑,O(1) 无分配:

```cpp
enum class Capability : uint32_t {
#define RD_CAPABILITY(name) name,
#include "rhi/rhi_constants.inc.h"
  kCount
};

class DeviceCaps {
public:
  uint32_t get(Capability c) const;        // 缺省 0 = 不支持
  bool supports(Capability c) const;       // get(c) != 0
  const char* name(Capability c) const;    // 日志/诊断用
  // 后端内部 set(Capability, uint32_t)
};
```

三后端在设备创建时上报:Vulkan 查 `VkPhysicalDeviceFeatures/Limits`,Metal 按 GPU family 判定,GLES 查扩展位与 `glGetIntegerv`。上层只查表,不直接碰扩展——能力探测集中收敛在 rhi 层(总体设计 §8 既定原则)。

### 2.2 内存模型 flag 化

```cpp
struct BufferDesc {
  uint64_t size = 0;
  BufferUsage usage = BufferUsage::Vertex;
  bool hostWrite = false;        // CPU 频繁写(动态 uniform/顶点)
  bool hostRead = false;         // CPU 回读(截图/readback staging)
  const void* data = nullptr;    // 非空则创建时随带上传
};
```

后端映射:`hostWrite/hostRead` 全 false → Vulkan device-local(+内部 staging 上传)/Metal Private;任一 true → Vulkan host-visible/Metal Shared;GLES 映射为 STATIC/DYNAMIC hint(GL 无真 host-visible 概念,map 模拟)。

`TextureDesc` 增加 `TextureUsage` flags(`Sampled | RenderTargetAttachment`);`SamplerDesc` 增加可选各向异性等级(能力门控)。

### 2.3 N 帧资源退休(核心修复)

**问题**:现有 `destroyXxx` 立即释放底层资源;Vulkan/Metal 上帧在飞(maxFramesInFlight≥1)时,GPU 可能仍在读该资源——Taichi 就靠底层分配器延迟释放侥幸工作(其文档自承),我们必须根治。

**设计**:
- `Device::beginFrame()/endFrame()` 帧括号,渲染循环每帧调用;Device 内部维护单调帧序号与退休队列 `vector<pair<frameIndex, 释放闭包>>`。
- `destroyXxx` 语义变化:句柄立即失效(调用方不得再用),底层资源封装为释放闭包推入退休队列,打上当前帧序号。
- 帧完成判定:Vulkan = per-frame fence;Metal = commandBuffer `completedHandler`;GLES = 单线程顺序执行,延迟 N 帧即安全。
- `endFrame()` 轮询退休队列,释放所有已完成帧的资源;`maxFramesInFlight = 2`(可配)。
- 对调用方完全透明;`waitIdle()` 语义不变(测试 readback 前用),并附加清空退休队列。

### 2.4 管线描述扩展与缓存

```cpp
struct BlendDesc {
  bool enable = false;
  BlendFactor srcColor = BlendFactor::SrcAlpha;        // 经典 alpha 混合默认
  BlendFactor dstColor = BlendFactor::OneMinusSrcAlpha;
  BlendFactor srcAlpha = BlendFactor::One;
  BlendFactor dstAlpha = BlendFactor::OneMinusSrcAlpha;
};

struct PipelineDesc {
  // ...现有字段保留...
  bool depthTest = false;
  bool depthWrite = false;      // 从 depthTest 拆出(Taichi 同款两 bool 最小面)
  BlendDesc blend;              // PBR alpha 混合需要
  uint32_t sampleCount = 1;     // 预留:MSAA(能力门控,与目标附件一致才有效)
};
```

**管线缓存**:以 `{vertexShader, fragmentShader, 顶点布局, topology, cull, depth*, blend, colorFormat, sampleCount}` 做 struct hash key,后端内部缓存底层 PSO 对象(Vulkan `VkPipeline` 模板按 renderpass 格式懒实例化;Metal `MTLRenderPipelineState`)。**句柄语义不变**:每次 `createPipeline` 返回新薄包装句柄,`destroyPipeline` 只释放包装,底层 PSO 由缓存持有——destroy 语义不被缓存污染。预留 `VkPipelineCache` 序列化接口(P4 启动优化用)。

### 2.5 GLES Cmd 延迟回放

GLES CommandBuffer 改为录制期构建命令对象列表(`vector<Cmd>`,小型多态对象),`submit` 时统一回放执行 GL 调用:

- 与 Vulkan/Metal"录制/提交"两阶段语义对齐;
- 错误检查收敛到 submit 边界(录制期零 `glGetError`);
- 为阶段二 render graph 的 pass 排序/去重打底。

纯后端内部实现变化,公共接口不变。

### 2.6 厚默认实现(适度)

- 基类非虚便捷组合:`createBuffer` 带 `data` 的"创建+staging 上传"路径抽象为可覆写默认实现;
- `readbackTarget` 各后端差异大,保持虚函数;
- 不为抽象而抽象——仅当三后端有明显公共骨架时才下沉。

### 2.7 P1 刚需接口补齐

**CommandBuffer 新增**:

```cpp
virtual void drawInstanced(uint32_t vertexCount, uint32_t firstVertex,
                           uint32_t instanceCount, uint32_t firstInstance) = 0;
virtual void drawIndexedInstanced(uint32_t indexCount, uint32_t firstIndex,
                                  int32_t vertexOffset, uint32_t instanceCount,
                                  uint32_t firstInstance) = 0;
```

**Device 新增**:

```cpp
virtual const DeviceCaps& caps() const = 0;
virtual void beginFrame() = 0;
virtual void endFrame() = 0;
// 纹理子资源更新(KTX2 逐 mip/face 上传);2D 纹理 face 传 0
virtual void updateTexture(TextureHandle tex, uint32_t mipLevel, uint32_t face,
                           const void* data, uint64_t size) = 0;
// 运行时 mip 生成;能力门控,不支持返回 false
virtual bool generateMipmaps(TextureHandle tex) = 0;
```

**OffscreenTargetDesc 扩展**(IBL 预滤波渲染到 cubemap face/mip):

```cpp
struct OffscreenTargetDesc {
  uint32_t width = 0, height = 0;
  Format colorFormat = Format::RGBA8_UNORM;
  bool depth = false;
  bool depthSampled = false;          // 深度附件可作纹理采样(P2 阴影预埋)
  TextureHandle colorFromTexture;     // 非空:挂载到已有纹理子资源(替代自建附件)
  uint32_t face = 0;                  // cube 面 0..5
  uint32_t mipLevel = 0;              // mip 级
  uint32_t sampleCount = 1;           // 预留:MSAA(能力门控)
};
```

`colorFromTexture` 非空时,target 的 colorFormat 取纹理自身格式;`cube_render_target` 能力门控。

## 3. 帧流程与数据流

### 3.1 目标帧流程(渲染线程)

```
device.beginFrame()                        // 退休队列推进
target = device.acquireSwapChainTarget(sc) // acquire 同步在后端内部完成
cmd = device.acquireCommandBuffer()
cmd->beginRenderPass(target, clear)
  ... bindPipeline/bindVertexBuffer/bindUniformBuffer/bindTexture ...
  ... drawIndexedInstanced ...
cmd->endRenderPass()
device.submit(cmd)
device.present(sc)                         // present 同步在后端内部完成
device.endFrame()                          // 到期资源真正释放
```

### 3.2 资产上传路径(为 P1 打通)

```
静态几何:  createBuffer({size, usage=Vertex|Index, data})        → device-local + 内部 staging
动态数据:  createBuffer({hostWrite=true}) + 每帧 updateBuffer    → host-visible
KTX2 纹理: createTexture(逐 mip 布局) 或 createTexture + updateTexture(face, mip)
IBL 预滤波: createTexture(Cube) → createOffscreenTarget(colorFromTexture, face, mip)
           → 逐 face/mip 渲染(GGX 预滤波,阶段二实现算法)
```

## 4. 错误处理

沿用 AGENTS.md 既定约定,不变:

- 内核不用异常;创建失败返回无效句柄 + `RD_LOG*` 日志;
- 能力门控调用在不支持时记警告并返回 false/跳过(如 `generateMipmaps`);
- FFI 边界错误码 + `rd_get_last_error()` 不变;
- debug 构建:能力违规使用(如超 slot 绑定)触发断言 + 日志;release 优雅降级。

## 5. 测试策略

| 层级 | 内容 | 通过标准 |
|---|---|---|
| 回归 | 现有 Cube golden image 三后端 | `./scripts/check.sh` 全绿 |
| RHI 契约测试(新增) | instancing 绘制;blend 开/关;渲染到 cube face 并读回逐面校验;`generateMipmaps`(不支持的后端断言返回 false);`updateTexture` 子资源更新;retire 压力(循环 1 万帧反复 create/destroy,无泄露无崩溃);caps 快照比对 | CI + 本地三后端一致 |
| 双端模拟器 | Android/iOS demo 截图 | `img_check` coverage 达标 |

契约测试写法:同一用例跑三后端,能力不支持的项自动 `GTEST_SKIP()` 并记录。

## 6. 迁移波及面与提交节奏

波及文件:`core/scene/cube_scene.cpp`、`tools/render_test`、`core/api/rd_api.cpp`、`platform/{ios,android}` 渲染循环、`tests/`——规模小,一次重构完成。

按 AGENTS.md 规范小步提交(每任务一个 commit):

1. `feat(rhi): 能力表系统(X-macro 单源 + DeviceCaps + 三后端上报)`
2. `feat(rhi): 内存模型 flag 化(BufferDesc host_write/read + TextureUsage)`
3. `feat(rhi): beginFrame/endFrame 帧括号 + N 帧资源退休队列`
4. `feat(rhi): PipelineDesc 扩展(blend/depthWrite 拆分/sampleCount 预留)+ 管线缓存`
5. `feat(rhi): instancing draw 三后端实现`
6. `feat(rhi): GLES 纹理/采样器实现`(计划阶段发现的缺口:P0 桩,P1 PBR 需要三后端对齐)
7. `feat(rhi): cubemap face/mip 渲染目标 + updateTexture + generateMipmaps`
8. `refactor(rhi): GLES CommandBuffer 延迟回放`
9. `test(rhi): 新能力契约测试 + caps 快照`

## 7. 验收标准

- `./scripts/check.sh` 全绿(含新增契约测试);
- 三后端 Cube golden image 一致(回归);
- 双端模拟器 demo 截图通过;
- retire 压力测试 1 万帧无泄露无崩溃(ASAN 配置下);
- AGENTS.md 同步更新(新约定:帧括号、能力表查询方式、Desc 新字段)。

## 8. 对阶段二的预埋

- prepass 类需求:CommandBuffer 已是"renderpass 外可录制任意命令"形态,阶段二 renderer 层加 Renderable 钩子即可,接口零改动;
- `depthSampled` 为 P2 阴影预埋;`sampleCount` 为 P2 MSAA 后处理链预埋;
- GLES 延迟回放为 render graph pass 排序打底;
- 鸿蒙:`createSwapChain(void* nativeWindow, ...)` 已是"外部注入原生窗口句柄"形态,鸿蒙 XComponent 直接适配;能力表天然支持新后端上报。

# Pipeline 缓存 v2 实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 按 `docs/superpowers/specs/2026-08-19-pipeline-cache-design.md` 实现 Vulkan VkPipelineCache + Metal MTLBinaryArchive 落盘。

**关键事实(代码侦察)**:
- Vulkan 已有**内存** `pipelineCache_`(CPU 端 map)——新驱动级成员命名 `driverPipelineCache_` 避免混淆;
  `vkCreateGraphicsPipelines` 在 vulkan_device.cpp:1111,当前传 VK_NULL_HANDLE;
  析构 vulkan_device.cpp:769(`vkDestroyDevice` 前落盘)。
- Metal createPipeline 在 metal_device.mm:412(`newRenderPipelineStateWithDescriptor`)。
- RHI Device 基类 rhi_device.h。

**通用约定:**
- 构建+测试:`export RD_DEPS_MIRROR=$HOME/.rd_deps_mirror && ./scripts/check.sh`
- 提交规范:每 Task 一个 commit

---

### Task 1: RHI 接口 + Vulkan 实现

**Files:**
- Modify: `core/rhi/rhi_device.h`、`core/rhi/backends/vulkan/vulkan_device.cpp`

- [ ] **Step 1: RHI 接口**

`rhi_device.h` Device 加(非纯虚,默认空):

```cpp
  /// 设置驱动级管线缓存文件路径;空串关闭(默认关)。
  /// Vulkan:VkPipelineCache 预载/落盘;Metal:MTLBinaryArchive(macOS 11+/iOS 14+);
  /// GLES:no-op。
  virtual void setPipelineCachePath(const char* path) { (void)path; }
```

- [ ] **Step 2: Vulkan 实现**

`vulkan_device.cpp`:
- 成员:`VkPipelineCache driverPipelineCache_ = VK_NULL_HANDLE; std::string pipelineCachePath_;`
- `setPipelineCachePath` 实现(override):

```cpp
void setPipelineCachePath(const char* path) override {
  // 已有缓存:先落盘销毁
  savePipelineCache();
  pipelineCachePath_ = path ? path : "";
  if (pipelineCachePath_.empty()) return;
  // 预载(存在则)
  std::vector<uint8_t> blob;
  if (FILE* f = fopen(pipelineCachePath_.c_str(), "rb")) {
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n > 0) { blob.resize(size_t(n)); fread(blob.data(), 1, size_t(n), f); }
    fclose(f);
  }
  VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
  ci.initialDataSize = blob.size();
  ci.pInitialData = blob.empty() ? nullptr : blob.data();
  if (vkCreatePipelineCache(device_, &ci, nullptr, &driverPipelineCache_) == VK_SUCCESS)
    RD_LOGI("rhi.vk", "管线缓存装载 %s (%zu B)", pipelineCachePath_.c_str(), blob.size());
}
```

- `savePipelineCache()`(私有成员):

```cpp
void savePipelineCache() {
  if (driverPipelineCache_ == VK_NULL_HANDLE || pipelineCachePath_.empty()) return;
  size_t n = 0;
  vkGetPipelineCacheData(device_, driverPipelineCache_, &n, nullptr);
  if (n == 0) return;
  std::vector<uint8_t> blob(n);
  vkGetPipelineCacheData(device_, driverPipelineCache_, &n, blob.data());
  const std::string tmp = pipelineCachePath_ + ".tmp";
  // 目录自动创建(一层)
  ...
  if (FILE* f = fopen(tmp.c_str(), "wb")) {
    fwrite(blob.data(), 1, n, f);
    fclose(f);
    std::filesystem::rename(tmp, pipelineCachePath_);  // 原子
  }
}
```

- `vkCreateGraphicsPipelines(device_, driverPipelineCache_, 1, ...)`(替换 VK_NULL_HANDLE;
  未设路径时 driverPipelineCache_=VK_NULL_HANDLE 行为同现状)。
- 析构:`savePipelineCache()` → `vkDestroyPipelineCache`(非空)→ 现有销毁序列之前。
- 头文件 `#include <filesystem>`;目录创建用 std::filesystem::create_directories(父目录)。

- [ ] **Step 3: 跑测试**

Run: `./scripts/check.sh 2>&1 | tail -4`(全绿——默认关零回归)

- [ ] **Step 4: Commit**

`git commit -m "feat(rhi.vk): VkPipelineCache 落盘(setPipelineCachePath + 析构保存)"`

---

### Task 2: Metal 实现

**Files:**
- Modify: `core/rhi/backends/metal/metal_device.mm`

- [ ] **Step 1: Metal 实现**

- 成员:`id<MTLBinaryArchive> archive_ = nil; std::string pipelineCachePath_;`
- `setPipelineCachePath`:

```objc
void setPipelineCachePath(const char* path) override {
  savePipelineCache();
  pipelineCachePath_ = path ? path : "";
  archive_ = nil;
  if (pipelineCachePath_.empty()) return;
  if (@available(macOS 11.0, iOS 14.0, *)) {
    auto* d = [MTLBinaryArchiveDescriptor new];
    NSString* p = [NSString stringWithUTF8String:pipelineCachePath_.c_str()];
    if ([[NSFileManager defaultManager] fileExistsAtPath:p])
      d.url = [NSURL fileURLWithPath:p];
    NSError* err = nil;
    archive_ = [device_ newBinaryArchiveWithDescriptor:d error:&err];
    if (!archive_)
      RD_LOGW("rhi.metal", "管线缓存装载失败: %s", err.localizedDescription.UTF8String);
  }
}
```

- `createPipeline`(412 行附近,`newRenderPipelineStateWithDescriptor:pd` 前):

```objc
  if (@available(macOS 11.0, iOS 14.0, *))
    if (archive_) pd.binaryArchives = @[ archive_ ];
```

- `savePipelineCache()`(私有;析构调用):

```objc
void savePipelineCache() {
  if (!archive_ || pipelineCachePath_.empty()) return;
  if (@available(macOS 11.0, iOS 14.0, *)) {
    NSString* tmp = [NSString stringWithUTF8String:(pipelineCachePath_ + ".tmp").c_str()];
    NSError* err = nil;
    // 父目录自动创建
    ...
    [archive_ serializeToURL:[NSURL fileURLWithPath:tmp] error:&err];
    if (!err) {
      NSString* dst = [NSString stringWithUTF8String:pipelineCachePath_.c_str()];
      [[NSFileManager defaultManager] removeItemAtPath:dst error:nil];
      [[NSFileManager defaultManager] moveItemAtPath:tmp toPath:dst error:nil];
    }
  }
}
```

- 析构:savePipelineCache() 在最前。

- [ ] **Step 2: 跑测试 + Commit**

Run: `./scripts/check.sh 2>&1 | tail -4`(全绿)
Commit: `feat(rhi.metal): MTLBinaryArchive 落盘(setPipelineCachePath + 析构保存)`

---

### Task 3: engine 接线 + ctest 双跑

**Files:**
- Modify: `core/api/rd_api.cpp`(set_cache_dir 追加 pipeline 路径)
- Modify: `tools/render_test/main.cpp`(cube/默认路径也接 cache-dir?——默认 cube 路径
  是 demo::CubeScene 不经 Renderer/Engine,**不接**;--model/--scene 路径经 Renderer.init……
  **问题:renderer.init 不持 device setPipelineCachePath**——Renderer 不拥有 device。
  工具侧:main.cpp 在 createDevice 后直接 `device->setPipelineCachePath(...)`;
  engine 侧:set_cache_dir 里 device->setPipelineCachePath)
- Modify: `tests/CMakeLists.txt`(pipeline_cache ctest)

- [ ] **Step 1: engine 接线**

`rd_engine_set_cache_dir`:

```cpp
void rd_engine_set_cache_dir(rd_engine* e, const char* path) {
  if (!e) return;
  e->renderer.setCacheDir(path);
  // pipeline 缓存同目录:<dir>/pipelines/<backend>.bin
  if (e->device && path && path[0]) {
    const std::string dir = std::string(path) + "/pipelines";
    std::filesystem::create_directories(dir);
    const char* bn = e->device->backend() == rd::Backend::Vulkan ? "vulkan"
                   : e->device->backend() == rd::Backend::Metal ? "metal" : "gles";
    e->device->setPipelineCachePath((dir + "/" + bn + ".bin").c_str());
  } else if (e->device) {
    e->device->setPipelineCachePath("");
  }
  e->renderDirty = true;
}
```

(#include <filesystem>)

- [ ] **Step 2: 工具接线(main.cpp)**

scene 模式与 model 模式的 `createDevice` 之后:

```cpp
if (!cacheDir.empty()) {
  const std::string pdir = cacheDir + "/pipelines";
  std::filesystem::create_directories(pdir);
  const char* bn = backend == rd::Backend::Vulkan ? "vulkan" : "metal";
  device->setPipelineCachePath((pdir + "/" + bn + ".bin").c_str());
}
```

- [ ] **Step 3: ctest 双跑**

`tests/CMakeLists.txt` 追加(沿用 ibl_cache 模式;metal + vulkan 各一组):

```cmake
# pipeline 缓存双跑:miss 落盘 → hit 装载日志 → 两图字节一致
foreach(b IN ITEMS metal vulkan)
  if(b STREQUAL "vulkan" AND NOT RD_HAVE_VULKAN)
    continue()
  endif()
  set(d ${CMAKE_BINARY_DIR}/test_pipeline_cache_${b})
  add_test(NAME pipeline_cache_miss_${b}
           COMMAND $<TARGET_FILE:render_test> --backend ${b} --pbr
                   --model ${CMAKE_CURRENT_SOURCE_DIR}/assets/DamagedHelmet.glb
                   --cache-dir ${d} --out ${CMAKE_BINARY_DIR}/pc_miss_${b}.png)
  add_test(NAME pipeline_cache_hit_${b}
           COMMAND $<TARGET_FILE:render_test> --backend ${b} --pbr
                   --model ${CMAKE_CURRENT_SOURCE_DIR}/assets/DamagedHelmet.glb
                   --cache-dir ${d} --out ${CMAKE_BINARY_DIR}/pc_hit_${b}.png)
  set_tests_properties(pipeline_cache_hit_${b} PROPERTIES DEPENDS pipeline_cache_miss_${b})
  add_test(NAME pipeline_cache_equal_${b}
           COMMAND ${CMAKE_COMMAND} -E compare_files ${CMAKE_BINARY_DIR}/pc_miss_${b}.png
                   ${CMAKE_BINARY_DIR}/pc_hit_${b}.png)
  set_tests_properties(pipeline_cache_equal_${b} PROPERTIES DEPENDS pipeline_cache_hit_${b})
endforeach()
```

(注:vulkan 与 ibl 缓存共目录会叠加 IBL 命中——两图仍应字节一致;二跑必装载
pipeline blob。PASS_REGULAR_EXPRESSION 不加(Metal 二跑日志时机不稳定——
archive 装载在 setPipelineCachePath 时,比渲染早,日志在 stdout;稳定。加上:
`PASS_REGULAR_EXPRESSION "管线缓存"`——Vulkan 日志"管线缓存装载";Metal 无成功日志,
只在失败时 W——**统一:Vulkan 打"管线缓存装载",Metal 成功也打**;
Metal setPipelineCachePath 成功时装载大小打 I 日志。)

- [ ] **Step 4: 跑测试 + Commit**

Run: `./scripts/check.sh 2>&1 | tail -4`(全绿,含 pipeline_cache_* 6 条)
Commit: `feat(api): set_cache_dir 接线 pipeline 缓存 + render_test --cache-dir 落盘 + ctest 双跑`

---

### Task 4: AGENTS.md 收尾

- [ ] **Step 1: 更新 + 全量回归 + Commit**

AGENTS.md 追加:

```markdown
- pipeline 缓存(F3D 落地):`Device::setPipelineCachePath`(Vulkan VkPipelineCache
  blob/Metal MTLBinaryArchive[macOS 11+/iOS 14+ 门控]/GLES no-op);
  `rd_engine_set_cache_dir` 一旋钮同开 IBL+pipeline(<dir>/pipelines/<backend>.bin);
  析构落盘,装载/失败日志;ctest pipeline_cache_* 双跑
```

全量回归 + commit `docs: pipeline 缓存收尾(AGENTS.md)`

---

## 附:已知风险与决策记录

| 风险/决策 | 处理 |
|---|---|
| Vulkan 已有内存 pipelineCache_ 命名撞车 | 驱动级成员命名 driverPipelineCache_ |
| Metal archive 版本兼容 | serializeToURL 失败仅告警;装载失败 archive_=nil 正常路径 |
| iOS 11 部署目标 | @available 门控,低系统 no-op |
| 坏 blob | Vulkan 驱动自带 header 校验;Metal 装载失败降级 |
| 多设备并发写 | 单引擎单设备约定;tmp+rename 原子 |

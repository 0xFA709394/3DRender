# Pipeline 缓存 v2 设计(F3D 后续候选 1/3)

日期:2026-08-19
状态:已确认(用户审阅通过)

## 0. 目标与范围

驱动级管线对象磁盘缓存:二次启动/切档后 pipeline 创建走缓存,
消除 shader 编译/管线链接耗时。

**范围**:Vulkan(VkPipelineCache blob)+ Metal(MTLBinaryArchive);
GLES 空实现(无对应机制)。默认关,经 `rd_engine_set_cache_dir` 一旋钮开启。

## 1. RHI 接口

```cpp
// rhi_device.h Device
/// 设置驱动级管线缓存文件路径;空串关闭(默认关)。
/// Vulkan:VkPipelineCache 预载/落盘;Metal:MTLBinaryArchive(macOS 11+/iOS 14+,
/// 低系统 no-op);GLES:no-op。
virtual void setPipelineCachePath(const char* path) { (void)path; }
```

## 2. Vulkan 实现

- `setPipelineCachePath(path)`:若已有 cache 先落盘销毁;读文件(存在则)
  `VkPipelineCacheCreateInfo.pInitialData`;`vkCreatePipelineCache`;存路径。
- `createPipeline`:`vkCreateGraphicsPipelines(device_, pipelineCache_, ...)`。
- device 销毁前:`vkGetPipelineCacheData` → 写 tmp → rename 原子落盘 →
  `vkDestroyPipelineCache`。
- 坏文件/驱动不匹配:驱动自行处理(VkPipelineCache 有 header 校验,
  不兼容即按空缓存工作)——不做额外校验。

## 3. Metal 实现

- `setPipelineCachePath(path)`(@available macOS 11/iOS 14):
  `MTLBinaryArchiveDescriptor` + `url`(存在则装载);存 `archive_` 与路径。
- `createPipeline`:`pd.binaryArchives = @[archive_]`(archive_ 非空时)。
- device 销毁前:`[archive_ serializeToURL:tmp]` + rename 原子落盘。
- 低系统/装载失败:archive_=nil,正常路径(每次新建)。

## 4. engine 接线

`rd_engine_set_cache_dir(e, dir)` 追加:
`e->device->setPipelineCachePath(("<dir>/pipelines/" + backendName + ".bin").c_str())`;
目录自动创建(`<dir>/pipelines/`)。

## 5. 测试

| 层 | 内容 |
|---|---|
| ctest | pipeline_cache_miss/hit(metal + vulkan):一跑文件存在,二跑 PASS_REGULAR "管线缓存"日志,两图字节一致 |
| 回归 | 默认关;现有全绿 |

## 6. 实施顺序

1. RHI 接口 + Vulkan 实现 + ctest(vulkan)
2. Metal 实现 + ctest(metal)
3. engine set_cache_dir 接线 + AGENTS.md

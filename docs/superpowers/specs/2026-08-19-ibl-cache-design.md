# 内容哈希缓存 设计(F3D 落地项 4/4)

日期:2026-08-19
状态:已确认(用户审阅通过)
来源:docs/f3d_learnings.md 融入项 4/4

## 0. 目标与范围

环境 IBL 预滤波(init 的 ~96% 耗时)内容哈希缓存:二次启动/切换回同环境
零 GPU 预滤波,直接上传缓存像素。

**v1 范围**:仅 IBL 预滤波;**默认关**,`rd_engine_set_cache_dir` 显式开启。
pipeline 缓存(VkPipelineCache blob/Metal BinaryArchive)留 v2(需逐后端侵入)。

## 1. 机制

- **键**:FNV-1a 64 位,输入 = equirect 源像素字节 + iblSize + iblMips +
  LUT 尺寸 + 后端无关(RGBA8 输出)。
- **值**:预滤波 cube 全 mip 像素(RGBA8)+ BRDF LUT 像素;文件
  `<cache_dir>/ibl/<hash16hex>.ibc`,头 `{magic 'RDBI', version=1, size, mips,
  lutW, lutH}`。
- **路径统一**:缓存开启时,预滤波一律"逐面离屏目标渲染 + 读回"(GLES 已有
  路径推广到三后端);命中→跳过渲染直接上传 cube;未命中→渲染+读回+写盘+上传。
  未命中多一次读回(一次性 init 成本,可接受)。
- **BRDF LUT**:同键一并缓存(其生成路径同统一读回)。
- **并发/坏文件**:写临时文件再 rename(原子);读时校验 magic/version/尺寸,
  不符即弃(按未命中处理);不锁(单引擎单线程约定)。

## 2. C API

```c
/// 设置 IBL 预滤波磁盘缓存目录;NULL/空串关闭(默认关)。
/// 目录不存在自动创建(一层)。
void rd_engine_set_cache_dir(rd_engine* engine, const char* path);
```

## 3. 测试

| 层 | 内容 |
|---|---|
| 单测(tests/resource/cache_test.cpp) | hash 确定性/雪崩;缓存文件写读往返;坏文件拒绝 |
| golden | cache dir 启用连渲两次(二次=命中),与 golden SSIM 一致 |
| 回归 | 默认关;现有 golden 全不变 |

## 4. 实施顺序

1. foundation hash + resource 缓存文件 IO + 单测
2. Renderer 接线(统一读回路径 + 命中跳过)+ golden 双跑验证
3. AGENTS.md 收尾

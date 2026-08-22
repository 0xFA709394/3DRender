# HDR 环境 + 天空盒 设计

日期:2026-08-23
状态:已确认(用户审阅通过)

## 0. 目标与现状

现状:Environment 只有程序化摄影棚环境(CPU 生成 LDR cubemap)→ SH9 +
RGBA8 预滤波。无 HDR 环境加载、无天空盒背景。

目标:.hdr equirect 加载 → HDR cubemap → SH9 + HDR 预滤波 → 天空盒 +
env.yaw 旋转;程序化环境保留为默认/回退。

非目标:.exr 加载(tinyexr 依赖太重,v2)、多环境混合、环境强度选项(v2)。

## 1. 架构

### 1.1 HDR 加载(resource/hdr_env.{h,cpp})
- stb_image `stbi_loadf`(float RGBA)解 .hdr equirect;stb 已在依赖内。
- `HdrEnv { uint32_t w, h; std::vector<float> pixels; }` + `loadHdrEnv(path)`。
- 测试资产:运行时经 stb_image_write `stbi_write_hdr` 生成小 hdr(64×32 渐变)。

### 1.2 equirect → cube(GPU pass)
- 新 shader `equirect_to_cube.frag`(全屏三角形,cube face 方向采样 equirect 2D,
  方向表复用 prefilter 的 kFaceBasis;等距柱状采样 `dir → (atan2, asin) uv`)。
- 逐面渲到 RGBA16F cube mip0(自建管线,RGBA16F 格式;HDR caps 门控——
  hdr_render_target 已是能力表项)。
- envTex_ 类型:HDR 模式 = RGBA16F cube(GPU pass 生成);程序化模式保持
  RGBA8(CPU 像素直接上传)。**双格式**:envTex_ format 记录成员,
  预滤波 shader 采样不在意格式(都是 float 采样)。

### 1.3 预滤波升 HDR(16F)
- prefilterCube_ 格式 RGBA8 → **RGBA16F**(程序化路径同步升,统一单路径);
  预滤波管线 pd.colorFormat 同步;预滤波输出 HDR(不夹 [0,1])。
- AGENTS 记过"预滤波恒 RGBA8"——本次变更即移除该约束,记新约定。
- IBL 缓存:version 2,pixel 格式 16F(8B/px);键含格式+HDR 源哈希。
  旧 v1 缓存(version 1)读取拒绝(版本不符按未命中)。

### 1.4 SH9 HDR
- projectToSH 输入改 float(HDR 模式用 HDR cube 像素——GPU 生成的 cube 需读回?
  **简化:SH9 从 equirect 源像素直接投影**(CPU,float),不读 GPU cube;
  程序化路径沿用现有 faces 投影)。

### 1.5 天空盒 pass
- 场景 pass 内最先画:fullscreen 三角形,depth 写关/测试 Equal-far?简化:
  专用 pipeline(深度测试 LessEqual + depthWrite 关,位置输出 z=w 即远平面),
  采样 envTex cube;frag 输出经 ACES/线性按 post 模式一致(与 pbr 同 HDR/LDR 路径)。
- 天空盒 shader:`skybox.frag`(dir = invViewProj × ndc;sample cube;曝光同 composite 不,
  天空盒在场景 pass 内,过 post 链统一曝光)。
- 开关:options `env.skybox` bool 默认 true。

### 1.6 env.yaw_deg
- 预滤波/天空盒/equirect 采样方向绕 Y 旋转:shader 内 `dir = rotY(yaw) * dir`;
  UBO:prefilter UBO 加 yaw;skybox UBO 加 yaw;SH9 投影时旋转方向(CPU)。
- 选项 `env.yaw_deg` float -180..180 step 1,默认 0。

## 2. C API

```c
/// 加载 .hdr equirect 环境(替换程序化;失败回退程序化并 RD_ERROR_ASSET)。
rd_result_t rd_engine_set_environment_hdri(rd_engine* engine, const char* path);
/// 恢复程序化环境。
void rd_engine_set_environment_procedural(rd_engine* engine);
```

## 3. 测试

| 层 | 内容 |
|---|---|
| 单测(tests/resource/hdr_env_test.cpp) | stbi_write_hdr 生成 → loadHdrEnv 解码(尺寸/浮点值域) |
| golden | `hdr_env`(头盔 + tests 生成的渐变 hdr + 天空盒);程序化 golden 全回归——16F 升格式预期像素微差,SSIM 判据内;超阈则重生成并目视核对 |
| 缓存 | HDR 键 ≠ LDR 键(格式入键);v1 缓存文件拒绝 |
| 回归 | 默认(程序化+天空盒开?)——**默认天空盒开会改所有 golden!** → 默认 env.skybox=false? |

**关键决策**:env.skybox 默认 false(保持现有 golden 零回归);golden_hdr_env 用例显式开。

## 4. 实施顺序

1. hdr_env 加载 + 单测
2. equirect→cube pass + 预滤波 16F 升级 + SH9 HDR + 缓存 v2 + 程序化 golden 回归核对
3. 天空盒 pass + env.skybox/env.yaw 选项 + golden_hdr_env
4. AGENTS.md 收尾

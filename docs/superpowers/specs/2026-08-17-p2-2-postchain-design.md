# P2-2 设计:后处理链(HDR Bloom → ACES → FXAA)

日期:2026-08-17
状态:已确认(用户审阅通过)
前置:P2-1(阴影+多光源)已完成;阶段二c 上屏链(SceneTarget→blit upscale)即本阶段挂载点

## 0. 目标与范围

P2 第二个子项目:HDR 后处理链。

1. **HDR 管线**:High/Mid 档 SceneTarget 升级 RGBA16F,PBR 输出线性 HDR,
  后处理全程 HDR,末级 ACES tone mapping + gamma。
2. **Bloom**:阈值提取(半分辨率)→ 3 级降采样模糊 → composite 加权叠加。
3. **FXAA**:与 MSAA 互斥(MSAA 档不开 FXAA);Low 档(无 MSAA)兜底。
4. **画质联动**:QualityPreset 加 postEnabled/fxaaEnabled;既有 LDR 路径零变化。

非目标:自动曝光/eye adaptation、bloom 逐级独立权重配置、flare/dirt 纹理、
TAA、motion blur。

## 1. RHI 扩展

- `Format::R16G16B16A16_FLOAT`(formatSize=8):Metal `MTLPixelFormatRGBA16Float`、
  Vulkan `VK_FORMAT_R16G16B16A16_SFLOAT`、GLES `GL_RGBA16F`(internal)+
  `GL_HALF_FLOAT`(upload)。渲染目标用途(TextureUsage::RenderTargetAttachment)。
- caps 新增 `hdr_render_target`:Metal/Vulkan 恒 1;GLES 查
  `GL_EXT_color_buffer_half_float` 扩展。缺失时后处理链自动关闭(回落 LDR blit,
  不影响主渲染)。

## 2. 管线结构(endScene 内,ShadowPass 之后、blit 之前)

```
场景 → SceneTarget(post开=RGBA16F, post关=RGBA8;MSAA/renderScale 照旧)
  │ post 开(High/Mid)
  ├─ bloom extract(threshold=1.0,半分辨率,R16F)
  ├─ blur:extract(1/2) → l1(1/4) → l2(1/8) → l3(1/16),9-tap tent
  └─ composite:sceneResolve + 1.0·l1 + 0.6·l2 + 0.4·l3
       → ACES(Narkowicz 近似) → gamma(1/2.2) → 最终目标(上采样)
  │ post 关(Low)
  └─ blit 上采样(现状)→ msaa==1 时追加 FXAA pass → 最终目标
```

- pbr_forward.frag 输出开关:`lightCount.y = hdrMode`
  (post 开=1:输出线性 HDR;post 关=0:维持现状 Reinhard+gamma)。
  LDR 路径零变化,既有 golden 全部保留。
- 新 shader(均复用 blit.vert 全屏三角形,无顶点缓冲):
  - `bloom_extract.frag`:max(color - threshold, 0) 输出半分辨率。
  - `bloom_blur.frag`:9-tap tent(4 角 1/16 + 4 边 1/8 + 中心 1/4,单 pass 降采样)。
  - `composite.frag`:scene + w1·l1 + w2·l2 + w3·l3(w=1.0/0.6/0.4),
    ACES(Narkowicz:`x*(a*x+b)/(x*(c*x+d)+e)` 经典系数)+ gamma。
    采样槽:0=scene,1=l1,2=l2,3=l3。
  - `fxaa.frag`:标准 FXAA 3.11(浓缩版,仅 luma 边检测+5-tap);
    采样槽 0=blit 输出。vFlip 复用 BlitUBO 约定(bloom/composite/fxaa 的
    输入纹理都是渲染出来的,与 blit 同一方向约定)。
- 采样器:全部 Clamp+Linear(composite 的 l1/l2/l3 与 scene 同线性采样)。

## 3. 画质联动

- QualityPreset 追加:`uint32_t postEnabled; uint32_t fxaaEnabled;`
  - High:{post=1, fxaa=0}(MSAA4 有抗锯齿,不开 FXAA)
  - Mid:{post=1, fxaa=0}(MSAA2)
  - Low:{post=0, fxaa=1}(无 MSAA,FXAA 兜底)
  - 默认 legacy(非档):{post=0, fxaa=0}——**既有 golden 全部不变**。
- bloom 级数 High/Mid 统一 3 级(v1 不分档)。
- post 目标在尺寸/档位变化时重建(与 SceneTarget 同生命周期)。

## 4. 错误处理

- R16F 渲染目标创建失败/caps 缺失:postEnabled 强制 0,回落 LDR blit + 警告日志。
- Low 档 FXAA 目标创建失败:跳过 FXAA(仅 blit)+ 警告。

## 5. 测试与验证

| 层 | 内容 |
|---|---|
| 单测 | formatSize(R16F)=8;post 目标重建尺寸跟随 |
| RHI 契约 | R16F 渲染目标 roundtrip(渲→采样→readback,双后端) |
| golden | `helmet_post`(High,Bloom+ACES,双后端);`helmet_low` 随 FXAA 重新生成;`helmet_shadow`(High 档)随 ACES 重新生成 |
| 手动 | render_test --interactive 观察 Bloom(emissive 辉光)与边缘质量 |

## 6. 实施顺序

1. RHI R16F 格式 + caps + 契约测试
2. shader 4 个(bloom_extract/bloom_blur/composite/fxaa)+ pbr hdrMode 开关
3. renderer PostChain(extract/blur/composite/fxaa 目标与 pass)+ 画质接线
4. golden 生成/回归(helmet_post 新建;low/shadow 重新生成核对)
5. AGENTS.md 收尾

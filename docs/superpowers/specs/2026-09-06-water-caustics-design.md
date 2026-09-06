# 焦散 + 波动方程动态水波纹 设计

日期:2026-09-06
状态:已确认(分节审阅通过)

## 0. 目标与现状

现状:Renderer 有 pbr/unlit/阴影/后处理/蒙皮/morph 等管线,无任何水渲染;
demo 场景在 `tools/render_test/scenes.cpp`(host 直驱),iOS demo 只走 C API
加载 glb 模型,程序场景引擎层不可达。

目标:引擎级 Water 模块——波动方程高度场仿真(GPU ping-pong)+ Jacobian
汇聚近似焦散 + 顶点位移水面/受水体管线;C API 暴露;render_test
`--scene water_pool` 与 iOS demo 场景菜单共用同一实现。

非目标:翻卷浪(高度场限制)、平面反射 pass/屏幕空间折射(反射走 IBL)、
焦散投影到水上物体、Android demo 接入、多水体实例。

## 1. 架构

### 1.1 模块挂载(Renderer 子系统)

新 `core/renderer/water.{h,cpp}`:`WaterSurface` 类,与 `Environment` 同级,
`Renderer` 持有(`std::optional<WaterSurface>`,默认未激活=零回归)。
GPU 资源只经 `rhi::Device` 创建;禁用时整体释放。

endScene 帧序列插两步:

```
dir ShadowPass → spot ShadowPass
→ [新] water.step(cmd)      // 仿真步进 + 焦散 pass(render pass 外)
→ ScenePass:
    受水体(池底/水下物体) → opaque 分区,water receiver 管线
    水面 → blend 分区(半透明),water surface 管线
→ post 链 / blit(不变)
```

### 1.2 绘制主体(WaterRenderable)

`Renderable` 新子类(与 MeshRenderable 平级),持 `MeshRenderResource` + world,
两 mode:
- `Surface`:平面网格(~128×128 段),vert 顶点纹理 fetch 位移;
- `Receiver`:普通网格,frag 简化 pbr + 焦散调制。

管线由 `Renderer` 建,经 `RenderContext` 注入新字段
`waterSurfacePipeline/waterReceiverPipeline`;随 SceneTarget 格式/采样数在
`ensureScenePipelines` 同规则重建(pipeFmt/pipeSamples key 扩展)。
Surface 项走 endScene 既有 blend 分区排序(depthTest 开/depthWrite 关);
Receiver 走 opaque 分区。Water 项不参与实例化分组与视锥剔除保守跳过
(与蒙皮同待遇,动态包围)。

### 1.3 数据流

```
注入(点按/雨滴,CPU 列表) ──UBO──► water_step pass ──► 波场 ping-pong(RGBA16F×2)
                                         │
                                         ▼
                                water_caustics pass ──► 焦散强度图(RGBA16F,R 通道)
                                         │
                  ┌──────────────────────┴─────────────────────┐
                  ▼                                              ▼
         water_surface.vert/frag                        water_receiver.frag
```

## 2. 波动方程仿真

- **纹理**:2× RGBA16F ping-pong(R=当前高度 u,G=上一帧 u_prev;BA 闲置),
  池面正交俯视 UV 域;尺寸随画质档。**不扩 RHI 格式面**(无 RG16F/R16F
  单通道格式,512² RGBA16F 仅 2MB×2,内存可忽略)。
- **离散**(显式二阶有限差分 + 阻尼):
  `u_new = 2u − u_prev + k·(四邻均值 − u) − damp·(u − u_prev)`,
  k = c²·dt²/dx² ≤ 0.5(CFL 稳定,damp ≈ 0.005)。
- **步进**:固定 dt=1/60 子步,按累计帧时间驱动,每帧至多 2 步
  (120Hz 屏不加速;golden 确定性;render_frame 的 dt 只累计不直接进方程)。
- **边界**:纹理 clamp 寻址 ≈ 池壁全反射(涟漪回弹,ripple tank 观感)。
- **注入**:帧内注入列表 UBO(`vec4[8]`:xy=uv,z=强度,w=半径),step shader
  内叠加高斯脉冲到 u_new;点按与雨滴统一走此通道;列表满丢弃最旧。
- **雨滴**:CPU 侧固定 seed LCG(确定性),~0.8s 一滴,`water.rain` 可关
  (golden 必关)。
- **法线**:不预生成法线图,水面 frag 对高度场中央差分现算。

## 3. 焦散生成(Jacobian 汇聚近似)

`water_caustics.frag` 全屏 pass(与波场同分辨率,波场更新后录):
逐 texel:高度梯度 → 水面法线 → 方向光折射 → 落点偏移 p(uv) →
p 的 2×2 Jacobian(邻 texel 差分)→ 强度 = clamp(1/|det J|)。
输出 RGBA16F(R 通道);视差近似(焦散图按水面 UV 索引,非严格落点),
pattern 与波场同源联动。Low 档跳过此 pass(焦散纹理绑 1×1 黑图)。

## 4. 着色

### 4.1 水面(water_surface.vert/.frag)

- vert:高度 fetch×4(位移 + 三点差分法线),波幅 = `water.wave_scale`;
- frag:Schlick Fresnel(F0=0.02)→ IBL prefilterCube 反射 + 方向光 GGX
  高光 + 折射色(水体吸收,Beer-Lambert 随 `water.depth`);
  alpha = Fresnel 混合(近垂直看穿池底,掠射角反射为主);
- blend 管线(srcAlpha/oneMinusSrcAlpha),depthTest 开/depthWrite 关。

### 4.2 受水体(water_receiver.vert/.frag)

Lambert 漫反射 × 方向光 + 阴影 PCF(slot7 比较采样)+ SH 环境项,
`× (1 + caustics × water.caustics_intensity)`;焦散按世界 xz → 池 UV 采样。

### 4.3 纹理槽与 UBO(独立管线族,不占 pbr 槽位)

0=材质贴图(可选)、1=波场、2=焦散、5=prefilterCube、7=方向光阴影
(combined 采样器族,同 blit/skybox;GLES 语义名表加 texWave→1/texCaustics→2);
UBO:slot0=FrameUBO 复用(**尾部扩展 272→336B,water[3] 参数组**——RHI uniform
slot 仅 0..3,不新增 slot;未激活时全零)、slot1=ItemUBO 复用(mvp/world/factors)、
slot2=LightUBO 复用(阴影矩阵/hdrMode);step/caustics 全屏 pass 用各自小 UBO
(slot0,块名 WaterStepUBO/WaterCausticsUBO,GLES 块名表增补)。

## 5. 画质档(QualityPreset 扩展)

| 档 | 仿真分辨率 | 焦散 |
|---|---|---|
| High | 512² | 开 |
| Mid | 256² | 开 |
| Low | 128² | 关(仅位移+法线) |

水面网格密度恒定;GLES 无 `hdr_render_target` caps(half-float 渲染性)
时水整体禁用(Metal/Vulkan 恒可用)。档位切换=WaterSurface 重建。

## 6. C API / options

```c
/// 加载程序场景(本期仅 "water_pool";未知名 RD_ERROR_ASSET)。
rd_result_t rd_engine_load_scene(rd_engine* engine, const char* name);
/// 屏幕像素坐标点按注入涟漪(engine 内相机 ray ∩ 水平面 → uv)。
void rd_engine_water_disturb(rd_engine* engine, float x, float y);
```

- 点按识别不下沉 orbit:iOS 单击手势/host interactive 位移阈值判 tap;
  命中水面才注入(复用 ray 求交,不需要 pick 模型)。
- options.json:`water.rain`(默认 true)/`water.wave_scale`(0..2)/
  `water.caustics_intensity`(0..3)/`water.depth`(0.2..3);
  命令总线 set/get/toggle 自动可用;场景切换释放。
- `rd_engine_water_disturb` 置脏(按需渲染)。

## 7. iOS demo 接入

- `RenderView.loadScene(_:)` → `rd_engine_load_scene`;
- `AppDelegate.DemoState` 扩展 scene 型 state,states() 追加
  `water_pool`(High 档),`applyCurrentState` 分支处理;
- RenderView 加单击 UITapGestureRecognizer(短按无位移,与拖拽/双击共存)
  → `rd_engine_water_disturb`;输入录制通道照常记录(回放同格式)。

## 8. render_test / 测试

- `--scene water_pool`:scenes.cpp 直驱 Renderer+WaterSurface(与其他
  demo 场景同模式,不经 engine);
- golden `water_pool_{metal,vulkan}`:雨关、注入 2 滴(命令
  `water_disturb` 驱动)、固定 dt=1/60、固定帧数后截图,SSIM 判据;
- 静止回归:rain=false + 无注入,平态启动 → 逐帧像素一致(无漂移/无 NaN);
  注入 1 滴后帧间像素必须变化,且足够多帧后回到与初始平态逐像素一致
  (阻尼耗尽,能量守恒);
- 动态冒烟:注入后帧间像素必须变化;
- 门控零操作:`water.wave_scale=0`(静态场)下,`water.caustics_intensity`
  0..3 切换与 `water.rain` on/off 均逐像素一致;
- GLES caps 缺失路径:跳过(日志注明)。

## 9. 已知限制

- 焦散为单次折射视差近似,不投影到水面以上物体;
- 水面不接收阴影、不反射场景几何(仅 IBL 环境反射);
- 波场为高度场,无翻卷浪/破碎;
- WaterRenderable 不参与视锥剔除与实例化分组(动态包围,同蒙皮);
- GLES 无 half-float 渲染 caps 时整体禁用;
- Android demo 接入后续。

## 10. 实施顺序

1. WaterSurface 仿真 ping-pong + step pass + 注入(单测:CPU 参考解对照)
2. 焦散 pass + 水面/受水体管线 + Renderer 接线
3. options + C API + engine 场景构建 + render_test --scene water_pool
4. golden 三类(基线/静止/门控)+ 冒烟
5. iOS demo(场景菜单 + 单击涟漪)+ 模拟器截图验证
6. AGENTS.md 收尾

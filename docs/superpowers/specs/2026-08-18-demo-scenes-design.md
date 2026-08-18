# 场景示例集合 设计

日期:2026-08-18
状态:已确认(用户审阅通过)
前置:P2 全部完成(阴影/多光源/后处理/骨骼动画/拾取/性能基准)

## 0. 目标与范围

增加常见 3D 渲染场景示例,用于功能展示、回归验证与手动调试。

**程序生成(零下载,内核可复用)**——新 `core/resource/primitives.{h,cpp}` 几何生成器
(球/平面/盒,输出 MeshData,含法线/切线/uv,48B 布局):

| 场景 | 内容 | 展示点 |
|---|---|---|
| `material_balls` | 5×5 球阵(metallic 0→1 × roughness 0→1 渐变) | PBR/IBL 材质域 |
| `cornell_box` | 五面盒墙 + 2 内盒 + 方向光 | 阴影 |
| `light_playground` | 平面 + 小物件 + 点光×2 + 聚光×1 | 多光源 |
| `skinned_demo` | 程序骨骼模型循环播放 | 骨骼动画 |
| `instanced_field` | 256 实例化物件(正弦波动) | RHI instancing |

**知名 glb(下载脚本,失败可跳过)**:

| 场景 | 内容 | 大小级 |
|---|---|---|
| `sponza` | Sponza Palace 大场景漫游 | ~25MB |
| `cesium_man` | CesiumMan 真蒙皮动画 | ~0.5MB |

- `scripts/fetch_assets.sh`:curl Khronos glTF-Sample-Assets 官方 URL 到 `assets/`;
  失败不阻塞(弱网/CI 可跳过);运行时资产缺失 → 该场景提示跳过。
- 资产入 `assets/`(新目录,与 tests/assets 区分——demo 资产非测试基线)。

非目标:场景编辑器、glTF 任意场景自动取景以外的美术调整、HDR 环境贴图加载
(环境仍为程序化生成)。

## 1. 载体

- `render_test --scene <name>`:headless 渲 PNG(`--out`);`--interactive --scene <name>`
  窗口浏览(Orbit 手势照常)。`--scene` 与 `--model` 互斥(scene 优先)。
- 移动 demo:iOS/Android 各加「下一场景」按钮(轮换:helmet → material_balls →
  cornell_box → light_playground → skinned_demo → cesium_man(若已下载))。

## 2. 场景实现(render_test 内 scenes 模块)

- `tools/render_test/scenes.{h,cpp}`:`buildDemoScene(name, device, renderer) → DemoScene`
  (资源/相机/灯光/动画状态持有);程序场景经 primitives 组装 ModelAsset 上传。
- `instanced_field` 走 drawInstanced 路径(实例变换 instance-rate 顶点缓冲,
  正弦相位驱动;复用阶段一 instancing 契约)。
- `skinned_demo` 复用 tests/common/skinned_gen(移至 resource 侧共用?——
  保持在 tests/common,demo 工具链接它;内核不动)。

## 3. 测试

- golden:material_balls / cornell_box / light_playground 三场景双后端
  (tests/renderer/demo_scenes_test.cpp;容差 3/0.02)。
- 冒烟:全部程序场景渲染不崩 + 覆盖率>3%;sponza/cesium_man 资产缺失自动 skip。
- 既有 282 测试零回归。

## 4. 实施顺序

1. primitives 生成器 + 单测
2. scenes 模块 + render_test --scene + 3 场景 golden + 全场景冒烟
3. fetch_assets.sh + sponza/cesium_man 场景集成
4. interactive 场景选择 + 移动 demo 场景切换按钮
5. AGENTS.md 收尾

# P2-5 设计:性能基准套件

日期:2026-08-18
状态:已确认(用户审阅通过)
前置:P2-4(拾取交互)已完成

## 0. 目标与范围

P2 收尾子项目:性能基准套件(CI 防回退底座)。

1. **perf_test 工具**:离屏渲染 N 帧逐帧计时(含 GPU),输出 avg/p95/p99/FPS;
   4 个确定性场景,双后端。
2. **软门槛**:基线 JSON 入库,avg 超 1.5× 基线判失败(可关);可重新生成。

非目标:GPU 硬件计数器、能耗采样、多分辨率矩阵、自动化真机农场(归 P4/基建)。

## 1. perf_test 工具(tools/perf_test)

- 离屏渲染(512×512):默认 120 帧,前 20 帧 warmup 不计;逐帧 CPU 墙钟计时
  (beginFrame→submit→waitIdle→endFrame,waitIdle 使计时含 GPU 时间)。
- 参数:`--backend metal|vulkan`(默认 metal,`--backend all` 双后端连跑)、
  `--frames N`(默认 120)、`--json <path>`、`--no-gate`、`--update-baseline`、
  `--scene <name>`(单场景调试)。
- 输出:终端表格(scene/backend/avg/p95/p99/FPS)+ 可选 JSON 落盘。

## 2. 场景集(4 个,确定性)

| 场景 | 内容 |
|---|---|
| `helmet_high` | DamagedHelmet + High 画质(阴影 2048 + HDR Bloom+ACES) |
| `helmet_low` | DamagedHelmet + Low 画质(0.5x + FXAA,无阴影) |
| `skinned_anim` | 蒙皮 quad(运行时生成)循环播放,逐帧 Animator 驱动 + JointUBO 上传 |
| `items_64` | BoxTextured 8×8 网格阵列(64 渲染项;per-item UBO/多 draw 压力) |

## 3. 门槛(软)

- 基线 `tests/perf/baseline.json` 入库:`{scene: {backend: avgMs}}`。
- 判定:avg > 1.5× baseline → 该场景 FAIL,工具非零退出;`--no-gate` 只报告;
  `--update-baseline` 重新生成基线。
- 基线按本机(M 系列 Mac)生成;CI 机器差异大时用 `--no-gate` 只采集。

## 4. 测试与回归

- ctest 冒烟:`perf_test --frames 8 --no-gate --backend metal|vulkan`(验证不崩 +
  输出含 4 场景行;纳入 ctest 需双可用性门控)。
- 既有 280 测试零侵入。

## 5. 实施顺序

1. perf_test 工具(场景/计时/输出)+ ctest 冒烟
2. 基线生成 + 软门槛 + `--update-baseline`
3. AGENTS.md 收尾

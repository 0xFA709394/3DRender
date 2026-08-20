# golden 测试三件套 设计(SSIM + DSL + 输入注入回放)

日期:2026-08-19
状态:已确认(用户审阅通过)
来源:F3D 学习笔记(docs/f3d_learnings.md)落地项之一

## 0. 目标与范围

把 golden image 测试升级为 F3D 式方法论三件套:

1. **SSIM 比较**:自实现亮度域 SSIM(8x8 窗口),替代逐像素容差做主判据;
   跨 GPU/驱动的 AA/抖动微差不再误报。
2. **声明式用例 DSL**:C++ 宏消灭 golden 双后端 twin 样板。
3. **输入注入回放**:host 端触摸/鼠标事件录制+回放(确定性固定 dt),
   手势路径进回归测试;移动端录制同格式日志,host 回放闭环。

非目标:通用 JSON 日志解析器、移动端真机回放(回放只在 host)、视频录制。

## 1. SSIM 比较(tests/common/image.{h,cpp})

```cpp
/// SSIM 比较结果:error = 1 - 平均 SSIM(0=完全一致)。
struct SsimResult { bool pass = false; double error = 0; };
/// 亮度域 SSIM(8x8 高斯窗口逐块,均值);阈值默认 0.05。
SsimResult compareSSIM(const uint8_t* a, const uint8_t* b, uint32_t w, uint32_t h,
                       double errTol = 0.05);
```

- 实现:亮度 `Y = 0.299R+0.587G+0.114B`;按 8x8 块计算 (μx,μy,σx²,σy²,σxy),
  SSIM = (2μxμy+c1)(2σxy+c2)/((μx²+μy²+c1)(σx²+σy²+c2)),c1=(0.01·255)²,
  c2=(0.03·255)²;边缘块按实际尺寸截断。
- golden 用例主判据:`compareSSIM(...).pass`;`compareRGBA8` 保留为辅助日志
  (失败时打印 diffRatio 供定位)。
- 全部既有 golden 迁移验证通过才提交(阈值统一 0.05;个别如有问题单独放宽并注明)。

## 2. 声明式用例 DSL(tests/common/golden_test.h)

```cpp
/// 展开 Metal+Vulkan 双 golden 用例;run 为调用方提供的渲染函数。
#define RD_GOLDEN_TEST(Suite, Name, SceneName, GoldenBase, Tol)  ...
```

- 展开形态:`TEST(Suite, NameMetal)` + `TEST(Suite, NameVulkan)`,内部调
  `rd::test::runGoldenScene(backend, SceneName, GoldenBase ".png", Tol)`。
- 现有 golden 用例(helmet/box/low/shadow/post/ktx2/skinned/demo_*)
  全部改写为宏形式,文件行数应显著下降。

## 3. 输入注入 + host 回放

- `tools/render_test/interactive.mm`:
  - `--record <path>`:把指针事件按 `t action id x y` 行追加(时间戳 ms,
    x/y 为归一化 0..1 视口坐标);scroll/double-tap 同理。
  - `--play <path>`:逐行按时间戳回放;**回放模式用固定 dt**(日志时间戳差值,
    确定性)驱动 render_frame;末帧可 --out 落盘供 golden 比对。
- 注入层:C API 已有 rd_engine_on_pointer 等;host 回放经 C API(顺带验证)。
- ctest `input_replay`:回放 `tests/recordings/orbit_drag.log`(提交) →
  末帧 golden `tests/golden/replay_orbit.png` 断言(SSIM)。
- 移动端录制:iOS/Android demo 加录制开关(长按切换按钮 2s 起停),同一格式
  写入 app 文档目录;`simctl`/`adb pull` 取回。

## 4. 错误处理

- 回放日志缺失/格式错:工具报错退出码 2。
- SSIM 阈值越界:钳到 [0,1]。
- 录制文件写入失败:记警告,不崩。

## 5. 测试与验证

| 层 | 内容 |
|---|---|
| 单测 | SSIM 已知对(同图 error=0;微噪声 <阈值;结构差异 >阈值) |
| golden | 全部既有 golden 迁移 SSIM 后零回归 |
| 回放 | orbit_drag.log → replay_orbit.png golden(双后端) |
| 移动端 | demo 录制开关可用;日志取回后 host 回放一致 |

## 6. 实施顺序

1. SSIM 实现 + 单测 + 全 golden 迁移验证
2. DSL 宏 + 现有 golden 用例改造
3. host 录制/回放 + ctest 手势回归
4. 移动端录制开关
5. AGENTS.md 收尾

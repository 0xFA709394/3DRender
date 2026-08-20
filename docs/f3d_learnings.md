# F3D 引擎设计学习笔记

日期:2026-08-19
对象:`/Users/meetyou/Documents/CG_projs/f3d`(Kitware F3D,libf3d C++20/VTK 系)
目的:提炼可借鉴设计,选择性融入本框架(非照搬——移动端/C++17/自有 RHI)

## 0. F3D 架构速览

```
application(薄壳 CLI)
library(libf3d:engine/window/scene/interactor + options + plugin 工厂)
vtkext(私有 VTK 扩展:渲染器/交互样式/importer 适配)
plugins(native+可选;reader 由 CMake 宏声明注册)
```

关键骨架(`library/public/engine.h`):engine 持有 `options + window + scene + interactor`
四件(PIMPL internals),构造顺序即依赖注入,共享 `options&` 是跨层"总线"。

## 1. 声明式选项系统(最值得学)

`library/options.json`(576 行)是唯一事实源;`cmake/f3dOptions.cmake` 用纯 CMake
`string(JSON ...)` 生成 C++ 选项类。每个叶子条目带 type/default/domain(enum 或
range{min,max,increment})/deprecated;生成 13 组片段(成员、setter/getter、
字符串版、reset、hasDomain、increase/decrease/cycle...)。

- **一类两面**:同一个 `options` 既是强类型聚合 struct(`opt.render.grid.enable`)
  又是字符串反射字典(`set("render.grid.enable", ...)`),CLI/配置/绑定/UI 全走后者,
  引擎内部走前者。
- **domain 即行为**:range 域直接驱动 UI 滑条与 increase/decrease 交互;
  enum 域驱动 cycle。校验/步进/循环语义三合一。
- 零额外工具链:生成器是 CMake,不是 Python;改 JSON 经
  `CMAKE_CONFIGURE_DEPENDS` 自动重配置。

**对我们的意义**:C API 画质档/开关正快速增长(rd_engine_set_quality/shadow/...);
用 JSON schema 生成"强类型内核选项 + C API 字符串反射 + CLI/调试面板"三面,
比逐条手写 C API 可扩展得多。C++17 可承载(variant/optional/string_view 都有)。

## 2. 引擎/场景分层与插件化 loader

- scene 只面对单一 `vtkImporter` 抽象;"纯几何 reader"经 `vtkF3DGenericImporter`
  适配成 importer(适配器模式收敛能力差异),多文件经 `vtkF3DMetaImporter` 聚合。
- reader 选择 = 扩展名粗筛 + 流内容嗅探 + **score 竞争**(0-100,GLTF=60 压 draco);
  声明走 CMake 宏 `f3d_plugin_declare_reader(...)` → 样板代码+清单同点生成。
- 对我们的意义:loader 目前单 glTF;KHR 扩展/Draco/meshopt 增多后可引入
  "reader 注册表 + score"防止 if-else 堆积;适配器思路可用于 KTX2/Draco 旁路。

## 3. 交互:输入 → 绑定 → 文本命令 → 分发

四层解耦:物理输入 → `interaction_bind_t`(修饰键位域+keysym 字符串) →
**文本命令**(`triggerCommand("increase render.light.intensity")`) → 命令分发。
绑定只是命令的触发器;远程控制/脚本/控制台/UI 按钮/测试回放共用同一命令总线。

- **程序化输入注入**:`triggerMouseButton/Position/Wheel/Key/Text` +
  `triggerEventLoop(dt)` —— 无窗口环境可完整驱动交互栈(baseline 测试基石)。
- **按需渲染事件循环**:原子 `RenderRequested`/`StopRequested` + 全渲染/UI-only
  两档,仅 TAA 类强制每帧全渲 —— 移动端省电直接可抄。
- camera 正交代数接口:azimuth/elevation/roll/yaw/pitch/dolly/pan/zoom +
  `camera_state_t` 快照 + 100ms 过渡插值。

**对我们的意义**:OrbitController 已有合成事件注入;可升级为"命令总线 +
触摸注入回放",让手势路径进入回归测试;省电事件循环与移动端 vsync 驱动天然契合。

## 4. 动画子系统(采样式模型)

- "时钟推进"(Tick 推进 CurrentTime×SpeedFactor×Direction,fmod 循环)与
  "时刻加载"(LoadAtTime 重采样)分离;播放/拖动/逐帧/倒放全收敛到采样入口。
- 多动画 indices 组合成一条虚拟时间轴(多 clip 同时播放)。
- 动画状态全经 options 反射(speed_factor 改动即时生效)。

**对我们的意义**:我们的 Animator 是 clip 播放+交叉淡入;可借鉴"采样入口单点化"
与"多 clip 组合时间轴"(而非双 clip 硬编码淡入)。

## 5. 测试基建(golden image 方法论,直接可搬)

- **SSIM 比较**替代逐像素 diff(阈值 0.05):跨 GPU/驱动的 AA/抖动微差不再误报 ——
  我们三后端 golden 的 pixel-tolerance 方案可以升级。
- **f3d_test() 声明式 DSL**:`f3d_test(NAME x DATA cow.vtp ARGS --axis THRESHOLD 0.04)`
  一行声明,自动补 resolution/output/reference;缺失 baseline 自动落盘待人肉确认。
- **交互回放**:`--interaction-test-record` 录制真人输入、`--interaction-test-play` 回放。
- **环境归一化**:`CTEST_F3D_FORCE_DPI_SCALE=1.0`、mac Ctrl/Cmd 统一等;
  超时按 sanitizer 放大;LABELS 多维筛选;CI 失败时 baseline artifact 回收工作流。

**对我们的意义**:golden 升级 SSIM + 测试 DSL + 输入注入回放,三件套可单独落地。

## 6. libf3d 公共 API 风格

- PIMPL(engine 裸 `internals*`;建议我们改 unique_ptr)+ 纯虚子类
  (scene/window/interactor/camera)+ `[[nodiscard]]` + 链式返回。
- 异常层次:`f3d::exception` 根类 + 类内嵌具体异常;**C API 机械翻译层**:
  每函数 `try{}catch→NULL+日志`,异常不过 ABI(与我们的 C API 约定一致,可强化)。
- 外部上下文注入:`engine::createExternal(glfwGetProcAddress)` 一行接 GLFW;
  库延迟 dlopen 图形库。
- 强类型物理量:`angle_deg_t`/`ratio_t`/`color_t`/`direction_t` 构造校验,
  杜绝单位歧义。
- 线程契约显式文档化(仅 requestRender/requestStop 标 "multithreaded safe",
  内部就是 atomic)。

## 7. 资源/资产管线(HDRI 内容哈希缓存是精华)

- **内容寻址缓存**:IBL 预计算(SH 投影 + GGX 预滤波)以文件 MD5 为 key 落盘,
  `cache/<hash>/sh.vtt + specular.vtm`;跨平台缓存目录推导
  (XDG_CACHE_HOME → ~/.cache → ~/Library/Caches → %LOCALAPPDATA%)。
- **分阶段惰性配置 + 失效传播图**:7 个阶段各有 `XxxConfigured`/`HasValidXxx` 双标志,
  选项变更只失效相关阶段 —— 比整体 dirty 精细。
- 加载协议三分:文件/内存流(嗅探选 reader)/零拷贝 mesh 视图。
- 同步加载 + 进度回调,**>0.15s 才弹进度条**(阈值防抖)。

**对我们的意义**:若 resource 层做 KTX2 转码缓存/pipeline 缓存,内容哈希 +
分阶段失效是直接范式;移动端异步加载需自行加层(F3D 没有)。

## 8. 值得融入的 3 个设计(排序)

1. **golden 测试升级:SSIM + 声明式 DSL + 输入注入回放** —— 直接可搬,
   三后端 golden 的鲁棒性刚需;触摸手势回归测试可照建模。
2. **声明式选项 schema → 代码生成** —— 打通内核强类型/C API 反射/CLI/调试面板四面;
   画质档与后端 toggle 的可持续扩展底座。投资较大,单独立项做。
3. **命令总线 + 按需渲染事件循环** —— C 宿主(移动 App)经文本命令驱动引擎全功能,
   无需逐条绑定;按需渲染给移动端省电。中期做。
4. (荣誉提名)**内容哈希 + 分阶段失效缓存** —— 留作 resource 层缓存范式参考。

## 9. 明确不学

- VTK 依赖与整套 vtk 渲染管线(我们自研 RHI)。
- 动态插件/桌面 mimetype 集成(移动端优先级低)。
- 同步加载模型(移动端要异步,我们自行设计)。
- C++20 concepts/模块(我们 C++17)。

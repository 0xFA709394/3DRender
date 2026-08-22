# 命令总线 + 按需渲染 设计(F3D 落地项 3/4)

日期:2026-08-19
状态:已确认(用户审阅通过)
来源:docs/f3d_learnings.md 融入项 3/4

## 0. 目标与范围

1. **命令总线**:引擎行为收敛为文本命令(`triggerCommand` 风格),C 宿主/脚本/
   调试面板共用同一入口;选项经 domain 驱动 increase/decrease/cycle/toggle。
2. **按需渲染**:状态干净时 render_frame 零 GPU 工作(移动端省电);
   Orbit 惯性/动画播放期间持续渲染。

非目标:命令脚本文件、命令补全 UI、撤销/重做栈、远程协议。

## 1. 命令总线(core/api/command_bus.{h,cpp})

```cpp
namespace rd {
/// 命令总线:解析 verb+参数并分发;注册表持有 handler。
class CommandBus {
public:
  using Handler = std::function<bool(const std::string& args, std::string& out)>;
  /// 注册命令(名 → 处理器;返回 false=参数错/失败)。
  void add(const char* name, Handler h);
  /// 执行一行命令;返回 false + out 为错误描述。
  bool exec(const std::string& cmdLine, std::string& out);
};
}
```

v1 内建命令(engine 注册):
- 选项族(全部经 options 反射):
  `set <name> <value>` / `get <name>` / `increase <name>` / `decrease <name>` /
  `cycle <name>` / `toggle <name>`
  - increase/decrease:range 域按 step 增减并钳制;非 range 报错
  - cycle:enum 域循环;非 enum 报错
  - toggle:bool 翻转;非 bool 报错
  - 域数据来自 optionsDomainJson(生成器产出)
- 引擎族:`load_model <path>`、`play_animation <i>`、`crossfade_animation <i> <sec>`、
  `pause_animation <0|1>`、`quality <auto|high|mid|low>`、`reset_view`(Orbit 重置)

## 2. C API

```c
/// 执行一行文本命令;结果码 RD_OK/RD_ERROR_INVALID_ARG。
rd_result_t rd_engine_exec_command(rd_engine* engine, const char* command);
/// 最近一次命令的输出(get 等的值);无输出返回空串。
const char* rd_engine_command_output(rd_engine* engine);
```

## 3. 按需渲染

- `rd_engine.renderDirty`(初始 true);render_frame 开头:
  `if (!renderDirty && !animatorPlaying && !orbit.isMoving()) return;`
  渲完 `renderDirty = false`。
- 置脏:set_option/exec_command、load_gltf、on_pointer/scroll/double_tap/pinch、
  set_quality/shadow、play/crossfade/pause 切换播放态。
- 新增 `OrbitController::isMoving()`(有指针按下或惯性速度非零)。
- 新增 `rd_engine_request_render(engine)` 手动置脏。
- 平台层零改动(CADisplayLink/Choreographer 照常调,干净时跳过)。

## 4. 测试

| 层 | 内容 |
|---|---|
| 单测(tests/api/command_bus_test.cpp) | 解析/分发/选项 increase/cycle/toggle/域外拒绝 |
| api_test | exec_command 往返;按需渲染脏标记 |
| 回归 | golden 全不变(默认每帧渲=现状) |

## 5. 实施顺序

1. command_bus + 单测
2. C API + engine 接线
3. 按需渲染 + isMoving
4. AGENTS.md 收尾

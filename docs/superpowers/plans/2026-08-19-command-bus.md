# 命令总线 + 按需渲染 实现计划(F3D 落地项 3/4)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 按 `docs/superpowers/specs/2026-08-19-command-bus-design.md` 实现 CommandBus + rd_engine_exec_command + 按需渲染。

**通用约定:**
- 构建+测试:`export RD_DEPS_MIRROR=$HOME/.rd_deps_mirror && ./scripts/check.sh`
- 提交规范:每 Task 一个 commit,`<type>(<scope>): 描述`

---

### Task 1: command_bus 模块 + 单测

**Files:**
- Create: `core/api/command_bus.h`、`core/api/command_bus.cpp`
- Modify: `core/CMakeLists.txt`
- Test: `tests/api/command_bus_test.cpp`(新建)、`tests/CMakeLists.txt`(注册)

- [ ] **Step 1: 写失败测试**

`tests/api/command_bus_test.cpp`:

```cpp
// CommandBus 单测:解析/分发/选项命令(increase/cycle/toggle)/错误拒绝。
#include <gtest/gtest.h>
#include "api/command_bus.h"
#include "options_generated.h"

namespace {
struct Fixture {
  rd::Options options;
  rd::CommandBus bus;
  Fixture() {
    // 注册选项族命令(与 engine 相同的内建处理器)
    rd::Options* o = &options;
    bus.add("set", [o](const std::string& a, std::string& out) {
      const auto sp = a.find(' ');
      if (sp == std::string::npos) return false;
      return rd::optionsSet(*o, a.substr(0, sp), a.substr(sp + 1));
    });
    bus.add("get", [o](const std::string& a, std::string& out) {
      return rd::optionsGet(*o, a, out);
    });
    bus.add("toggle", [o](const std::string& a, std::string& out) {
      std::string cur;
      if (!rd::optionsGet(*o, a, cur)) return false;
      if (cur != "true" && cur != "false") return false;
      return rd::optionsSet(*o, a, cur == "true" ? "false" : "true");
    });
  }
};
} // namespace

TEST(CommandBus, SetGet) {
  Fixture f;
  std::string out;
  EXPECT_TRUE(f.bus.exec("set render.exposure 2.0", out));
  EXPECT_TRUE(f.bus.exec("get render.exposure", out));
  EXPECT_EQ(out, "2.000000");
}

TEST(CommandBus, ToggleBool) {
  Fixture f;
  std::string out;
  EXPECT_FALSE(f.options.quality.fxaa);
  EXPECT_TRUE(f.bus.exec("toggle quality.fxaa", out));
  EXPECT_TRUE(f.options.quality.fxaa);
  EXPECT_TRUE(f.bus.exec("toggle quality.fxaa", out));
  EXPECT_FALSE(f.options.quality.fxaa);
  // 非 bool 拒绝
  EXPECT_FALSE(f.bus.exec("toggle render.exposure", out));
}

TEST(CommandBus, UnknownAndBadArgs) {
  Fixture f;
  std::string out;
  EXPECT_FALSE(f.bus.exec("nonsense", out));
  EXPECT_FALSE(f.bus.exec("set", out));          // 缺参数
  EXPECT_FALSE(f.bus.exec("set no.such 1", out)); // 未知名
}

TEST(CommandBus, CycleEnum) {
  Fixture f;
  std::string out;
  // 注册 cycle(enum 域循环)
  rd::Options* o = &f.options;
  f.bus.add("cycle", [o](const std::string& a, std::string& out) {
    const std::string dom = rd::optionsDomainJson(a);
    if (dom.empty()) return false;
    // 粗解析 values(域 JSON 含 "values":[...]);找当前值并前进
    const auto vp = dom.find("\"values\"");
    if (vp == std::string::npos) return false;
    const auto lb = dom.find('[', vp), rb = dom.find(']', lb);
    std::string cur;
    if (!rd::optionsGet(*o, a, cur)) return false;
    // 提取 "x" 列表
    std::vector<std::string> vals;
    for (size_t p = lb; (p = dom.find('"', p + 1)) != std::string::npos && p < rb;) {
      const size_t q = dom.find('"', p + 1);
      vals.push_back(dom.substr(p + 1, q - p - 1));
      p = q;
    }
    for (size_t i = 0; i < vals.size(); ++i)
      if (vals[i] == cur) return rd::optionsSet(*o, a, vals[(i + 1) % vals.size()]);
    return false;
  });
  EXPECT_TRUE(f.bus.exec("cycle quality.tier", out));  // auto → high
  EXPECT_EQ(f.options.quality.tier, "high");
  EXPECT_TRUE(f.bus.exec("cycle quality.tier", out));  // high → mid
  EXPECT_EQ(f.options.quality.tier, "mid");
  EXPECT_TRUE(f.bus.exec("cycle quality.tier", out));  // mid → low
  EXPECT_TRUE(f.bus.exec("cycle quality.tier", out));  // low → auto(回卷)
  EXPECT_EQ(f.options.quality.tier, "auto");
}
```

`tests/CMakeLists.txt` 追加 `api/command_bus_test.cpp`。

- [ ] **Step 2: 跑测试确认失败**

Run: `./scripts/check.sh 2>&1 | tail -5`
Expected: 编译错误(`api/command_bus.h` 不存在)

- [ ] **Step 3: 实现 command_bus**

`core/api/command_bus.h`:

```cpp
/**
 * @file command_bus.h
 * @brief 命令总线:文本命令 → 注册表分发。C 宿主/脚本/调试面板共用入口。
 */
#pragma once
#include <functional>
#include <string>
#include <unordered_map>

namespace rd {

class CommandBus {
public:
  /// 命令处理器:args=去空格参数串;out=输出/错误描述;返回是否成功。
  using Handler = std::function<bool(const std::string& args, std::string& out)>;
  /// 注册命令(名 → 处理器;重复注册覆盖)。
  void add(const char* name, Handler h) { handlers_[name] = std::move(h); }
  /// 执行一行命令;未知命令/处理器失败返回 false,out 为错误描述。
  bool exec(const std::string& cmdLine, std::string& out);

private:
  std::unordered_map<std::string, Handler> handlers_;
};

} // namespace rd
```

`core/api/command_bus.cpp`:

```cpp
#include "api/command_bus.h"
#include "foundation/log.h"

namespace rd {

bool CommandBus::exec(const std::string& cmdLine, std::string& out) {
  // 前导空格跳过;空行成功 no-op
  const auto start = cmdLine.find_first_not_of(" \t");
  if (start == std::string::npos) return true;
  const auto sp = cmdLine.find_first_of(" \t", start);
  const std::string verb = cmdLine.substr(start, sp == std::string::npos
                                                   ? std::string::npos
                                                   : sp - start);
  const std::string args =
      sp == std::string::npos ? "" : cmdLine.substr(sp + 1);
  auto it = handlers_.find(verb);
  if (it == handlers_.end()) {
    out = "未知命令: " + verb;
    RD_LOGW("api.cmd", "%s", out.c_str());
    return false;
  }
  if (!it->second(args, out)) {
    if (out.empty()) out = "命令失败: " + cmdLine;
    RD_LOGW("api.cmd", "%s", out.c_str());
    return false;
  }
  return true;
}

} // namespace rd
```

`core/CMakeLists.txt` 追加 `api/command_bus.cpp`。

- [ ] **Step 4: 跑测试**

Run: `./scripts/check.sh 2>&1 | tail -5`
Expected: 全绿(CommandBus.* 通过)

- [ ] **Step 5: Commit**

```bash
git add core/api/command_bus.* core/CMakeLists.txt tests/api/command_bus_test.cpp tests/CMakeLists.txt
git commit -m "feat(api): CommandBus 命令总线(解析+分发)+ 单测"
```

---

### Task 2: C API + engine 接线

**Files:**
- Modify: `core/api/rd_api.h`、`core/api/rd_api.cpp`
- Test: `tests/api/api_test.cpp`(追加)

- [ ] **Step 1: 失败测试**

`tests/api/api_test.cpp` 追加:

```cpp
// 命令总线 C API:set/get/toggle 经文本命令
TEST(Api, ExecCommand) {
  rd_engine* e = rd_engine_create(RD_BACKEND_METAL);
  ASSERT_NE(e, nullptr);
  EXPECT_EQ(rd_engine_exec_command(e, "set render.exposure 2.0"), RD_OK);
  EXPECT_EQ(rd_engine_exec_command(e, "get render.exposure"), RD_OK);
  EXPECT_STREQ(rd_engine_command_output(e), "2.000000");
  EXPECT_EQ(rd_engine_exec_command(e, "toggle quality.fxaa"), RD_OK);
  EXPECT_EQ(rd_engine_exec_command(e, "get quality.fxaa"), RD_OK);
  EXPECT_STREQ(rd_engine_command_output(e), "true");
  EXPECT_EQ(rd_engine_exec_command(e, "nonsense cmd"), RD_ERROR_INVALID_ARG);
  rd_engine_destroy(e);
}
```

- [ ] **Step 2: rd_api.h**

```c
/// 执行一行文本命令(选项族 set/get/increase/decrease/cycle/toggle +
/// 引擎族 load_model/play_animation/crossfade_animation/pause_animation/quality/reset_view)。
/// 结果码 RD_OK / RD_ERROR_INVALID_ARG(未知命令/参数错)。
rd_result_t rd_engine_exec_command(rd_engine* engine, const char* command);
/// 最近一次命令的输出(get 等的值);无输出返回空串。
const char* rd_engine_command_output(rd_engine* engine);
```

- [ ] **Step 3: rd_api.cpp 接线**

1. 结构体加 `rd::CommandBus bus; char cmdOutput[256] = {};`(输出缓冲)。
2. `rd_engine_create` 内注册内建命令:

```cpp
namespace {
/// 注册内建命令(选项族 + 引擎族)。
void registerCommands(rd_engine* e) {
  auto& bus = e->bus;
  // ---- 选项族 ----
  bus.add("set", [e](const std::string& a, std::string& out) {
    const auto sp = a.find(' ');
    if (sp == std::string::npos) return false;
    e->renderDirty = true;
    return rd::optionsSet(e->options, a.substr(0, sp), a.substr(sp + 1));
  });
  bus.add("get", [e](const std::string& a, std::string& out) {
    return rd::optionsGet(e->options, a, out);
  });
  bus.add("toggle", [e](const std::string& a, std::string& out) {
    std::string cur;
    if (!rd::optionsGet(e->options, a, cur)) return false;
    if (cur != "true" && cur != "false") return false;
    e->renderDirty = true;
    return rd::optionsSet(e->options, a, cur == "true" ? "false" : "true");
  });
  bus.add("increase", [e](const std::string& a, std::string& out) {
    return optionsStep(e, a, +1);
  });
  bus.add("decrease", [e](const std::string& a, std::string& out) {
    return optionsStep(e, a, -1);
  });
  bus.add("cycle", [e](const std::string& a, std::string& out) {
    return optionsCycle(e, a);
  });
  // ---- 引擎族 ----
  bus.add("load_model", [e](const std::string& a, std::string&) {
    e->renderDirty = true;
    return rd_engine_load_gltf(e, a.c_str()) == RD_OK;
  });
  bus.add("play_animation", [e](const std::string& a, std::string&) {
    rd_engine_play_animation(e, atoi(a.c_str()));
    return true;
  });
  bus.add("crossfade_animation", [e](const std::string& a, std::string&) {
    const auto sp = a.find(' ');
    if (sp == std::string::npos) return false;
    rd_engine_crossfade_animation(e, atoi(a.substr(0, sp).c_str()),
                                  float(atof(a.substr(sp + 1).c_str())));
    return true;
  });
  bus.add("pause_animation", [e](const std::string& a, std::string&) {
    rd_engine_pause_animation(e, atoi(a.c_str()));
    return true;
  });
  bus.add("quality", [e](const std::string& a, std::string&) {
    e->renderDirty = true;
    return rd::optionsSet(e->options, "quality.tier", a);
  });
  bus.add("reset_view", [e](const std::string&, std::string&) {
    e->orbit.onDoubleTap();
    e->renderDirty = true;
    return true;
  });
}
}
```

   `optionsStep/optionsCycle`(匿名命名空间辅助;increase/decrease 用 range 域
   step 增减钳制,cycle 用 enum 域循环——domain JSON 粗解析,与单测同):

```cpp
/// increase/decrease:range 域按 step 增减并钳制。
bool optionsStep(rd_engine* e, const std::string& name, int dir) {
  // 读 domain JSON 的 min/max/step(粗解析;无 range 域返回 false)
  const std::string dom = rd::optionsDomainJson(name);
  if (dom.empty()) return false;
  double mn = 0, mx = 0, step = 0;
  if (sscanf(dom.c_str(), "%*[^\"]\"min\" : %lf", &mn) ... // 手写粗解析繁琐;
  // ——简化:options.json 的 range 域固定 4 键(style/min/max/step),逐键找 ":num":
  ...
}
```

   **粗解析太丑——改为:options 生成器补 range 访问器**。给 GenOptions.cmake
   追加生成 `bool optionsRange(name, double& mn, double& mx, double& step)`
   (range 域才 true)+ `bool optionsEnumValues(name, std::vector<std::string>&)`。
   然后 optionsStep/optionsCycle 用它们。这样生成器职责闭环。

3. API 实现:

```cpp
rd_result_t rd_engine_exec_command(rd_engine* e, const char* command) {
  if (!e || !command) return RD_ERROR_INVALID_ARG;
  std::string out;
  if (!e->bus.exec(command, out)) {
    setError(e, out.c_str());
    return RD_ERROR_INVALID_ARG;
  }
  std::strncpy(e->cmdOutput, out.c_str(), sizeof(e->cmdOutput) - 1);
  e->cmdOutput[sizeof(e->cmdOutput) - 1] = '\0';
  return RD_OK;
}
const char* rd_engine_command_output(rd_engine* e) {
  return e ? e->cmdOutput : "";
}
```

- [ ] **Step 4: 生成器补 range/enum 访问器 + 实现 optionsStep/Cycle**

`cmake/GenOptions.cmake` 追加生成:

```cpp
/// range 域查询(有 range 域返回 true 并填 min/max/step)。
bool optionsRange(const std::string& name, double& mn, double& mx, double& step);
/// enum 域查询(有 enum 域返回 true 并填值表)。
bool optionsEnumValues(const std::string& name, std::vector<std::string>& values);
```

(生成器内逐 range/enum 条目生成 if 链)
options_test.cpp 追加断言:`optionsRange("camera.fov_deg", ...)` 与
`optionsEnumValues("quality.tier", ...)`。

optionsStep/optionsCycle 实现(rd_api.cpp 匿名命名空间):

```cpp
bool optionsStep(rd_engine* e, const std::string& name, int dir) {
  double mn, mx, step;
  if (!rd::optionsRange(name, mn, mx, step)) return false;
  std::string cur;
  if (!rd::optionsGet(e->options, name, cur)) return false;
  // int/float 按域 step;bool/enum 报错
  double v = std::stod(cur) + dir * step;
  v = std::max(mn, std::min(mx, v));
  std::string s = std::to_string(v);
  // 去尾零
  s.erase(s.find_last_not_of('0') + 1);
  if (!s.empty() && s.back() == '.') s.pop_back();
  e->renderDirty = true;
  return rd::optionsSet(e->options, name, s);
}
bool optionsCycle(rd_engine* e, const std::string& name) {
  std::vector<std::string> vals;
  if (!rd::optionsEnumValues(name, vals) || vals.empty()) return false;
  std::string cur;
  if (!rd::optionsGet(e->options, name, cur)) return false;
  for (size_t i = 0; i < vals.size(); ++i)
    if (vals[i] == cur) {
      e->renderDirty = true;
      return rd::optionsSet(e->options, name, vals[(i + 1) % vals.size()]);
    }
  return false;
}
```

- [ ] **Step 5: 跑测试 + Commit**

Run: `./scripts/check.sh 2>&1 | tail -5`(全绿)
Commit: `feat(api): rd_engine_exec_command + 内建命令(选项族+引擎族)+ range/enum 访问器`

---

### Task 3: 按需渲染 + Orbit isMoving

**Files:**
- Modify: `core/api/rd_api.cpp`(renderDirty + render_frame 跳过 + request_render)
- Modify: `core/scene/orbit_controller.h/.cpp`(isMoving)
- Modify: `core/api/rd_api.h`(request_render)
- Test: `tests/api/api_test.cpp`、`tests/scene/orbit_test.cpp`(追加)

- [ ] **Step 1: 失败测试**

`tests/scene/orbit_test.cpp` 追加:

```cpp
// 按需渲染:无输入静止;拖拽/惯性期间在动;双击回位后静止
TEST(Orbit, IsMoving) {
  rd::scene::OrbitController c;
  c.frameModel((const float[]){0, 0, 0}, 1.0f);
  EXPECT_FALSE(c.isMoving());   // 静止
  c.onPointerDown(0, 0, 0);
  EXPECT_TRUE(c.isMoving());    // 指针按下
  c.onPointerUp(0, 0, 0);
  c.update(0.016f);
  EXPECT_FALSE(c.isMoving());   // 无惯性(未拖)
  c.onPointerDown(0, 0, 0);
  for (int i = 1; i <= 5; ++i) c.onPointerMove(0, float(i * 20), 0);
  c.onPointerUp(0, 100, 0);
  EXPECT_TRUE(c.isMoving());    // 惯性
  for (int i = 0; i < 600; ++i) c.update(0.016f);
  EXPECT_FALSE(c.isMoving());   // 收敛停止
}
```

`tests/api/api_test.cpp` 追加:

```cpp
// 按需渲染:render_frame 干净时跳过;set_option 置脏
TEST(Api, OnDemandRender) {
  rd_engine* e = rd_engine_create(RD_BACKEND_METAL);
  ASSERT_NE(e, nullptr);
  // 无 surface:render_frame 恒 no-op(干净或脏都不崩)
  rd_engine_render_frame(e, 0.016f);
  EXPECT_EQ(rd_engine_exec_command(e, "set render.exposure 2.0"), RD_OK);
  // 无崩溃即通过(脏标记行为经 render_frame 早退覆盖,难直接观测;
  // 真值验证:见 RD_ONDEMAND 日志/host 手动)
  rd_engine_destroy(e);
}
```

(脏标记难从外部观测,单测验证不崩即可;引擎行为验证留 Task 4 手动/交互。)

- [ ] **Step 2: 跑测试确认失败**

Run: `./scripts/check.sh 2>&1 | tail -5`
Expected: 编译错误(`isMoving`/`request_render` 未声明)

- [ ] **Step 3: 实现**

`orbit_controller.h` 加:

```cpp
  /// 是否在动(指针按下或惯性速度非零);按需渲染用。
  bool isMoving() const {
    return pointerCount() > 0 || vyaw_ != 0 || vpitch_ != 0;
  }
```

`rd_api.h` 加:

```c
/// 手动置脏(下一帧渲染);宿主在状态变更/外部事件时调用。
void rd_engine_request_render(rd_engine* engine);
```

`rd_api.cpp`:
1. 结构体加 `bool renderDirty = true;`(初始 true,首帧必渲)。
2. render_frame 开头:

```cpp
  if (!e->renderDirty && !e->hasAnimation && !e->orbit.isMoving()) return;  // 按需渲染
  e->renderDirty = false;  // 渲完清脏
```

   (hasAnimation 时动画驱动持续渲染——动画播放才脏;修正:动画播放态才脏:
   `!(e->hasAnimation && e->animator.playing())`……更准:把 hasAnimation 换成
   `e->animator.playing()`;无动画时 animator.playing()=false 恒真跳过安全)
   修正条件:`if (!e->renderDirty && !e->animator.playing() && !e->orbit.isMoving()) return;`
3. 置脏点:set_option/exec_command(已在内建处理器)、load_gltf、
   on_pointer/on_scroll/on_pinch/on_double_tap、set_quality、set_shadow_enabled、
   play/crossfade/pause、request_render。
4. request_render 实现:`if (e) e->renderDirty = true;`

- [ ] **Step 4: 跑测试 + Commit**

Run: `./scripts/check.sh 2>&1 | tail -5`(全绿)
Commit: `feat(api): 按需渲染(renderDirty + Orbit isMoving)+ request_render`

---

### Task 4: AGENTS.md 收尾 + 全量回归

**Files:**
- Modify: `AGENTS.md`

- [ ] **Step 1: 更新 + 全量回归 + Commit**

AGENTS.md 追加:

```markdown
- 命令总线+按需渲染(F3D 落地):core/api/command_bus;C API rd_engine_exec_command
  + command_output;内建命令=选项族(set/get/increase/decrease/cycle/toggle,
  域驱动)+ 引擎族(load_model/play_animation/...);render_frame 干净时零 GPU 跳过
  (脏源:选项/模型/输入/画质;Orbit 惯性或动画播放期间持续)
```

全量回归 + commit `docs: 命令总线收尾(AGENTS.md)`

---

## 附:已知风险与决策记录

| 风险/决策 | 处理 |
|---|---|
| 选项域 JSON 粗解析 | 生成器补 optionsRange/optionsEnumValues 访问器,不在运行时解析 JSON |
| 按需渲染跳帧与交互延迟 | 脏源全置脏;Orbit 惯性/动画播放持续渲;平台 vsync 循环零改动 |
| 命令输出缓冲 | cmdOutput[256] 固定;超长截断(get 输出短小,够用) |
| engine 无 surface 时命令 | 纯 CPU 命令(set/get/toggle)照常;load_model 照常(有 device) |

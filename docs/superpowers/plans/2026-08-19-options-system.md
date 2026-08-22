# 声明式选项系统 实现计划(F3D 落地项 2/4)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 按 `docs/superpowers/specs/2026-08-19-options-system-design.md` 实现 options.json + 纯 CMake 代码生成 + engine 集成 + C API。

**Architecture:** options.json 唯一事实源;`cmake/GenOptions.cmake`(cmake -P 脚本,string(JSON))生成 options_generated.h/.cpp 进 build/generated/;engine 持 rd::Options,render_frame 映射进 Renderer。

**通用约定:**
- 构建+测试:`export RD_DEPS_MIRROR=$HOME/.rd_deps_mirror && ./scripts/check.sh`
- 提交规范:每 Task 一个 commit,`<type>(<scope>): 描述`
- 生成文件路径:`build/generated/options_generated.{h,cpp}`;host=build 期生成(与 embedded_shaders 同模式)

---

### Task 1: options.json + GenOptions.cmake + 骨架单测

**Files:**
- Create: `core/api/options.json`、`cmake/GenOptions.cmake`
- Modify: `core/CMakeLists.txt`(build 期生成 + 编进 rd_core)
- Test: `tests/api/options_test.cpp`(新建)、`tests/CMakeLists.txt`(注册)

- [ ] **Step 1: options.json**

`core/api/options.json`:

```json
{
  "quality.tier": {
    "type": "enum", "default": "auto",
    "domain": {"style": "enum", "values": ["auto", "high", "mid", "low"]},
    "doc": "画质档"
  },
  "quality.shadow": {"type": "bool", "default": true, "doc": "阴影开关"},
  "quality.post": {"type": "bool", "default": true, "doc": "后处理链开关"},
  "quality.fxaa": {"type": "bool", "default": false, "doc": "FXAA 开关"},
  "render.exposure": {
    "type": "float", "default": 1.0,
    "domain": {"style": "range", "min": 0.1, "max": 4.0, "step": 0.05},
    "doc": "曝光"
  },
  "camera.fov_deg": {
    "type": "float", "default": 45.0,
    "domain": {"style": "range", "min": 10.0, "max": 120.0, "step": 1.0},
    "doc": "相机视场角(度)"
  },
  "ibl.prefilter_size": {
    "type": "int", "default": 64,
    "domain": {"style": "range", "min": 16, "max": 512, "step": 16},
    "doc": "IBL prefilter 边长"
  },
  "ibl.prefilter_mips": {
    "type": "int", "default": 5,
    "domain": {"style": "range", "min": 1, "max": 8, "step": 1},
    "doc": "IBL prefilter mip 级数"
  },
  "shadow.bias": {
    "type": "float", "default": 0.0015,
    "domain": {"style": "range", "min": 0.0001, "max": 0.01, "step": 0.0001},
    "doc": "阴影 bias"
  },
  "shadow.map_size": {
    "type": "int", "default": 0,
    "domain": {"style": "range", "min": 0, "max": 4096, "step": 128},
    "doc": "阴影贴图边长(0=按画质档)"
  }
}
```

- [ ] **Step 2: GenOptions.cmake(生成器核心,完整实现)**

`cmake/GenOptions.cmake`(cmake -P 运行;输入 -DJSON=<options.json> -DOUT_H/-DOUT_CPP):

```cmake
# options.json → options_generated.h/.cpp 生成器(cmake -P 脚本)
# 用法: cmake -DJSON=<options.json> -DOUT_H=<out.h> -DOUT_CPP=<out.cpp> -P GenOptions.cmake
file(READ ${JSON} JS)

# ---- 收集全部顶层 key(形如 "quality.tier")----
string(JSON nOpt LENGTH ${JS})
set(ENTRIES "")
math(EXPR last "${nOpt} - 1")
foreach(i RANGE ${last})
  string(JSON key MEMBER ${JS} ${i})
  list(APPEND ENTRIES ${key})
endforeach()

set(H "")
set(C "")
string(APPEND H "// GENERATED FILE - 勿手改(由 cmake/GenOptions.cmake 生成)\n")
string(APPEND H "#pragma once\n#include <cstdint>\n#include <string>\n\n")
string(APPEND H "namespace rd {\n\n")
string(APPEND H "/// 声明式选项(唯一事实源:core/api/options.json)\n")
string(APPEND H "struct Options {\n")

# ---- struct 成员(按 . 前缀分组)----
set(GROUPS "")
foreach(E ${ENTRIES})
  string(REGEX MATCH "^[^.]+" G ${E})
  list(FIND GROUPS ${G} gi)
  if(gi EQUAL -1)
    list(APPEND GROUPS ${G})
    string(APPEND H "  struct ${G} {\n")
    # 该组全部成员
    foreach(K ${ENTRIES})
      string(REGEX MATCH "^${G}\\.(.+)" _m ${K})
      if(CMAKE_MATCH_1)
        set(M ${CMAKE_MATCH_1})
        string(JSON type GET ${JS} ${K} type)
        string(JSON def GET ${JS} ${K} default)
        string(JSON doc GET ${JS} ${K} doc)
        if(type STREQUAL "bool")
          string(APPEND H "    bool ${M} = ${def};  ///< ${doc}\n")
        elseif(type STREQUAL "int")
          string(APPEND H "    int32_t ${M} = ${def};  ///< ${doc}\n")
        elseif(type STREQUAL "float")
          string(APPEND H "    float ${M} = ${def}f;  ///< ${doc}\n")
        elseif(type STREQUAL "enum")
          string(APPEND H "    std::string ${M} = \"${def}\";  ///< ${doc}\n")
        endif()
      endif()
    endforeach()
    string(APPEND H "  } ${G};\n")
  endif()
endforeach()
string(APPEND H "};\n\n")

# ---- 自由函数声明 ----
string(APPEND H "/// 全部选项名(静态表)。\n")
string(APPEND H "const char* const* optionsAllNames();\n")
string(APPEND H "int32_t optionsCount();\n")
string(APPEND H "/// 读:值序列化到 out;名不存在返回 false。\n")
string(APPEND H "bool optionsGet(const Options& o, const std::string& name, std::string& out);\n")
string(APPEND H "/// 写:字符串解析+域钳制;名不存在/类型错返回 false。\n")
string(APPEND H "bool optionsSet(Options& o, const std::string& name, const std::string& value);\n")
string(APPEND H "/// 重置为默认。\n")
string(APPEND H "bool optionsReset(Options& o, const std::string& name);\n")
string(APPEND H "/// domain 段原文(UI 侧自解析;无 domain 返回空串)。\n")
string(APPEND H "std::string optionsDomainJson(const std::string& name);\n")
string(APPEND H "\n} // namespace rd\n")

# ---- cpp:名表 + get/set/reset/domain ----
string(APPEND C "// GENERATED FILE - 勿手改\n#include \"options_generated.h\"\n#include <cstring>\n\n")
string(APPEND C "namespace rd {\nnamespace {\n")
string(APPEND C "const char* const kNames[] = {\n")
foreach(E ${ENTRIES})
  string(APPEND C "  \"${E}\",\n")
endforeach()
string(APPEND C "};\n} // namespace\n\n")
string(APPEND C "const char* const* optionsAllNames() { return kNames; }\n")
string(APPEND C "int32_t optionsCount() { return int32_t(sizeof(kNames) / sizeof(kNames[0])); }\n\n")

# get:逐名 if 链
string(APPEND C "bool optionsGet(const Options& o, const std::string& name, std::string& out) {\n")
foreach(E ${ENTRIES})
  string(JSON type GET ${JS} ${E} type)
  if(type STREQUAL "bool")
    string(APPEND C "  if (name == \"${E}\") { out = o.${E} ? \"true\" : \"false\"; return true; }\n")
  elseif(type STREQUAL "enum")
    string(APPEND C "  if (name == \"${E}\") { out = o.${E}; return true; }\n")
  else()
    string(APPEND C "  if (name == \"${E}\") { out = std::to_string(o.${E}); return true; }\n")
  endif()
endforeach()
string(APPEND C "  return false;\n}\n\n")

# set:bool/int/float/enum 分支;range 域钳制;enum 域校验
string(APPEND C "bool optionsSet(Options& o, const std::string& name, const std::string& value) {\n")
foreach(E ${ENTRIES})
  string(JSON type GET ${JS} ${E} type)
  if(type STREQUAL "bool")
    string(APPEND C "  if (name == \"${E}\") { o.${E} = value == \"true\" || value == \"1\"; return true; }\n")
  elseif(type STREQUAL "int")
    string(JSON hasDom ERROR err MEMBER ${JS} ${E} domain min)
    if(NOT hasDom)
      string(JSON dmin GET ${JS} ${E} domain min)
      string(JSON dmax GET ${JS} ${E} domain max)
      string(APPEND C "  if (name == \"${E}\") { o.${E} = std::max(int32_t(${dmin}), std::min(int32_t(${dmax}), int32_t(std::stoi(value)))); return true; }\n")
    else()
      string(APPEND C "  if (name == \"${E}\") { o.${E} = int32_t(std::stoi(value)); return true; }\n")
    endif()
  elseif(type STREQUAL "float")
    string(JSON hasDom ERROR err MEMBER ${JS} ${E} domain min)
    if(NOT hasDom)
      string(JSON dmin GET ${JS} ${E} domain min)
      string(JSON dmax GET ${JS} ${E} domain max)
      string(APPEND C "  if (name == \"${E}\") { o.${E} = std::max(float(${dmin}), std::min(float(${dmax}), std::stof(value))); return true; }\n")
    else()
      string(APPEND C "  if (name == \"${E}\") { o.${E} = std::stof(value); return true; }\n")
    endif()
  elseif(type STREQUAL "enum")
    string(JSON nVals LENGTH ${JS} ${E} domain values)
    string(APPEND C "  if (name == \"${E}\") {")
    math(EXPR lv "${nVals} - 1")
    foreach(i RANGE ${lv})
      string(JSON v GET ${JS} ${E} domain values ${i})
      string(APPEND C " if (value == \"${v}\") { o.${E} = value; return true; }")
    endforeach()
    string(APPEND C " return false; }\n")
  endif()
endforeach()
string(APPEND C "  return false;\n}\n\n")

# reset
string(APPEND C "bool optionsReset(Options& o, const std::string& name) {\n  Options d;\n")
foreach(E ${ENTRIES})
  string(APPEND C "  if (name == \"${E}\") { o.${E} = d.${E}; return true; }\n")
endforeach()
string(APPEND C "  return false;\n}\n\n")

# domainJson:把 domain 段原文存表
string(APPEND C "std::string optionsDomainJson(const std::string& name) {\n")
foreach(E ${ENTRIES})
  string(JSON hasDom ERROR err MEMBER ${JS} ${E} domain)
  if(NOT hasDom)
    string(JSON dom GET ${JS} ${E} domain)
    string(REPLACE "\"" "\\\"" dom ${dom})
    string(REPLACE "\n" "" dom ${dom})
    string(APPEND C "  if (name == \"${E}\") return \"${dom}\";\n")
  endif()
endforeach()
string(APPEND C "  return \"\";\n}\n")
string(APPEND C "\n} // namespace rd\n")

file(WRITE ${OUT_H} ${H})
file(WRITE ${OUT_CPP} ${C})
```

注意:`string(JSON ... MEMBER ... ERROR)` 的域探测写法以 CMake 3.22+ 行为为准;
若 MEMBER 探测不稳,改为在 JSON 里对无域条目显式写 `"domain": null` 并以
`string(JSON x GET ... domain)` 的 NOTFOUND 判断。实现时以实际行为调。

- [ ] **Step 3: CMakeLists 接入 + 单测**

`core/CMakeLists.txt` host 区(与 embedded_shaders 同 add_custom_command 模式):

```cmake
set(RD_OPTIONS_H ${CMAKE_BINARY_DIR}/generated/options_generated.h)
set(RD_OPTIONS_CPP ${CMAKE_BINARY_DIR}/generated/options_generated.cpp)
add_custom_command(
  OUTPUT ${RD_OPTIONS_H} ${RD_OPTIONS_CPP}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/generated
  COMMAND ${CMAKE_COMMAND} -DJSON=${CMAKE_CURRENT_SOURCE_DIR}/api/options.json
          -DOUT_H=${RD_OPTIONS_H} -DOUT_CPP=${RD_OPTIONS_CPP}
          -P ${CMAKE_SOURCE_DIR}/cmake/GenOptions.cmake
  DEPENDS api/options.json ${CMAKE_SOURCE_DIR}/cmake/GenOptions.cmake
  COMMENT "生成 options_generated")
target_sources(rd_core PRIVATE ${RD_OPTIONS_CPP})
target_include_directories(rd_core PUBLIC ${CMAKE_BINARY_DIR}/generated)
```

(ANDROID/IOS 交叉路径也须生成:仿 embedded 的 configure 期路径——
`if(ANDROID OR IOS)` 用 execute_process 在 configure 期跑同一脚本;
先 host 落地,移动端在 Task 2 末尾验证。)

`tests/api/options_test.cpp`:

```cpp
// 声明式选项单测:默认值/get/set/reset/domain 钳制/未知名。
#include <gtest/gtest.h>
#include "options_generated.h"

TEST(Options, Defaults) {
  rd::Options o;
  EXPECT_EQ(o.quality.tier, "auto");
  EXPECT_TRUE(o.quality.shadow);
  EXPECT_TRUE(o.quality.post);
  EXPECT_FALSE(o.quality.fxaa);
  EXPECT_FLOAT_EQ(o.render.exposure, 1.0f);
  EXPECT_FLOAT_EQ(o.camera.fov_deg, 45.0f);
  EXPECT_EQ(o.ibl.prefilter_size, 64);
  EXPECT_EQ(o.shadow.map_size, 0);
}

TEST(Options, GetSetReset) {
  rd::Options o;
  std::string s;
  EXPECT_TRUE(rd::optionsSet(o, "render.exposure", "2.0"));
  EXPECT_FLOAT_EQ(o.render.exposure, 2.0f);
  EXPECT_TRUE(rd::optionsGet(o, "render.exposure", s));
  EXPECT_EQ(s, "2.000000");
  EXPECT_TRUE(rd::optionsReset(o, "render.exposure"));
  EXPECT_FLOAT_EQ(o.render.exposure, 1.0f);
  // bool 与 enum
  EXPECT_TRUE(rd::optionsSet(o, "quality.fxaa", "true"));
  EXPECT_TRUE(o.quality.fxaa);
  EXPECT_TRUE(rd::optionsSet(o, "quality.tier", "low"));
  EXPECT_EQ(o.quality.tier, "low");
  EXPECT_FALSE(rd::optionsSet(o, "quality.tier", "nope"));  // 域外拒绝
  EXPECT_FALSE(rd::optionsSet(o, "nope.x", "1"));           // 未知名拒绝
}

TEST(Options, DomainClamp) {
  rd::Options o;
  EXPECT_TRUE(rd::optionsSet(o, "camera.fov_deg", "999"));
  EXPECT_FLOAT_EQ(o.camera.fov_deg, 120.0f);  // 钳到 max
  EXPECT_TRUE(rd::optionsSet(o, "render.exposure", "0.01"));
  EXPECT_FLOAT_EQ(o.render.exposure, 0.1f);   // 钳到 min
}

TEST(Options, NameTable) {
  EXPECT_GT(rd::optionsCount(), 0);
  EXPECT_NE(rd::optionsAllNames(), nullptr);
  EXPECT_FALSE(rd::optionsDomainJson("quality.tier").empty());
}
```

`tests/CMakeLists.txt` 追加 `api/options_test.cpp`。

- [ ] **Step 4: 跑测试**

Run: `./scripts/check.sh 2>&1 | tail -5`
Expected: 全绿(Options.* 通过;生成器产物被 rd_tests 经 rd_core PUBLIC include 找到)

- [ ] **Step 5: Commit**

```bash
git add core/api/options.json cmake/GenOptions.cmake core/CMakeLists.txt tests/api/options_test.cpp tests/CMakeLists.txt
git commit -m "feat(api): 声明式选项系统(options.json + 纯 CMake 生成器 + 反射 get/set/reset/domain)"
```

---

### Task 2: engine 集成 + C API

**Files:**
- Modify: `core/api/rd_api.h`(4 个新函数)、`core/api/rd_api.cpp`(options 持有+映射)
- Modify: `core/renderer/renderer.h/.cpp`(setShadowBias + compositeUbo_)
- Modify: `shaders/composite.frag`(exposure)
- Test: `tests/api/api_test.cpp`(追加)

- [ ] **Step 1: 失败测试**

`tests/api/api_test.cpp` 追加:

```cpp
// 选项 C API:名表/set/get 往返/非法拒绝
TEST(Api, OptionsApi) {
  EXPECT_GT(rd_options_count(), 0);
  EXPECT_NE(rd_options_name(0), nullptr);
  rd_engine* e = rd_engine_create(RD_BACKEND_METAL);
  ASSERT_NE(e, nullptr);
  char buf[64];
  EXPECT_EQ(rd_engine_get_option(e, "render.exposure", buf, sizeof(buf)), RD_OK);
  EXPECT_STREQ(buf, "1.000000");
  EXPECT_EQ(rd_engine_set_option(e, "render.exposure", "2.5"), RD_OK);
  EXPECT_EQ(rd_engine_get_option(e, "render.exposure", buf, sizeof(buf)), RD_OK);
  EXPECT_STREQ(buf, "2.500000");
  EXPECT_EQ(rd_engine_set_option(e, "no.such", "1"), RD_ERROR_INVALID_ARG);
  EXPECT_EQ(rd_engine_set_option(e, "quality.tier", "nope"), RD_ERROR_INVALID_ARG);
  rd_engine_destroy(e);
}
```

- [ ] **Step 2: rd_api.h**

```c
/// 选项名表(静态,引擎无关)。
int32_t rd_options_count();
const char* rd_options_name(int32_t index);

/// 设置选项(字符串值;bool 用 "true"/"false",enum 用枚举名)。
/// 名不存在/域外值 → RD_ERROR_INVALID_ARG。
rd_result_t rd_engine_set_option(rd_engine* engine, const char* name, const char* value);
/// 读选项到 out(字符串);名不存在/缓冲过小 → RD_ERROR_INVALID_ARG。
rd_result_t rd_engine_get_option(rd_engine* engine, const char* name, char* out,
                                 uint32_t size);
```

- [ ] **Step 3: rd_api.cpp 集成**

1. `#include "options_generated.h"`;结构体加 `rd::Options options;`。
2. 4 个 API 实现(get 注意缓冲钳制 strncpy)。
3. render_frame 开头映射(简表;默认=现状零回归):

```cpp
namespace {
/// 把 options 映射进 renderer(每帧开头;幂等)。
void applyOptions(rd_engine* e) {
  if (!e->rendererReady) return;
  auto& o = e->options;
  // 画质档:tier 字符串 → resolveTier 复用现有路径(把字符串落到 e->quality)
  if (o.quality.tier == "high") e->quality = RD_QUALITY_HIGH;
  else if (o.quality.tier == "mid") e->quality = RD_QUALITY_MID;
  else if (o.quality.tier == "low") e->quality = RD_QUALITY_LOW;
  else e->quality = RD_QUALITY_AUTO;
  // 重应用质量档(映射含 post/fxaa 组合)
  applyQuality(e);
  e->renderer.setShadowEnabled(o.quality.shadow && e->shadowEnabled);
  e->renderer.setExposure(o.render.exposure);   // 新成员(Task 2 Step 4)
  e->renderer.setFovDeg(o.camera.fov_deg);      // 新成员
  e->renderer.setShadowBias(o.shadow.bias);     // 新成员
  if (o.shadow.map_size > 0) e->renderer.setShadowMapSizeOverride(o.shadow.map_size);
  // ibl:经 preset 覆盖后 setQuality(applyQuality 内做)
}
} // namespace
```

4. quality 组合细节(写入 applyQuality):preset = qualityPreset(tier);
   `preset.postEnabled = preset.postEnabled && e->options.quality.post ? 1 : 0;`
   `preset.fxaaEnabled = preset.fxaaEnabled || e->options.quality.fxaa ? 1 : 0;`
   `preset.iblPrefilterSize = o.ibl.prefilter_size; preset.iblPrefilterMips = o.ibl.prefilter_mips;`
   `if (o.shadow.map_size > 0) preset.shadowMapSize = o.shadow.map_size;`
   再 renderer.setQuality(preset)。
   render_frame 开头调 applyOptions(e)。

- [ ] **Step 4: renderer 新成员 + composite exposure**

`renderer.h` 追加:
```cpp
  void setExposure(float e);          ///< composite 曝光(乘性;≤0 视为 1)
  void setFovDeg(float deg);          ///< (备用;engine 相机直接用)
  void setShadowBias(float b);        ///< LightUBO shadowParams.x
  void setShadowMapSizeOverride(uint32_t size);  ///< 0=按档
```
实现:
- setExposure:`compositeExposure_ = e > 0 ? e : 1.0f;`(成员)
- setShadowBias:成员 `shadowBias_`(fillLightUBO 的 0.0015 改为 shadowBias_)
- setShadowMapSizeOverride:成员;setQuality 末尾 `if (override>0) shadowMapSize_=override`
- composite.frag 改:
```glsl
layout(binding = 0) uniform BlitUBO { vec4 params; } u;  // x=vFlip, w=exposure
...
  c *= u.params.w > 0.0 ? u.params.w : 1.0;
```
  composite pass 的 UBO 换成新 `compositeUbo_`(逐 pass 独立 UBO 教训);
  ensurePostTargets 或帧初写 {vFlip,0,0,exposure}。
- fov:render_frame 的 setPerspective 用 `options.camera.fov_deg`(engine 直接读,
  不经 renderer)——修正:applyOptions 里的 setFovDeg 不必要,删除;
  render_frame 投影直接用 o.camera.fov_deg。

- [ ] **Step 5: 跑测试**

Run: `./scripts/check.sh 2>&1 | tail -5`
Expected: 全绿(Api.OptionsApi 通过;golden 全不回归——默认值=现状)

- [ ] **Step 6: Commit**

```bash
git add core shaders tests/api
git commit -m "feat(api): 选项 C API(rd_engine_set/get_option + 名表)+ engine 映射进 Renderer"
```

---

### Task 3: 移动端验证 + 细节收尾

**Files:**
- Modify: `core/CMakeLists.txt`(ANDROID/IOS configure 期生成)

- [ ] **Step 1: 移动端生成路径**

`core/CMakeLists.txt` 在 ANDROID/IOS 分支补 execute_process(configure 期):

```cmake
if(ANDROID OR IOS)
  set(RD_OPTIONS_H ${CMAKE_BINARY_DIR}/generated/options_generated.h)
  set(RD_OPTIONS_CPP ${CMAKE_BINARY_DIR}/generated/options_generated.cpp)
  execute_process(
    COMMAND ${CMAKE_COMMAND} -DJSON=${CMAKE_CURRENT_SOURCE_DIR}/api/options.json
            -DOUT_H=${RD_OPTIONS_H} -DOUT_CPP=${RD_OPTIONS_CPP}
            -P ${CMAKE_SOURCE_DIR}/cmake/GenOptions.cmake
    RESULT_VARIABLE res)
  if(NOT res EQUAL 0)
    message(FATAL_ERROR "options 生成失败")
  endif()
  target_sources(rd_core PRIVATE ${RD_OPTIONS_CPP})
  target_include_directories(rd_core PUBLIC ${CMAKE_BINARY_DIR}/generated)
endif()
```

- [ ] **Step 2: 三端回归**

Run: `./scripts/check.sh 2>&1 | tail -4`(全绿)
Run: Android assembleDebug / iOS build 成功。
Commit: `feat(api): options 移动端生成路径`

---

### Task 4: AGENTS.md 收尾

**Files:**
- Modify: `AGENTS.md`

- [ ] **Step 1: 更新 + 全量回归 + Commit**

AGENTS.md 追加:

```markdown
- 声明式选项系统(F3D 落地):core/api/options.json 唯一事实源;
  cmake/GenOptions.cmake 纯 CMake 生成 options_generated;`rd::Options` 强类型 +
  字符串反射 get/set/reset/domainJson;C API rd_options_count/name + set/get_option;
  engine render_frame 每帧映射进 Renderer(默认=现状零回归);
  composite 曝光经独立 compositeUbo_(逐 pass 独立 UBO)
```

全量回归 + `git commit -m "docs: 选项系统收尾(AGENTS.md)"`

---

## 附:已知风险与决策记录

| 风险/决策 | 处理 |
|---|---|
| 纯 CMake JSON 解析鲁棒性 | 域探测用 NOTFOUND 语义;无域条目 JSON 显式 "domain": null;生成器失败即 configure 失败(早暴露) |
| 选项与画质档组合语义 | post=preset&&opt;fxaa=preset||opt;shadow.map_size>0 覆盖档;文档写明 |
| enum 存字符串 | 反射协议统一字符串;强类型经 engine 映射层(rd_quality_t) |
| 默认值=现状 | golden 零回归是硬门槛;改默认须重生成 golden 并注明 |

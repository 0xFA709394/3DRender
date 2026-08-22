# 声明式选项系统 设计(F3D 学习落地之二)

日期:2026-08-19
状态:已确认(用户审阅通过)
来源:docs/f3d_learnings.md 融入项 2/4

## 0. 目标与范围

声明式选项 schema(JSON)→ 纯 CMake 代码生成:内核强类型访问 + C API 字符串反射 +
domain 驱动 UI/调试面板。

**v1 范围**:10 个选项(5 组),引擎集成 + C API;不引入 Python 依赖(生成器纯 CMake)。

非目标:国际化、选项持久化到磁盘、increase/decrease/cycle 交互绑定(留待命令总线)、
枚举本地化。

## 1. options.json + 纯 CMake 生成器

`core/api/options.json` 唯一事实源;条目结构:

```json
{
  "quality.tier": {
    "type": "enum",
    "default": "auto",
    "domain": {"style": "enum", "values": ["auto", "high", "mid", "low"]},
    "doc": "画质档"
  },
  "render.exposure": {
    "type": "float",
    "default": 1.0,
    "domain": {"style": "range", "min": 0.1, "max": 4.0, "step": 0.05},
    "doc": "曝光"
  }
}
```

`cmake/GenOptions.cmake`(script 模式,`string(JSON ...)`)→
`build/generated/options_generated.h/.cpp`:

- `rd::Options`:嵌套强类型 struct(`quality.tier` → `options.quality.tier`);
  成员带默认值初始化。
- 反射:`static const char* const* allNames()`、`bool get(name, out string)`、
  `bool set(name, string)`、`bool reset(name)`;bool/int/float/enum 统一字符串协议。
- domain 查询:`domainJson(name)` 返回 domain 段原文(UI 侧自解析;v1 不展开结构)。

## 2. v1 选项集

| 名 | 类型 | 默认 | 域 |
|---|---|---|---|
| quality.tier | enum | auto | auto/high/mid/low |
| quality.shadow | bool | true | - |
| quality.post | bool | true | - |
| quality.fxaa | bool | false | - |
| render.exposure | float | 1.0 | 0.1..4.0 step 0.05 |
| camera.fov_deg | float | 45 | 10..120 step 1 |
| ibl.prefilter_size | int | 64 | 16..512 |
| ibl.prefilter_mips | int | 5 | 1..8 |
| shadow.bias | float | 0.0015 | 0.0001..0.01 |
| shadow.map_size | int | 0(按档) | 0..4096 |

## 3. engine 集成

- `rd_engine` 持 `rd::Options options;`。
- `render_frame` 映射(默认值 = 现状行为,golden 零回归):
  - quality.tier → 现有 resolveTier 路径
  - quality.shadow → `renderer.setShadowEnabled`
  - quality.post → `postEnabled = preset.post && opt.post`
  - quality.fxaa → `fxaaEnabled = preset.fxaa || opt.fxaa`
  - render.exposure → 新增 `compositeUbo_`(16B,x=vFlip,w=exposure);
    composite.frag `c *= u.params.w`(**逐 pass 独立 UBO**)
  - camera.fov_deg → camera.setPerspective 的 fov
  - ibl.prefilter_size/mips → 覆盖 preset 的 ibl 字段后 setQuality
  - shadow.bias → 新增 `renderer.setShadowBias`(写 LightUBO shadowParams.x)
  - shadow.map_size → 非 0 时覆盖 preset.shadowMapSize
- 新 C API:
  ```c
  int32_t rd_options_count();                                  // 静态
  const char* rd_options_name(int32_t index);                  // 静态
  rd_result_t rd_engine_set_option(rd_engine*, const char* name, const char* value);
  rd_result_t rd_engine_get_option(rd_engine*, const char* name, char* out, uint32_t size);
  ```

## 4. 测试

| 层 | 内容 |
|---|---|
| 单测(tests/api/options_test.cpp) | 默认值/get/set/reset/domain 钳制/未知名 false |
| api_test | set_option 往返;非法名/非法值报错 |
| 回归 | 默认值=现状;golden 全不变 |

## 5. 实施顺序

1. options.json + GenOptions.cmake + options_generated 骨架 + 单测
2. engine 集成 + C API + api 测试
3. composite exposure + fov/ibl/shadow.bias 接线
4. AGENTS.md 收尾

# 命令脚本 设计(F3D 后续候选 2/3)

日期:2026-08-19
状态:已确认

## 0. 目标

文本脚本(每行一条命令)经命令总线批量执行;宿主配置/自动化测试/调试复现共用。

## 1. 设计

- 脚本格式:每行一条命令(现有 bus 命令);`#` 开头注释;空行跳过。
- 执行语义:遇错误**停止**,输出错误行号+内容;全部成功 RD_OK。
- C API:
  ```c
  /// 执行脚本文件(每行一条命令;# 注释;空行跳过;错误即停)。
  rd_result_t rd_engine_exec_script(rd_engine* engine, const char* path);
  /// 输出:失败时 "行号: 错误描述";成功时为空串。
  // (复用 rd_engine_command_output)
  ```
- 工具:`render_test --script <path>`(scene/model 模式渲染前执行;交互模式启动后执行)。

## 2. 测试

- api 测试:临时脚本(set/toggle/quality + 注释/空行)执行;错误行停止语义。
- ctest:render_test --script 冒烟(脚本 set render.exposure 2.0 + 截图非黑)。
- 回归:现有全绿。

## 3. 实施顺序

1. exec_script 实现 + api 测试
2. render_test --script + ctest 冒烟
3. AGENTS.md

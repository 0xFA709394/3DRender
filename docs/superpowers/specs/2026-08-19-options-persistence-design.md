# 选项持久化 设计(F3D 后续候选 3/3)

日期:2026-08-19
状态:已确认

## 0. 目标

选项保存/加载到 JSON 文件(宿主配置持久化;与 options.json schema 无关——
纯扁平名值对,字符串协议)。

## 1. 设计

- 格式:扁平 `{ "name": "value", ... }`(值为字符串,与反射协议一致)。
- C API:
  ```c
  /// 保存全部选项到 JSON 文件(原子写 tmp+rename)。
  rd_result_t rd_engine_save_options(rd_engine* engine, const char* path);
  /// 从 JSON 文件加载并应用选项;未知名跳过(向前兼容),非法值报错。
  rd_result_t rd_engine_load_options(rd_engine* engine, const char* path);
  ```
- 解析:手写扁平 JSON 扫描器(`"key"\s*:\s*"value"` 或裸值);
  嵌套对象/数组不支持(v1 选项集无嵌套需求)。
- 加载置脏(renderDirty);逐个 set_option 语义(域钳制)。

## 2. 测试

- api 测试:save → 改值 → load → 恢复;未知名跳过;坏文件报错。
- 回归:现有全绿。

## 3. 实施

单 Task:rd_api.{h,cpp} + api_test。

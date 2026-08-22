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

// CommandBus 的实现:verb+参数解析 → 注册表分发。
#include "api/command_bus.h"
#include "foundation/log.h"

namespace rd {

bool CommandBus::exec(const std::string& cmdLine, std::string& out) {
  const auto start = cmdLine.find_first_not_of(" \t");
  if (start == std::string::npos) return true;  // 空行 no-op
  const auto sp = cmdLine.find_first_of(" \t", start);
  const std::string verb =
      cmdLine.substr(start, sp == std::string::npos ? std::string::npos : sp - start);
  const std::string args = sp == std::string::npos ? "" : cmdLine.substr(sp + 1);
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

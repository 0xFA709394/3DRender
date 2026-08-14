/**
 * @file rhi_capability.h
 * @brief 设备能力表:能力枚举(X-macro 单源)+ DeviceCaps 查询容器。
 *
 * 能力探测集中收敛在各后端 init 内,上层只查表不直接碰扩展/特性位。
 * 缺省值 0 = 不支持;非 0 为级别(如 msaa=4)。
 */
#pragma once
#include <array>
#include <cstdint>

namespace rd {

/// 能力枚举;kCount 用于数组维度与遍历。
enum class Capability : uint32_t {
#define RD_CAPABILITY(name) name,
#include "rhi/rhi_constants.inc.h"
  kCount
};

/// 能力值容器:定长数组,缺省 0。后端 init 时 set 上报,之后只读。
class DeviceCaps {
public:
  /// 查询能力级别;0 = 不支持。
  uint32_t get(Capability c) const { return values_[static_cast<uint32_t>(c)]; }
  /// 能力是否支持(get != 0)。
  bool supports(Capability c) const { return get(c) != 0; }
  /// 能力名(日志/诊断);枚举与名表同源,不会越界。
  static const char* name(Capability c);

  /// 上报能力值(仅后端 init 内调用)。
  void set(Capability c, uint32_t v) { values_[static_cast<uint32_t>(c)] = v; }

private:
  std::array<uint32_t, static_cast<size_t>(Capability::kCount)> values_{};
};

} // namespace rd

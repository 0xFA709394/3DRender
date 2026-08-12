#pragma once
#include <cstdint>
#include <type_traits>

namespace rd {

/**
 * @brief 类型安全的不透明句柄模板。
 *
 * 每种 GPU 资源用不同的 Tag 类型实例化（如 using BufferHandle = Handle<struct BufferTag>;），
 * 不同 Tag 的句柄在编译期不可互换，避免把纹理句柄误传给缓冲接口。
 *
 * 约定：
 * - 值为 0 表示无效句柄（默认构造即无效）；工厂/创建函数失败时返回无效句柄。
 * - 句柄本身只是 32 位整数，不拥有资源；资源生命周期由 rhi::Device 的
 *   createXxx/destroyXxx 对管理。
 * - 拷贝/比较开销与 uint32_t 相同，可安全按值传递。
 */
template <typename Tag>
class Handle {
public:
  /// 构造无效句柄（value == 0）。
  Handle() = default;
  /// 由原始值构造句柄；explicit 防止整数隐式转为句柄。
  explicit Handle(uint32_t v) : value_(v) {}

  /// 句柄是否有效（非 0）。
  bool valid() const { return value_ != 0; }
  /// 取出原始 32 位值（后端实现内部使用，业务代码一般不需要）。
  uint32_t value() const { return value_; }

  bool operator==(const Handle& o) const { return value_ == o.value_; }
  bool operator!=(const Handle& o) const { return value_ != o.value_; }

private:
  uint32_t value_ = 0; ///< 0 = 无效；非 0 值由各后端自行分配与解释
};

} // namespace rd

namespace std {
/// 使 Handle 可直接作为 unordered_map/unordered_set 的 key（按原始值散列）。
template <typename Tag>
struct hash<rd::Handle<Tag>> {
  size_t operator()(rd::Handle<Tag> h) const noexcept { return h.value(); }
};
} // namespace std

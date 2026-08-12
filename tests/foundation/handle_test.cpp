// Handle<Tag> 的单元测试：无效值约定、显式构造、按值比较、类型安全。
#include <gtest/gtest.h>
#include "foundation/handle.h"

namespace {
// 两个互不相关的 Tag：验证不同资源的句柄类型在编译期隔离。
struct FooTag;
using FooHandle = rd::Handle<FooTag>;
struct BarTag;
using BarHandle = rd::Handle<BarTag>;

// 默认构造 = 无效句柄（0 值约定）
TEST(Handle, DefaultIsInvalid) {
  FooHandle h;
  EXPECT_FALSE(h.valid());
}

// 显式构造非 0 值 = 有效句柄，value() 可取回原始值
TEST(Handle, ExplicitValueIsValid) {
  FooHandle h(42);
  EXPECT_TRUE(h.valid());
  EXPECT_EQ(h.value(), 42u);
}

// 相等性按原始值
TEST(Handle, EqualityByValue) {
  EXPECT_EQ(FooHandle(1), FooHandle(1));
  EXPECT_NE(FooHandle(1), FooHandle(2));
}

// 类型安全：不同 Tag 的句柄不能隐式互转（编译期约束的运行期验证）
TEST(Handle, DistinctTypesNotComparable) {
  // 不同类型句柄无法互转/比较 —— 静态断言验证类型安全
  EXPECT_FALSE((std::is_convertible_v<FooHandle, BarHandle>));
}
} // namespace

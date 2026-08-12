// log.h 的单元测试：sink 路由/格式化、最低级别过滤。
// 注意全局状态的恢复：每个用例结束都把 sink/级别还原，避免影响其他用例。
#include <gtest/gtest.h>
#include "foundation/log.h"
#include <string>
#include <vector>

namespace {
// 日志捕获器：sink 回调把每条日志格式化为 "level|tag|msg" 存入 lines。
// 用静态 self 指针是因为 LogSink 是普通函数指针（无捕获 lambda 可以转换，
// 但需要地方存结果）。
struct Capture {
  std::vector<std::string> lines;
  static Capture* self;
  static void sink(rd::LogLevel level, const char* tag, const char* msg) {
    self->lines.push_back(std::string(rd::logLevelName(level)) + "|" + tag + "|" + msg);
  }
};
Capture* Capture::self = nullptr;

// sink 路由 + printf 风格格式化 + 级别名映射
TEST(Log, RoutesToSinkWithFormatting) {
  Capture c;
  Capture::self = &c;
  rd::setLogSink(&Capture::sink);
  rd::log(rd::LogLevel::Info, "rhi", "created %s #%d", "buffer", 7);
  ASSERT_EQ(c.lines.size(), 1u);
  EXPECT_EQ(c.lines[0], "info|rhi|created buffer #7");
  rd::setLogSink(nullptr);
}

// 低于最低级别的日志被丢弃，不低于的通过
TEST(Log, FiltersBelowMinLevel) {
  Capture c;
  Capture::self = &c;
  rd::setLogSink(&Capture::sink);
  rd::setLogMinLevel(rd::LogLevel::Warn);
  rd::log(rd::LogLevel::Info, "rhi", "hidden");
  rd::log(rd::LogLevel::Error, "rhi", "shown");
  EXPECT_EQ(c.lines.size(), 1u);
  EXPECT_EQ(c.lines[0], "error|rhi|shown");
  rd::setLogSink(nullptr);
  rd::setLogMinLevel(rd::LogLevel::Debug);
}
} // namespace

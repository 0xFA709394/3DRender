// log.h 的实现：全局 sink/级别过滤 + 平台默认输出（Android=logcat，其余=stderr）。
#include "foundation/log.h"
#include <cstdarg>
#include <cstdio>
#if defined(__ANDROID__)
#include <android/log.h>
#endif

namespace rd {

namespace {
// 全局日志状态。P0 阶段假定在渲染线程/主线程串行调用，未做线程同步；
// 若未来多线程写日志，需要为这两个变量加锁或改为原子。
LogSink g_sink = nullptr;
LogLevel g_minLevel = LogLevel::Debug;
} // namespace

const char* logLevelName(LogLevel level) {
  switch (level) {
    case LogLevel::Debug: return "debug";
    case LogLevel::Info:  return "info";
    case LogLevel::Warn:  return "warn";
    case LogLevel::Error: return "error";
  }
  return "unknown";
}

void setLogSink(LogSink sink) { g_sink = sink; }
void setLogMinLevel(LogLevel level) { g_minLevel = level; }

void log(LogLevel level, const char* tag, const char* fmt, ...) {
  // 级别过滤：低于阈值直接丢弃，避免格式化开销。
  if (level < g_minLevel) return;
  // 定长栈缓冲格式化，超长截断；内核路径不分配堆内存。
  char buf[1024];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  // 平台层注入了 sink 则直接转发（sink 负责后续落地，如 os_log/UI）。
  if (g_sink) {
    g_sink(level, tag, buf);
    return;
  }
#if defined(__ANDROID__)
  // Android 默认落地 logcat，级别映射到 android log priority。
  int prio = ANDROID_LOG_DEBUG;
  if (level == LogLevel::Info) prio = ANDROID_LOG_INFO;
  if (level == LogLevel::Warn) prio = ANDROID_LOG_WARN;
  if (level == LogLevel::Error) prio = ANDROID_LOG_ERROR;
  __android_log_print(prio, tag, "%s", buf);
#else
  // host/iOS 默认落地 stderr，格式：[level] tag: msg
  fprintf(stderr, "[%s] %s: %s\n", logLevelName(level), tag, buf);
#endif
}

} // namespace rd

/**
 * @file log.h
 * @brief 内核统一日志接口。
 *
 * 用法：RD_LOGD/I/W/E(tag, fmt, ...)，tag 约定为模块名（如 "rhi.vk"、"rhi.metal"）。
 * 默认输出：Android → logcat；其他平台 → stderr。可通过 setLogSink 重定向
 * （例如平台层转发到 os_log 或应用 UI）。内核不使用异常，日志是重要的错误反馈通道。
 */
#pragma once

namespace rd {

/// 日志级别；数值单调递增，便于用 < 比较过滤。
enum class LogLevel { Debug = 0, Info, Warn, Error };

/// 返回级别的短名字（"debug"/"info"/"warn"/"error"），用于默认 stderr 输出格式。
const char* logLevelName(LogLevel level);

/// 日志输出回调。由平台层/应用注入；在内核日志线程上同步调用，实现须快速返回。
using LogSink = void (*)(LogLevel level, const char* tag, const char* msg);

/**
 * @brief 设置日志输出目标。
 * @param sink 输出回调；传 nullptr 恢复默认输出（Android=logcat，其余=stderr）。
 */
void setLogSink(LogSink sink);
/// 设置最低输出级别；低于该级别的日志直接丢弃（默认 Debug，即全量输出）。
void setLogMinLevel(LogLevel level);

/**
 * @brief 输出一条日志（printf 风格格式化，最大约 1KB，超长截断）。
 * @param level 级别；低于 setLogMinLevel 设定的阈值时不产生任何输出。
 * @param tag   模块标签，约定用模块名（如 "rhi.vk"）。
 */
void log(LogLevel level, const char* tag, const char* fmt, ...);

} // namespace rd

/// @name 日志便捷宏（推荐业务代码使用，自动带上 ::rd::LogLevel）
/// @{
#define RD_LOGD(tag, ...) ::rd::log(::rd::LogLevel::Debug, tag, __VA_ARGS__)
#define RD_LOGI(tag, ...) ::rd::log(::rd::LogLevel::Info, tag, __VA_ARGS__)
#define RD_LOGW(tag, ...) ::rd::log(::rd::LogLevel::Warn, tag, __VA_ARGS__)
#define RD_LOGE(tag, ...) ::rd::log(::rd::LogLevel::Error, tag, __VA_ARGS__)
/// @}

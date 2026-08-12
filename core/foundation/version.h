/**
 * @file version.h
 * @brief 内核版本查询接口。
 */
#pragma once
namespace rd {
/// 返回内核版本字符串（如 "0.1.0-p0"），用于运行时诊断与示例展示。
const char* version();
}

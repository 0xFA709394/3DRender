/**
 * @file embedded_shaders.h
 * @brief 内嵌 cube shader 字节的查询接口。
 *
 * 实现（embedded_shaders.cpp）由构建系统自动生成，勿手改：
 * host 在 build 期生成；Android/iOS 在 configure 期生成（iOS 真机/模拟器的
 * metallib 分别编译，由 RD_EMBED_IOS_METAL / RD_EMBED_IOS_SIMULATOR 选择）。
 */
#pragma once
#include "rhi/rhi_types.h"
#include <cstddef>
#include <cstdint>

namespace rd {
/**
 * @brief 返回内嵌的 cube shader 字节。
 * @param backend 目标后端（决定返回 SPIR-V / metallib / GLSL ES 文本）。
 * @param stage   顶点或片段阶段。
 * @param data/size 输出：指向静态存储的字节区间，调用方勿释放。
 * @return 该后端无内嵌产物（编译期未嵌入）时返回 false。
 * @note 入口名约定：Metal="main0"，其余="main"（由调用方按后端选择）。
 */
bool embeddedCubeShader(Backend backend, ShaderStage stage, const uint8_t** data, size_t* size);
} // namespace rd

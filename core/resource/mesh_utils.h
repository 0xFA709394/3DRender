/**
 * @file mesh_utils.h
 * @brief CPU 网格工具:切线计算(Lengyel 法,手性存 w)。
 * 顶点布局约定:pos3@0|normal3@12|tangent4@24|uv2@40(floatStride 12)。
 */
#pragma once
#include "rhi/rhi_types.h"
#include <cstdint>

namespace rd {

/// 就地计算切线写入顶点 tangent 字段。
/// @param vertices 交错顶点(float),tangent 段预置 0
/// @param floatStride 每顶点 float 数(本框架=12)
/// @return 成功 true;全部三角形 uv 退化(面积≈0)返回 false(调用方降级)
bool computeTangents(float* vertices, uint32_t vertexCount, const void* indices,
                     uint32_t indexCount, IndexType indexType, uint32_t floatStride);

} // namespace rd

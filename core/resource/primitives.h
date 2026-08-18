/**
 * @file primitives.h
 * @brief 程序化几何生成器(球/平面/盒):输出 48B 交错 MeshData(含切线)。
 * demo 场景与测试共用;内核级复用(勿手写零散几何)。
 */
#pragma once
#include "resource/gltf_loader.h"

namespace rd::primitives {

/// UV 球:radius 半径,segments 经向分段(≥8),rings 纬向分段(≥4)。
MeshData makeSphere(float radius, uint32_t segments, uint32_t rings);
/// 平面:y 朝上,中心原点,边长 size;uv 平铺 repeat 次。
MeshData makePlane(float size, float repeat = 1.0f);
/// 盒:中心原点,三边长;逐面法线。
MeshData makeBox(float sx, float sy, float sz);

} // namespace rd::primitives

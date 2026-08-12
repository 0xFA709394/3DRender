/**
 * @file image.h
 * @brief 测试用图像工具：PNG 读写与容差比较（rd::test 命名空间）。
 *
 * 基于 stb_image/stb_image_write（单头库，实现在 image.cpp）。
 * 用于 golden image 测试与 render_test/img_check 工具。
 */
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace rd::test {

/// 内存图像：RGBA8 紧凑排列（行主序，顶向下——与各后端 readback 输出一致）。
struct Image {
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<uint8_t> pixels; // RGBA8 紧凑排列
};

/// 保存 RGBA8 像素为 PNG；成功返回 true。
bool savePNG(const std::string& path, uint32_t width, uint32_t height, const uint8_t* rgba);
/// 加载 PNG 并转为 RGBA8；失败返回 width==0/pixels 空的 Image。
Image loadPNG(const std::string& path);

/// 容差比较结果。
struct CompareResult {
  bool pass = false;        ///< 是否通过（diffRatio <= ratioTol）
  double diffRatio = 0;   // 超差像素占比
  int maxChannelDiff = 0; // 最大单通道差值
};

/**
 * @brief 逐像素比较两幅 RGBA8 图像（尺寸均为 w×h）。
 * @param channelTol 单通道容差：像素任一通道差超过该值即计为"超差像素"。
 * @param ratioTol   超差像素占比上限：不超过则 pass=true。
 */
CompareResult compareRGBA8(const uint8_t* a, const uint8_t* b, uint32_t w, uint32_t h,
                           int channelTol, double ratioTol);

} // namespace rd::test

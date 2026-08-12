// ============================================================================
// img_check：截图结构校验工具（golden image 的稳健替代/补充）。
// 验证"确实渲染出了彩色立方体"，跨设备/分辨率稳健（不逐像素比对）。
//
// 用法: img_check <png> [--bg r,g,b] [--min-coverage 0.05]
// 检查项（任一不过即 FAIL）：
//   1) 去重采样色数 > 32（排除单色/花屏/无渲染）
//   2) 可选（--bg 给出时）：四角像素为背景色（排除全屏糊满/错位）
//   3) 非背景像素占比 >= min-coverage（立方体确实占了可见面积）
// 退出码: 0=PASS；1=FAIL；2=用法错误。
// ============================================================================
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include "common/image.h"

int main(int argc, char** argv) {
  if (argc < 2) {
    fprintf(stderr, "用法: img_check <png> [--bg r,g,b] [--min-coverage 0.05]\n");
    return 2;
  }
  const char* path = argv[1];
  // ---- 参数解析（默认背景 26,26,31 = 场景清屏色 0.1,0.1,0.12 的 8bit 值）----
  bool checkBg = false;
  int bgR = 26, bgG = 26, bgB = 31;
  double minCoverage = 0.05;
  for (int i = 2; i < argc; ++i) {
    if (!strcmp(argv[i], "--bg") && i + 1 < argc) {
      checkBg = sscanf(argv[++i], "%d,%d,%d", &bgR, &bgG, &bgB) == 3;
    } else if (!strcmp(argv[i], "--min-coverage") && i + 1 < argc) {
      minCoverage = atof(argv[++i]);
    }
  }

  auto img = rd::test::loadPNG(path);
  if (img.pixels.empty()) {
    fprintf(stderr, "FAIL: 无法加载 %s\n", path);
    return 1;
  }

  // ---- 检查 1：采样去重色数 ----
  std::set<uint32_t> colors;
  for (size_t i = 0; i + 3 < img.pixels.size(); i += 64) { // 每 16 像素采样
    colors.insert((img.pixels[i] << 16) | (img.pixels[i + 1] << 8) | img.pixels[i + 2]);
  }
  if (colors.size() <= 32) {
    fprintf(stderr, "FAIL: 颜色数 %zu <= 32（疑似单色/无渲染）\n", colors.size());
    return 1;
  }

  // ---- 检查 2（可选）：四角为背景色（容差 ±8/通道）----
  auto nearColor = [&](size_t idx, int r, int g, int b, int tol) {
    return abs(int(img.pixels[idx]) - r) <= tol && abs(int(img.pixels[idx + 1]) - g) <= tol &&
           abs(int(img.pixels[idx + 2]) - b) <= tol;
  };
  if (checkBg) {
    const size_t w = img.width, h = img.height;
    const size_t corners[4] = {0, (w - 1) * 4, (h - 1) * w * 4, ((h - 1) * w + w - 1) * 4};
    for (size_t c : corners) {
      if (!nearColor(c, bgR, bgG, bgB, 8)) {
        fprintf(stderr, "FAIL: 角落像素非背景色 (%d,%d,%d)\n", img.pixels[c],
                img.pixels[c + 1], img.pixels[c + 2]);
        return 1;
      }
    }
  }

  // ---- 检查 3：非背景像素覆盖率 ----
  uint64_t covered = 0;
  for (size_t i = 0; i + 3 < img.pixels.size(); i += 4) {
    if (!nearColor(i, bgR, bgG, bgB, 8)) ++covered;
  }
  double coverage = double(covered) / (double(img.width) * img.height);
  printf("img_check: %ux%u, 采样色数=%zu, 覆盖率=%.3f\n", img.width, img.height, colors.size(),
         coverage);
  if (coverage < minCoverage) {
    fprintf(stderr, "FAIL: 覆盖率 %.3f < %.3f\n", coverage, minCoverage);
    return 1;
  }
  printf("PASS\n");
  return 0;
}

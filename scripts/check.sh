#!/bin/bash
# ============================================================================
# 一键验证脚本（host）：配置 + 构建 + 全部测试（Vulkan 经 MoltenVK 跑真渲染）
# 用法：./scripts/check.sh
# 更新 golden image：RD_UPDATE_GOLDENS=1 ctest --test-dir build -R Cube
#   （更新后须目视核对 tests/golden/*.png 再提交）
# ============================================================================
set -euo pipefail          # 任一步失败立即退出；未定义变量/管道中间失败也算错误
cd "$(dirname "$0")/.."    # 无论从何处调用都回到仓库根
cmake -S . -B build
cmake --build build -j8
ctest --test-dir build --output-on-failure
echo "hint: RD_UPDATE_GOLDENS=1 ctest ... 可重新生成 golden image"

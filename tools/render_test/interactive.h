// render_test --interactive:GLFW 窗口 + Metal swapchain + C API 鼠标交互(macOS)。
#pragma once

namespace rd::tool {
/// 打开交互窗口;modelPath 非空则加载模型。窗口关闭返回 0;初始化失败返回 1。
/// 环境变量 RD_INTERACTIVE_FRAMES=N:渲 N 帧后自动退出(冒烟用)。
int runInteractive(const char* modelPath);
} // namespace rd::tool

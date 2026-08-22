// render_test --interactive:GLFW 窗口 + Metal swapchain + 鼠标交互(macOS)。
#pragma once

namespace rd::tool {
/// 打开交互窗口;sceneName 非空走 demo 场景直驱(Renderer+OrbitController),
/// 否则经 C API(modelPath 非空加载模型)。窗口关闭返回 0;初始化失败返回 1。
/// recordPath 非空:指针事件录制到该文件(归一化坐标);
/// playPath 非空:按日志回放(固定 dt,确定性;仅 C API 路径有效)。
/// 环境变量 RD_INTERACTIVE_FRAMES=N:渲 N 帧后自动退出(冒烟用)。
int runInteractive(const char* modelPath, const char* sceneName, const char* recordPath,
                   const char* playPath, const char* scriptPath = nullptr);
} // namespace rd::tool

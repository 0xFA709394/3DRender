// 空文件：让 CMake/Xcode 以 C++ 链接器链接本 target（rd_core 为 C++ 静态库）。
// 纯 Swift/ObjC target 默认用 C 链接器，会缺 C++ 运行时（libc++/libstdc++）符号。
#include <cstdint>

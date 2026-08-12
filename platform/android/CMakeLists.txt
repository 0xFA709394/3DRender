add_library(rd_jni SHARED jni/rd_jni.cpp)
target_link_libraries(rd_jni PRIVATE rd_core)
# 16KB 页对齐（Android 15+/模拟器 16KB 系统要求，否则弹兼容性警告框）
target_link_options(rd_jni PRIVATE "-Wl,-z,max-page-size=16384")

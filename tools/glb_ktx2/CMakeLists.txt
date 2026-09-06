# glb_ktx2:glb 纹理 ASTC(KTX2)化——包体积瘦身工具。
# 用法: glb_ktx2 <in.glb> <out.glb> [--max-dim 1024] [--quality 60]
# 机制:解析 glb(12B 头 + JSON/BIN chunk)→ 逐图像 stb 解码 →(可选)降采样
# → libktx RGBA8 KTX2 → CompressAstcEx(4x4)→ 追加 BIN + 改 JSON mimeType/bufferView。
add_executable(glb_ktx2 main.cpp)
target_include_directories(glb_ktx2 PRIVATE ${ktx_SOURCE_DIR}/include)
target_link_libraries(glb_ktx2 PRIVATE rd_core nlohmann_json::nlohmann_json draco_static)

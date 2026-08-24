# ============================================================================
# 内嵌 shader 名单(core/CMakeLists.txt 的 embedded 生成与 GenEmbedded.cmake 共用)
# 新增 shader:此处注册一次即可(embedded 产物按名表驱动)
# ============================================================================
set(RD_EMBED_SHADERS
  cube unlit pbr_forward prefilter blit shadow_depth bloom_extract bloom_blur
  composite fxaa pbr_forward_skinned shadow_depth_skinned equirect_to_cube
  skybox pbr_forward_instanced shadow_depth_mask shadow_depth_instanced)

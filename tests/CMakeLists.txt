include(GoogleTest)

add_executable(rd_tests
  foundation/smoke_test.cpp
  foundation/handle_test.cpp
  foundation/log_test.cpp
  foundation/math_test.cpp
  foundation/task_queue_test.cpp
  rhi/rhi_types_test.cpp
  rhi/factory_test.cpp
  rhi/caps_test.cpp
  rhi/buffer_memory_test.cpp
  rhi/retire_test.cpp
  rhi/blend_test.cpp
  rhi/instancing_test.cpp
  rhi/cube_target_test.cpp
  rhi/depth_test.cpp
  rhi/compressed_texture_test.cpp
  rhi/target_texture_test.cpp
  rhi/msaa_test.cpp
  rhi/depth_texture_test.cpp
  rhi/shadow_target_test.cpp
  rhi/hdr_target_test.cpp
  resource/gltf_test.cpp
  resource/ktx2_test.cpp
  resource/ibl_cache_test.cpp
  resource/mesh_utils_test.cpp
  resource/primitives_test.cpp
  scene/scene_test.cpp
  scene/orbit_test.cpp
  scene/animator_test.cpp
  scene/picking_test.cpp
  renderer/renderer_test.cpp
  renderer/box_render_test.cpp
  renderer/pbr_test.cpp
  renderer/environment_test.cpp
  renderer/prefilter_test.cpp
  renderer/quality_test.cpp
  renderer/ktx2_render_test.cpp
  renderer/shadow_test.cpp
  renderer/post_test.cpp
  renderer/skinned_test.cpp
  renderer/demo_scenes_test.cpp
  ${CMAKE_SOURCE_DIR}/tools/render_test/scenes.cpp
  common/image.cpp
  common/image_test.cpp
  common/ktx2_gen.cpp
  common/golden_test.cpp
  common/skinned_gen.cpp
  common/shader_code.cpp
  rhi/cube_test.cpp
  rhi/swapchain_test.cpp
  rhi/texture_test.cpp
  rhi/texture_quad_test.cpp
  api/api_test.cpp
  api/embedded_test.cpp
  api/replay_test.cpp
  api/options_test.cpp
  api/command_bus_test.cpp
)
target_link_libraries(rd_tests PRIVATE rd_core GTest::gtest_main ktx)
target_include_directories(rd_tests PRIVATE
  ${CMAKE_BINARY_DIR}/generated
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}
  ${stb_SOURCE_DIR}
  ${ktx_SOURCE_DIR}/include)
target_compile_definitions(rd_tests PRIVATE RD_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(rd_tests PROPERTIES
  BUILD_RPATH "/opt/homebrew/lib;/usr/local/lib;$ENV{VULKAN_SDK}/lib")
gtest_discover_tests(rd_tests)

# perf_test 冒烟:8 帧无门槛,验证不崩 + 输出 4 场景行
# (ctest 正则不支持 \s\S;PASS_REGULAR_EXPRESSION 列表语义=每个模式都须匹配)
add_test(NAME perf_smoke_metal
         COMMAND $<TARGET_FILE:perf_test> --backend metal --frames 8 --no-gate)
set_tests_properties(perf_smoke_metal PROPERTIES PASS_REGULAR_EXPRESSION
  "\\[bench\\] helmet_high;\\[bench\\] items_64")
if(RD_HAVE_VULKAN)
  add_test(NAME perf_smoke_vulkan
           COMMAND $<TARGET_FILE:perf_test> --backend vulkan --frames 8 --no-gate)
  set_tests_properties(perf_smoke_vulkan PROPERTIES PASS_REGULAR_EXPRESSION
    "\\[bench\\] helmet_high;\\[bench\\] items_64")
endif()

# IBL 缓存双跑:miss 写缓存 → hit 命中 → 两图字节一致(同后端同参数确定性)
set(RD_IBL_CACHE_DIR ${CMAKE_BINARY_DIR}/test_ibl_cache)
add_test(NAME ibl_cache_miss
         COMMAND $<TARGET_FILE:render_test> --backend metal --pbr
                 --model ${CMAKE_CURRENT_SOURCE_DIR}/assets/DamagedHelmet.glb
                 --cache-dir ${RD_IBL_CACHE_DIR} --out ${CMAKE_BINARY_DIR}/ibl_miss.png)
add_test(NAME ibl_cache_hit
         COMMAND $<TARGET_FILE:render_test> --backend metal --pbr
                 --model ${CMAKE_CURRENT_SOURCE_DIR}/assets/DamagedHelmet.glb
                 --cache-dir ${RD_IBL_CACHE_DIR} --out ${CMAKE_BINARY_DIR}/ibl_hit.png)
set_tests_properties(ibl_cache_hit PROPERTIES DEPENDS ibl_cache_miss
                     PASS_REGULAR_EXPRESSION "IBL 缓存命中")
add_test(NAME ibl_cache_equal
         COMMAND ${CMAKE_COMMAND} -E compare_files ${CMAKE_BINARY_DIR}/ibl_miss.png
                 ${CMAKE_BINARY_DIR}/ibl_hit.png)
set_tests_properties(ibl_cache_equal PROPERTIES DEPENDS ibl_cache_hit)

# pipeline 缓存双跑:miss 落盘 → hit 装载 → 两图字节一致
foreach(b IN ITEMS metal vulkan)
  if(b STREQUAL "vulkan" AND NOT RD_HAVE_VULKAN)
    continue()
  endif()
  set(d ${CMAKE_BINARY_DIR}/test_pipeline_cache_${b})
  add_test(NAME pipeline_cache_miss_${b}
           COMMAND $<TARGET_FILE:render_test> --backend ${b} --pbr
                   --model ${CMAKE_CURRENT_SOURCE_DIR}/assets/DamagedHelmet.glb
                   --cache-dir ${d} --out ${CMAKE_BINARY_DIR}/pc_miss_${b}.png)
  add_test(NAME pipeline_cache_hit_${b}
           COMMAND $<TARGET_FILE:render_test> --backend ${b} --pbr
                   --model ${CMAKE_CURRENT_SOURCE_DIR}/assets/DamagedHelmet.glb
                   --cache-dir ${d} --out ${CMAKE_BINARY_DIR}/pc_hit_${b}.png)
  set_tests_properties(pipeline_cache_hit_${b} PROPERTIES DEPENDS pipeline_cache_miss_${b}
                       PASS_REGULAR_EXPRESSION "管线缓存装载")
  add_test(NAME pipeline_cache_equal_${b}
           COMMAND ${CMAKE_COMMAND} -E compare_files ${CMAKE_BINARY_DIR}/pc_miss_${b}.png
                   ${CMAKE_BINARY_DIR}/pc_hit_${b}.png)
  set_tests_properties(pipeline_cache_equal_${b} PROPERTIES DEPENDS pipeline_cache_hit_${b})
endforeach()

# 命令脚本冒烟:交互模式加载脚本(30 帧自动退出),错误输出即失败
file(WRITE ${CMAKE_BINARY_DIR}/smoke.rds "# 冒烟脚本\nset render.exposure 1.5\ntoggle quality.fxaa\n")
add_test(NAME command_script_smoke
         COMMAND $<TARGET_FILE:render_test> --interactive --scene cornell_box
                 --script ${CMAKE_BINARY_DIR}/smoke.rds)
set_tests_properties(command_script_smoke PROPERTIES
  ENVIRONMENT "RD_INTERACTIVE_FRAMES=30"
  FAIL_REGULAR_EXPRESSION "脚本执行失败")

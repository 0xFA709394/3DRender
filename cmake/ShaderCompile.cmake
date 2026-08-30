# ============================================================================
# shader 离线编译管线：rd_compile_shader(<源文件相对 shaders/ 的路径>)
#
# 每个 GLSL 源的处理链与产物（构建目录 shaders_out/）：
#   glslang        .vert/.frag → <name>.spv        Vulkan 直接用
#   spirv-cross    .spv → <name>.metal             MSL 源（参考/调试用）
#                  .spv → <name>.gles              GLSL ES 3.0 源（GLES 运行期编译）
#                  .spv → <name>.json              反射信息（材质系统用，P1 消费）
#   xcrun metal    .metal → .air → <name>.metallib Metal 后端加载（仅 APPLE；
#                  macosx/iphoneos/iphonesimulator 三个 SDK 分别编译——
#                  真机与模拟器的 metallib 指令集不同，必须分开）
#
# 绑定约定的落实：spirv-cross 加 --msl-decoration-binding，使 SPIR-V 的
# binding 号直接映射为 MSL 的 [[buffer(N)]]/[[texture(N)]] 索引
# （texture slot N ↔ Metal texture/sampler(N+4) ↔ Vulkan binding(N+4)，见 rhi_types.h）。
# ============================================================================
set(RD_SHADER_OUT ${CMAKE_BINARY_DIR}/shaders_out)
file(MAKE_DIRECTORY ${RD_SHADER_OUT})
# 函数内 CMAKE_CURRENT_LIST_DIR 解析到调用方目录,顶层先捕获本目录
set(RD_CMAKE_DIR ${CMAKE_CURRENT_LIST_DIR})

function(rd_compile_shader SRC)
  get_filename_component(F ${SRC} NAME)
  set(IN ${CMAKE_CURRENT_SOURCE_DIR}/${SRC})
  set(SPV ${RD_SHADER_OUT}/${F}.spv)
  set(MSL ${RD_SHADER_OUT}/${F}.metal)
  set(AIR ${RD_SHADER_OUT}/${F}.air)
  set(METALLIB ${RD_SHADER_OUT}/${F}.metallib)
  set(GLES ${RD_SHADER_OUT}/${F}.gles)
  set(REFL ${RD_SHADER_OUT}/${F}.json)

  # 第一步：GLSL → SPIR-V（-V = Vulkan 语义）
  add_custom_command(
    OUTPUT ${SPV}
    COMMAND $<TARGET_FILE:glslang-standalone> -V ${IN} -o ${SPV}
    DEPENDS ${SRC} glslang-standalone
    COMMENT "glsl->spv ${F}")

  # --msl-decoration-binding：SPIR-V binding 直接映射为 MSL 绑定索引，
  # 落实绑定约定（texture slot N ↔ Metal texture/sampler(N+4) ↔ Vulkan binding(N+4)）。
  # Metal sampler 参数上限 0..15：binding 16..19（纹理槽 12..15）经
  # fixup_msl_samplers.cmake 折返借用空闲 sampler 0..3
  # （与 Metal 后端 bindTexture 的折返映射一致；texture(N+4)≤19 在上限 31 内）。
  add_custom_command(
    OUTPUT ${MSL} ${GLES} ${REFL}
    COMMAND $<TARGET_FILE:spirv-cross> ${SPV} --msl --msl-decoration-binding --output ${MSL}
    COMMAND ${CMAKE_COMMAND} -P ${RD_CMAKE_DIR}/fixup_msl_samplers.cmake ${MSL}
    COMMAND $<TARGET_FILE:spirv-cross> ${SPV} --version 300 --es --output ${GLES}
    COMMAND $<TARGET_FILE:spirv-cross> ${SPV} --reflect --output ${REFL}
    DEPENDS ${SPV} spirv-cross
    COMMENT "spv->msl/gles/reflect ${F}")

  if(APPLE)
    # metallib 三份：macOS host / iOS 真机 / iOS 模拟器（指令集不同，不可混用）
    set(IOS_METALLIB ${RD_SHADER_OUT}_ios/${F}.metallib)
    set(IOSSIM_METALLIB ${RD_SHADER_OUT}_iossim/${F}.metallib)
    add_custom_command(
      OUTPUT ${METALLIB}
      COMMAND xcrun -sdk macosx metal -c ${MSL} -o ${AIR}
      COMMAND xcrun -sdk macosx metallib ${AIR} -o ${METALLIB}
      DEPENDS ${MSL}
      COMMENT "msl->metallib ${F}")
    add_custom_command(
      OUTPUT ${IOS_METALLIB}
      COMMAND ${CMAKE_COMMAND} -E make_directory ${RD_SHADER_OUT}_ios
      COMMAND xcrun -sdk iphoneos metal -c ${MSL} -o ${RD_SHADER_OUT}_ios/${F}.air
      COMMAND xcrun -sdk iphoneos metallib ${RD_SHADER_OUT}_ios/${F}.air -o ${IOS_METALLIB}
      DEPENDS ${MSL}
      COMMENT "msl->metallib(ios) ${F}")
    add_custom_command(
      OUTPUT ${IOSSIM_METALLIB}
      COMMAND ${CMAKE_COMMAND} -E make_directory ${RD_SHADER_OUT}_iossim
      COMMAND xcrun -sdk iphonesimulator metal -c ${MSL} -o ${RD_SHADER_OUT}_iossim/${F}.air
      COMMAND xcrun -sdk iphonesimulator metallib ${RD_SHADER_OUT}_iossim/${F}.air -o ${IOSSIM_METALLIB}
      DEPENDS ${MSL}
      COMMENT "msl->metallib(iossim) ${F}")
    add_custom_target(shader_${F} ALL DEPENDS ${METALLIB} ${IOS_METALLIB} ${IOSSIM_METALLIB} ${GLES} ${REFL})
  else()
    add_custom_target(shader_${F} ALL DEPENDS ${MSL} ${GLES} ${REFL})
  endif()
endfunction()

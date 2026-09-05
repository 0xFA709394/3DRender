# MSL sampler 索引折返:smpMat(binding 23)→ sampler(0)。
# Metal sampler 参数上限 0..15;分离采样器模型下仅剩 smpMat(23)超限
# (combined cube/lut/shadow = 9..12 在限内;共享 index 0 空闲)。
# 与 core/rhi/backends/metal bindTexture 的共享映射一致。
if(NOT DEFINED CMAKE_ARGV3)
  message(FATAL_ERROR "用法: cmake -P fixup_msl_samplers.cmake <file.metal>")
endif()
if(NOT EXISTS "${CMAKE_ARGV3}")
  message(FATAL_ERROR "MSL 不存在: ${CMAKE_ARGV3}")
endif()
file(READ "${CMAKE_ARGV3}" MSL)
string(REPLACE "sampler(23)" "sampler(0)" MSL "${MSL}")
file(WRITE "${CMAKE_ARGV3}" "${MSL}")

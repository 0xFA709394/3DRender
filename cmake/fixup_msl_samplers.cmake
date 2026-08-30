# MSL sampler 索引折返:binding 16..19(纹理槽 12..15)→ 空闲 sampler 0..3。
# Metal sampler 参数上限 0..15(texture 上限 31,texture(N+4)≤19 不受限);
# 与 core/rhi/backends/metal bindTexture 的折返映射一致。
# 用法:cmake -P fixup_msl_samplers.cmake <path/to/shader.metal>
if(NOT DEFINED CMAKE_ARGV3)
  message(FATAL_ERROR "用法: cmake -P fixup_msl_samplers.cmake <file.metal>")
endif()
if(NOT EXISTS "${CMAKE_ARGV3}")
  message(FATAL_ERROR "MSL 不存在: ${CMAKE_ARGV3}")
endif()
file(READ "${CMAKE_ARGV3}" MSL)
string(REPLACE "sampler(16)" "sampler(0)" MSL "${MSL}")
string(REPLACE "sampler(17)" "sampler(1)" MSL "${MSL}")
string(REPLACE "sampler(18)" "sampler(2)" MSL "${MSL}")
string(REPLACE "sampler(19)" "sampler(3)" MSL "${MSL}")
file(WRITE "${CMAKE_ARGV3}" "${MSL}")

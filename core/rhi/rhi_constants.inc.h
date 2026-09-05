// 能力枚举单源定义(X-macro):被 rhi_capability.h/cpp 多处展开。
// 值语义:0 = 不支持;非 0 为级别(如 msaa=4 表示最大 4x)。
#ifndef RD_CAPABILITY
#define RD_CAPABILITY(name)
#endif
RD_CAPABILITY(max_texture_size)         // 2D 纹理像素上限
RD_CAPABILITY(max_texture_slots)        // 纹理槽数(绑定约定上限 19:slot0..18;GLES 真机普遍 32+)
RD_CAPABILITY(max_uniform_buffer_slots) // uniform 槽数(绑定约定上限 4)
RD_CAPABILITY(instancing)               // 实例化绘制
RD_CAPABILITY(msaa)                     // 最大 sample count(预留,P2 使用)
RD_CAPABILITY(depth_texture)            // 可采样深度附件(预留,P2 阴影)
RD_CAPABILITY(cube_render_target)       // 渲染到 cube 指定 face/mip(IBL 预滤波)
RD_CAPABILITY(generate_mipmap)          // 运行时 mip 生成
RD_CAPABILITY(anisotropy)               // 最大各向异性等级(0/1 = 不支持)
RD_CAPABILITY(texture_compression_astc) // ASTC 4x4 LDR 纹理(1=支持)
RD_CAPABILITY(texture_compression_etc2) // ETC2 RGBA 纹理(1=支持)
RD_CAPABILITY(hdr_render_target)        // R16F 渲染目标(1=支持;GLES 查 EXT)
#undef RD_CAPABILITY

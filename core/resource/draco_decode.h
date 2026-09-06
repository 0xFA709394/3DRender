/**
 * @file draco_decode.h
 * @brief KHR_draco_mesh_compression 就地解码:合成 cgltf accessor 链改写指针。
 * 解码内存归 DracoPrimBlock 持有(调用方保活到 cgltf_data 使用结束);
 * 无扩展返回 0(零开销);单 primitive 失败记日志跳过。
 */
#pragma once
#include <cgltf.h>
#include <memory>
#include <vector>

namespace rd {

/// 单 primitive 的解码产物:blob + 1 buffer + N view/accessor 链(下标 0=索引)。
/// 完整定义在此——持有方(loadGltf)的 unique_ptr 析构需要完整类型。
struct DracoPrimBlock {
  std::unique_ptr<uint8_t[]> blob;
  cgltf_buffer buffer{};
  std::vector<cgltf_buffer_view> views;
  std::vector<cgltf_accessor> accessors;             // 与 views 一一对应
  std::vector<const cgltf_accessor*> jsonSrc;        // accessors[k] 复制自哪个 JSON accessor
};

/// 解码 data 中全部带扩展的 primitives 并改写 prim.indices / attributes[].data
/// 指针到合成 accessor;blocks 持有全部内存。
size_t applyDracoDecoding(cgltf_data* data,
                          std::vector<std::unique_ptr<DracoPrimBlock>>& blocks);

} // namespace rd

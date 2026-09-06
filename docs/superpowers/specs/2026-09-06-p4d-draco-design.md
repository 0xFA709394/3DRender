# P4-D 设计:KHR_draco_mesh_compression(几何压缩解码 + 工具重打包)

日期:2026-09-06
状态:已确认(用户逐节审阅通过)
前置:总设计 docs/superpowers/specs/2026-08-09-mobile-3d-renderer-design.md;
P4-C 完成(docs/superpowers/specs/2026-09-05-p4c-morph-targets-design.md)

## 0. 目标与范围

里程碑 D(P4 渲染能力扩充第四批):glTF Draco 几何压缩。

1. **解码(加载链)**:glb/gltf 内 `KHR_draco_mesh_compression` 压缩 primitive
   的解码,加载期一次性展开,运行时渲染路径零改动。
2. **编码(工具链)**:`glb_ktx2 --draco` 对未压缩几何重打包(与 KTX2 纹理
   压缩正交叠加,包体双重收益)。

非目标:morph targets/TANGENT 压缩(spec 禁止)、Draco 比特流编码器进内核
(仅工具与测试链接)、点云/元数据。

**零回归硬约束**:无 draco 扩展的资产加载路径**零变化**(一行调用提前返回);
现有 golden 全量不动;新 golden 仅新增用例。

## 1. 依赖(cmake/Deps.cmake)

```cmake
# Draco:KHR_draco_mesh_compression 解码(内核)/编码(glb_ktx2 工具)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)  # 仅 draco 构建期(rd_core 消费)
if(EXISTS "$ENV{RD_DEPS_MIRROR}/draco/CMakeLists.txt")
  FetchContent_Declare(draco SOURCE_DIR $ENV{RD_DEPS_MIRROR}/draco)
else()
  FetchContent_Declare(draco
    URL https://github.com/google/draco/archive/refs/tags/1.5.7.tar.gz)
endif()
FetchContent_MakeAvailable(draco)
```

- 链接 `draco_static`;最终二进制 Android `-gc-sections`/Apple dead-strip
  剥离未引用对象(内核仅用解码器,工具/测试用编码器)。
- iOS:draco 若污染 `CMAKE_OSX_DEPLOYMENT_TARGET` 同 ktx 清理回 16.0。
- `glb_ktx2`(独立目标)与 `rd_core` 各自链接 draco_static。

## 2. 解码模块(core/resource/draco_decode.h/.cpp,新文件)

```cpp
namespace rd {
/// 就地解码 data 中带 KHR_draco_mesh_compression 的 primitives:
/// 合成指向解码内存的 cgltf_buffer/buffer_view/accessor 链并改写指针。
/// decodedStorage 持有解码内存(调用方保活到 cgltf_data 使用结束)。
/// 返回成功解码的 primitive 数;单个失败记日志跳过(该 primitive 后续
/// 在既有读取路径因 accessor 无数据自然跳过,不崩溃)。
size_t applyDracoDecoding(cgltf_data* data,
                          std::vector<std::unique_ptr<uint8_t[]>>& decodedStorage);
}
```

实现要点:

1. **解码**:extension 的 bufferView 内存 → `draco::Decoder::
   DecodeMeshFromBuffer`(不请求 edgbreaker 失败自动回落 sequential,
   `DecodeMeshFromBuffer` 内建两算法)。
2. **索引**:faces → 三元组序列,按 **JSON accessor 声明的 componentType**
   (5121/5123/5125)落盘;合成 accessor 复制 JSON 的 component_type/type/count。
3. **属性**:遍历 `prim.attributes`(glTF 语义),在扩展 attributes 列表按
   语义名找 unique_id → `mesh.GetAttributeByUniqueId`;逐 glTF 顶点
   `attr->mapped_index(draco::PointIndex(i))` → `ConvertValue<float>` 读出,
   按 **JSON accessor 声明的 componentType** 落盘(POSITION/TEXCOORD/WEIGHTS
   → float;JOINTS_0 5121/5123 → u8/u16;NORMAL → float)。合成 accessor 的
   component_type/type/count 逐一复制自 JSON accessor —— 既有
   `cgltf_accessor_read_uint/float/read_index` 消费零适配。
4. **内存**:每 primitive 一块连续 blob(4B 对齐),storage 持有;
   假 cgltf_buffer{data=blob} → 每 accessor 一个 cgltf_buffer_view{offset}
   → cgltf_accessor{buffer_view,component_type,type,count,byte_offset=0}。
5. **改写**:`prim.attributes[k].data = &synth;` `prim.indices = &synthIdx;`
   (extension 结构保留不动,仅指针替换)。
6. glTF loader 接线(gltf_loader.cpp,唯一改动):
   `cgltf_load_buffers` 之后、mesh 遍历之前一行
   `applyDracoDecoding(data, dracoStorage);`(局部 vector 保活到函数尾)。

## 3. glb_ktx2 --draco(工具)

- 参数:`--draco`(开关)+ 可选 `--qp N/--qn N/--qt N`(位置/法线/UV 量化
  bits,默认 14/10/12)。
- 未压缩 primitive:读原 accessor 原始数据 → `draco::Encoder`
  (设置 quantization)→ `EncodeMeshToBuffer` → 新 BIN 追加 bufferView;
  JSON 改写:该 primitive 的 attributes/indices 对应 accessor 剥
  `bufferView/byteOffset`(spec:数据移入扩展),primitive 加
  `extensions.KHR_draco_mesh_compression{bufferView, attributes:{语义:unique_id}}`,
  顶层 extensionsUsed/extensionsRequired 补条目。
- 已压缩 primitive:bufferView 原样透传(不二次压缩)。
- 与 KTX2 纹理重排共用 BIN 重建(两流程串行:纹理替换 → 几何追加)。
- 编码 Mesh 构造:draco::Mesh 从索引/逐属性 PointAttribute 填充
  (SetAttribute等标准 API),unique_id 自增分配。

## 4. 测试

1. **单测**(tests/resource/gltf_test.cpp 追加):
   - `tests/common/draco_gen`(编码器生成确定性 draco 球 glb,零二进制提交):
     原始球 → EncodeMeshToBuffer → glb JSON+BIN(带扩展);
   - 断言:loadGltf 解码后顶点数/索引数一致,顶点位置误差 ≤ 量化容差
     (14bit 位置量化球半径 0.5 → 容差 ~1e-4 量级,断言 1e-3 稳妥),
     法线存在、UV 存在;蒙皮属性(JOINTS/WEIGHTS)round-trip 正确
     (生成器带单骨蒙皮)。
2. **golden**:`draco_sphere` 双后端(生成器资产,SSIM 阈值 0.05;
   round-trip 确定性)。
3. **工具冒烟**(手动):`glb_ktx2 assets/Duck.glb --draco --ktx2 -o /tmp/duck_dk.glb`
   → loadGltf 成功 + 体积下降 + render_test 渲染正常。
4. 全量回归:无 draco 路径零变化(既有 249 用例即证)。

## 5. 错误处理与降级

- 解码失败(坏流/属性缺失):该 primitive 跳过 + RD_LOGW;模型其余部分照常。
- 扩展声明但 `extensionsRequired` 未含且解码失败:同上(宽松处理)。
- 工具编码失败:该 primitive 保持原样(未压缩透传)+ 告警。
- 依赖拉取失败:FetchContent 报错即构建失败(与 ktx 同语义,无静默降级)。

## 6. 已知限制

- 压缩 primitive 不支持 morph targets/TANGENT(spec 禁止;切线由现有
  computeTangents 后算)。
- 解码为加载期阻塞(大模型 ~几十 ms 量级,移动端可接受;异步加载线程
  已存在——load_gltf_async 天然受益)。
- unique_id 映射仅按 glTF 语义字符串匹配(POSITION/NORMAL/TEXCOORD_n/
  JOINTS_n/WEIGHTS_n);其余语义忽略。

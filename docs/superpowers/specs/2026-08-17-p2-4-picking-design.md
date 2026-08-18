# P2-4 设计:拾取交互(射线三角形精确拾取)

日期:2026-08-17
状态:已确认(用户审阅通过)
前置:P2-3(骨骼动画)已完成

## 0. 目标与范围

P2 第四个子项目:点击拾取(电商"点选商品"交互闭环)。

1. **三角形精确拾取**:Möller–Trumbore 射线求交(CPU 顶点数据,包围球预筛);
   同步返回结构体(mesh 下标/名称/距离/命中点)。
2. 蒙皮模型按**绑定姿态**拾取(动画姿态拾取归 P4;记录为已知限制)。

非目标:GPU ID 缓冲拾取、动画姿态精确拾取、多选/框选、拾取回调事件系统。

## 1. 核心模块 core/scene/picking.{h,cpp}(纯 CPU)

```cpp
namespace rd::scene {
struct PickResult {
  bool hit = false;
  int32_t meshIndex = -1;    // meshes[] 下标
  float distance = 0;        // 沿射线距离
  float point[3] = {};       // 世界命中点
};
/// 屏幕像素坐标 → 世界射线(逆 viewProj;屏幕 y 向下,NDC y 向上翻转)。
void screenRay(const Camera& cam, float px, float py, float vpW, float vpH,
               float outOrigin[3], float outDir[3]);
/// 射线与 ModelAsset 逐三角形求交(包围球预筛,world 逆变换入模型空间;
/// 48B/80B 布局兼容;蒙皮按绑定姿态)。最近命中胜出。
PickResult pickModel(const ModelAsset& model, const math::Mat4& world,
                     const float origin[3], const float dir[3]);
}
```

- 包围球随 pick 调用随算(低频操作,不预计算)。
- 射线方向不要求归一化(函数内归一,距离按归一化射线计)。

## 2. C API + engine

```c
typedef struct rd_pick_result {
  int32_t hit;          // 0/1
  int32_t mesh_index;   // meshes[] 下标(-1=未中)
  float distance;       // 命中距离
  float px, py, pz;     // 命中点(世界)
  char mesh_name[64];   // mesh 名(截断 63B)
} rd_pick_result_t;
/// 像素坐标(与输入事件同一坐标系);无模型/未命中返回 hit=0。
rd_pick_result_t rd_engine_pick(rd_engine* engine, float x, float y);
```

- engine 用当前相机(Orbit 驱动)+ 持久 modelAsset;线程约定同 render_frame。
- 蒙皮模型:world 用单位阵(绑定姿态拾取)。

## 3. 错误处理

- 空引擎/无模型:返回 hit=0 的零值结构体。
- 尺寸为 0 视口:hit=0。
- 退化三角形(零面积):跳过。

## 4. 测试与验证

| 层 | 内容 |
|---|---|
| 单测 | screenRay 中心前向/边角方向;双三角形模型(命中/未中/最近胜出);skinned_quad 绑定姿态拾取 |
| API | load TetraU32 → 中心像素命中;空引擎安全;无模型 hit=0 |
| 回归 | 纯新增模块,既有全量不变 |

## 5. 实施顺序

1. picking 模块 + 单测
2. C API + engine 集成 + api 测试
3. AGENTS.md 收尾

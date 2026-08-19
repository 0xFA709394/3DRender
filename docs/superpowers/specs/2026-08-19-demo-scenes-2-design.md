# 场景库扩充 设计(6 新场景)

日期:2026-08-19
状态:已确认(用户审阅通过)
前置:场景示例集合(primitives + 5 程序场景 + 2 知名场景)已完成

## 0. 目标与范围

再增 6 个常见 3D 演示场景:emissive_bloom / normal_map_wall / shadow_gallery /
ktx2_gallery(纯程序)+ alpha_blend(含引擎混合支持小改)+ fox_anim(资产下载)。

## 1. 纯程序场景(4 个,均入 golden 双后端)

| 场景 | 内容 | 展示点 |
|---|---|---|
| `emissive_bloom` | 4×4 自发光球阵(emissiveFactor 2~8 递增) | HDR Bloom(P2-2) |
| `normal_map_wall` | 砖墙平面(运行时程序化生成法线贴图)+ 斜向方向光 | 法线贴图/切线 |
| `shadow_gallery` | 双高差平面 + 悬空盒,斜向方向光 + 阴影取景 | PCF 软边/阴影 |
| `ktx2_gallery` | 左右双 quad:左 PNG、右 KTX2(运行时渐变图现场编码转码) | KTX2 压缩纹理 |

- `normal_map_wall` 的法线贴图在 scenes 模块内程序化生成(砖行错缝+灰浆凹槽→
  高度场→法线),RGBA8 直接上传 ImageData,材质 normalScale=1。
- `ktx2_gallery`:渐变图生成 → 左经 stb(PNG 路径)、右经 ktx2_gen 同源
  libktx 编码 → decodeKtx2(按 caps 转码目标)上传。

## 2. alpha_blend(引擎小改)

- **loader**:glTF `alphaMode` 解析(`MaterialData.alphaBlend`,BLEND 置位;MASK 暂不支持,记警告按 OPAQUE)。
- **renderer**:alphaBlend 材质绑混合管线(BlendDesc{srcAlpha, oneMinusSrcAlpha},
  depthTest 开 depthWrite 关);管线按 (fmt, samples, blend) 变体随 ensureScenePipelines
  一并创建(后端管线缓存兜底);**endScene 排序**:opaque 先、blend 后按视距远→近。
- 场景 `alpha_blend`:前排 3 块彩色玻璃板(alpha 0.3/0.5/0.8)+ 后排彩色盒,
  golden 双后端。
- 非 alphaMode 材质路径零变化,既有 golden 不回归。

## 3. fox_anim(资产)

- `scripts/fetch_assets.sh` 追加 Fox.glb(glTF-Binary 存在性执行验证;404 则回退
  glTF-Embedded 或外链形式);`fox_anim` 场景自动播放 clip0;冒烟(资产缺失 skip)。

## 4. 实施顺序

1. 4 纯程序场景 + golden(emissive/normal/shadow/ktx2)
2. alpha_blend:loader alphaMode + renderer 混合管线 + 排序 + 场景 golden
3. fox_anim 接入
4. AGENTS.md 收尾

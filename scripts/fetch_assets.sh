#!/bin/bash
# 知名示例资产下载(Khronos glTF-Sample-Assets;失败不阻塞,可重跑)。
set -u
cd "$(dirname "$0")/.."
mkdir -p assets
BASE=https://github.com/KhronosGroup/glTF-Sample-Assets/raw/main/Models
dl() {  # dl <本地名> <远端路径>
  if [ -f "assets/$1" ]; then echo "已存在 assets/$1"; return 0; fi
  echo "下载 $1 ..."
  curl -fL --retry 2 --connect-timeout 10 --max-time 300 -o "assets/$1" "$BASE/$2" \
    && echo "OK $1" || { echo "失败 $1(弱网可重跑)"; return 1; }
}
dl CesiumMan.glb CesiumMan/glTF-Binary/CesiumMan.glb
dl Fox.glb Fox/glTF-Binary/Fox.glb
# P4 打磨新增(iOS demo 模型集;均验证可渲染)
dl Duck.glb Duck/glTF-Binary/Duck.glb
dl BoomBox.glb BoomBox/glTF-Binary/BoomBox.glb
dl WaterBottle.glb WaterBottle/glTF-Binary/WaterBottle.glb
dl Corset.glb Corset/glTF-Binary/Corset.glb
dl Lantern.glb Lantern/glTF-Binary/Lantern.glb

# P4-A KHR 材质扩展 golden 模型(clearcoat/sheen/specular)
dl ClearCoatTest.glb ClearCoatTest/glTF-Binary/ClearCoatTest.glb
dl SheenChair.glb SheenChair/glTF-Binary/SheenChair.glb
dl SpecularTest.glb SpecularTest/glTF-Binary/SpecularTest.glb

# P4-B KHR transmission/volume golden 模型
dl TransmissionTest.glb TransmissionTest/glTF-Binary/TransmissionTest.glb
dl TransmissionRoughnessTest.glb TransmissionRoughnessTest/glTF-Binary/TransmissionRoughnessTest.glb
dl AttenuationTest.glb AttenuationTest/glTF-Binary/AttenuationTest.glb
dl MosquitoInAmber.glb MosquitoInAmber/glTF-Binary/MosquitoInAmber.glb

# Sponza(无 glb 形态):glTF 主文件 + 全部外链纹理到 assets/sponza/
if [ -f assets/sponza/Sponza.gltf ]; then
  echo "已存在 assets/sponza/Sponza.gltf"
else
  echo "下载 Sponza(gltf + 外链纹理)..."
  mkdir -p assets/sponza
  FILES=$(curl -s --connect-timeout 10 --max-time 60 \
    "https://api.github.com/repos/KhronosGroup/glTF-Sample-Assets/contents/Models/Sponza/glTF" \
    | python3 -c "import json,sys;[print(it['name']) for it in json.load(sys.stdin) if it['type']=='file']")
  OK=1
  for name in $FILES; do
    [ -f "assets/sponza/$name" ] && continue
    curl -fL --retry 2 --connect-timeout 10 --max-time 120 -o "assets/sponza/$name" \
      "$BASE/Sponza/glTF/$name" > /dev/null 2>&1 || { OK=0; echo "  失败 $name"; }
  done
  [ $OK = 1 ] && echo "OK Sponza(assets/sponza/)" || echo "Sponza 部分失败(可重跑)"
fi
echo "完成(缺失项可稍后重跑本脚本)"

enable_language(Swift)

# 演示模型资产(bundle Resources;源码树内复用 tests 资产,勿重复存储)
set(DEMO_ASSET ${CMAKE_SOURCE_DIR}/tests/assets/DamagedHelmet.glb)
# 可选模型集(scripts/fetch_assets.sh 下载后自动入包;不入库)
set(DEMO_OPTIONAL CesiumMan Fox Duck BoomBox WaterBottle Corset Lantern)
add_executable(RdDemo MACOSX_BUNDLE
  RdDemo/AppDelegate.swift
  RdDemo/dummy.cpp
  ${CMAKE_SOURCE_DIR}/platform/ios/RenderView.swift
  ${DEMO_ASSET}
)
set_source_files_properties(${DEMO_ASSET} PROPERTIES
  MACOSX_PACKAGE_LOCATION Resources)
foreach(m ${DEMO_OPTIONAL})
  # KTX2 压缩版优先(glb_ktx2 产物,包体积减半);缺失回退原 glb
  set(f ${CMAKE_SOURCE_DIR}/assets/${m}.ktx2.glb)
  if(NOT EXISTS ${f})
    set(f ${CMAKE_SOURCE_DIR}/assets/${m}.glb)
  endif()
  if(EXISTS ${f})
    target_sources(RdDemo PRIVATE ${f})
    set_source_files_properties(${f} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
  endif()
endforeach()
target_link_libraries(RdDemo PRIVATE rd_core "-framework UIKit" "-framework QuartzCore")
set_target_properties(RdDemo PROPERTIES
  MACOSX_BUNDLE_BUNDLE_NAME RdDemo
  MACOSX_BUNDLE_GUI_IDENTIFIER com.rd.demo
  MACOSX_BUNDLE_BUNDLE_VERSION 1
  MACOSX_BUNDLE_SHORT_VERSION_STRING 1.0
  MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/RdDemo/Info.plist
  XCODE_ATTRIBUTE_SWIFT_OBJC_BRIDGING_HEADER
    ${CMAKE_CURRENT_SOURCE_DIR}/RdDemo/RdDemo-Bridging-Header.h
  XCODE_ATTRIBUTE_SWIFT_VERSION 5.0
  XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED NO
  XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
  XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER com.rd.demo
  # Swift 编译也须 ≥13(CAMetalLayer 的 Swift availability);不显式设置时
  # Xcode 生成器可能回落到 11.0 导致编译失败
  XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET 13.0
)

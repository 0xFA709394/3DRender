add_executable(img_check
  main.cpp
  ${CMAKE_SOURCE_DIR}/tests/common/image.cpp
)
target_link_libraries(img_check PRIVATE rd_core)
target_include_directories(img_check PRIVATE
  ${CMAKE_SOURCE_DIR}/tests
  ${stb_SOURCE_DIR})

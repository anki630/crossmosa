# Formosa Cover（2026-10-07）：書架索引（ShelfIndex）、書名排版（CoverTitleLayout）、歐風底框資料（CoverFrames.h 能解、尺寸對、書名區全白）。
add_executable(FormosaCoverTest
  FormosaCoverTest.cpp
  ${REPO_ROOT}/lib/InflateReader/InflateReader.cpp
  ${REPO_ROOT}/lib/uzlib/src/tinflate.c
  ${REPO_ROOT}/lib/EpdFont/EpdFont.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)
target_include_directories(FormosaCoverTest PRIVATE
  ${REPO_ROOT}/src
  ${REPO_ROOT}/lib/InflateReader
  ${REPO_ROOT}/lib/uzlib/src
  ${REPO_ROOT}
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/Utf8
)
target_link_libraries(FormosaCoverTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)
target_compile_options(FormosaCoverTest PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
target_link_options(FormosaCoverTest PRIVATE -fsanitize=address,undefined)
gtest_discover_tests(FormosaCoverTest)

# 閱讀統計・第一步（2026-10-07）
# ReadingStatsTest：src/util/ReadingStatsCore.h 的純邏輯（計時、上限、歸日、最近 10 本、序列化三態）。
# ReadingStatsGlueTest：編【韌體的同一個】src/util/ReadingStats.cpp，NVS／時鐘／設定／log 換成 stubs/ 的替身
#   （codex 程式碼複查：開機三態讀取、禁止覆寫較新版本、寫失敗重試、身分雜湊契約、換卡、搬書即存都要有測試）。
add_executable(ReadingStatsTest
  ReadingStatsTest.cpp
)
target_link_libraries(ReadingStatsTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)
target_compile_options(ReadingStatsTest PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
target_link_options(ReadingStatsTest PRIVATE -fsanitize=address,undefined)
gtest_discover_tests(ReadingStatsTest)

add_executable(ReadingStatsGlueTest
  ReadingStatsGlueTest.cpp
  ${REPO_ROOT}/src/util/ReadingStats.cpp
)
target_include_directories(ReadingStatsGlueTest BEFORE PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/src
)
target_compile_definitions(ReadingStatsGlueTest PRIVATE READING_STATS_HOST_TEST=1)
target_link_libraries(ReadingStatsGlueTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)
target_compile_options(ReadingStatsGlueTest PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
target_link_options(ReadingStatsGlueTest PRIVATE -fsanitize=address,undefined)
gtest_discover_tests(ReadingStatsGlueTest)

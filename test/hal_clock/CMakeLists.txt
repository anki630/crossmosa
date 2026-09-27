# v343（帳本 B12）：狀態列時鐘的整合測試 —— 編【韌體的同一個】lib/hal/HalClock.cpp，Arduino／SDK Rtc／WiFi／SNTP／
# SdFat 時戳換成 stubs/ 的替身（語意照 SDK 的 DS3231 那一支，見 stubs/FakeClockHw.h）。
# HalClock.cpp 用 time(nullptr) 拿 NTP 的時間 → 測試執行檔自己定義 time()（連結時執行檔的定義優先；GCC 的 libasan 是共享函式庫）。
add_executable(HalClockTest
  HalClockTest.cpp
  ${REPO_ROOT}/lib/hal/HalClock.cpp
)

target_include_directories(HalClockTest BEFORE PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/lib
  ${REPO_ROOT}/lib/hal
)

target_link_libraries(HalClockTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)
target_compile_options(HalClockTest PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
target_link_options(HalClockTest PRIVATE -fsanitize=address,undefined)

gtest_discover_tests(HalClockTest)

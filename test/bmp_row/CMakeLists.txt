# v387：1-bit BMP 列快轉跟逐點版逐位元組相同（lib/GfxRenderer/BmpRow1Bit.h）。
add_executable(BmpRow1BitTest
  BmpRow1BitTest.cpp
)
target_include_directories(BmpRow1BitTest PRIVATE ${REPO_ROOT})
target_link_libraries(BmpRow1BitTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)
target_compile_options(BmpRow1BitTest PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
target_link_options(BmpRow1BitTest PRIVATE -fsanitize=address,undefined)
gtest_discover_tests(BmpRow1BitTest)

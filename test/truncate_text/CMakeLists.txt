add_executable(TruncateTextTest
  TruncateTextTest.cpp
)

target_link_libraries(TruncateTextTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(TruncateTextTest)

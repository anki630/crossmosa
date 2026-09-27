add_executable(HintPillLayoutTest
  HintPillLayoutTest.cpp
)

target_link_libraries(HintPillLayoutTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(HintPillLayoutTest)

add_executable(TabSegmentLayoutTest
  TabSegmentLayoutTest.cpp
)

target_link_libraries(TabSegmentLayoutTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(TabSegmentLayoutTest)

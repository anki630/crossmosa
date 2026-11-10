add_executable(GridCenterTest
  GridCenterTest.cpp
)

target_link_libraries(GridCenterTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(GridCenterTest)

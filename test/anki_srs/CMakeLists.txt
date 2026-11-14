add_executable(AnkiSrsTest
  AnkiSrsTest.cpp
)

target_link_libraries(AnkiSrsTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(AnkiSrsTest)

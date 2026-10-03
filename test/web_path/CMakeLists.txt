add_executable(WebPathTest
  WebPathTest.cpp
  ${REPO_ROOT}/src/util/WebPath.cpp
  ${REPO_ROOT}/src/util/ProtectedPath.cpp
)

target_include_directories(WebPathTest PRIVATE
  ${REPO_ROOT}/src/util
)

target_link_libraries(WebPathTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(WebPathTest)

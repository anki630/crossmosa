# v345（帳本 D14）：XML 不准的控制字元在餵 expat 之前濾掉（XmlParserUtils.h 的 XmlControlCharFilter）。
#   用真的 lib/expat 驗「不濾會被拒收、濾了會接受」，以及 UTF-16 文件原封不動。
add_library(xml_control_filter_expat STATIC
  ${REPO_ROOT}/lib/expat/xmlparse.c
  ${REPO_ROOT}/lib/expat/xmlrole.c
  ${REPO_ROOT}/lib/expat/xmltok.c
)
target_include_directories(xml_control_filter_expat PUBLIC ${REPO_ROOT}/lib/expat)
target_compile_definitions(xml_control_filter_expat PUBLIC XML_GE=0 XML_CONTEXT_BYTES=1024 HAVE_EXPAT_CONFIG_H)

add_executable(XmlControlFilterTest XmlControlFilterTest.cpp)
target_include_directories(XmlControlFilterTest PRIVATE ${REPO_ROOT}/lib/XmlParserUtils)
target_link_libraries(XmlControlFilterTest PRIVATE
  crosspoint_test_common
  xml_control_filter_expat
  GTest::gtest_main
)
gtest_discover_tests(XmlControlFilterTest)

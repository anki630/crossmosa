// v361：網頁檔案管理的路徑整理（src/util/WebPath）。規則的出處是 SdFat 的 FatFile::parsePathName()：
//   一段路徑去掉開頭空白、結尾的點與空白之後是空的，SdFat 就開不起來 —— 我們在那之前就拒絕，
//   而且【不】像上游 normalisePath() 那樣把 ".." 解成「上一層」。
#include <gtest/gtest.h>

#include <string>

#include "ProtectedPath.h"
#include "WebPath.h"

namespace {

std::string canon(const std::string& in) {
  std::string out = "<untouched>";
  return WebPath::canonicalize(in, out) ? out : "REJECT";
}

TEST(WebPathCanonicalize, RootForms) {
  EXPECT_EQ(canon(""), "/");
  EXPECT_EQ(canon("/"), "/");
  EXPECT_EQ(canon("//"), "/");
}

TEST(WebPathCanonicalize, LeadingSlashAddedTrailingRemovedRunsMerged) {
  EXPECT_EQ(canon("books"), "/books");
  EXPECT_EQ(canon("/books/"), "/books");
  EXPECT_EQ(canon("/books//sub///x.epub"), "/books/sub/x.epub");
}

TEST(WebPathCanonicalize, OrdinaryNamesKeptVerbatim) {
  EXPECT_EQ(canon("/a b/c d.epub"), "/a b/c d.epub");
  EXPECT_EQ(canon("/volume..2.epub"), "/volume..2.epub");
  EXPECT_EQ(canon("/notes...txt"), "/notes...txt");
  EXPECT_EQ(canon("/書/範例書坊 第一冊.epub"), "/書/範例書坊 第一冊.epub");
  // 點開頭是真名字：能不能碰由 ProtectedPath 決定，不是這裡。
  EXPECT_EQ(canon("/.crossmosa/diag.log"), "/.crossmosa/diag.log");
  // SdFat 會把結尾的點去掉後開到 x.epub —— 這裡不改寫，ProtectedPath 與診斷檔的精確比對都是 fail-closed。
  EXPECT_EQ(canon("/books/x.epub."), "/books/x.epub.");
}

TEST(WebPathCanonicalize, SegmentsSdFatCannotNameAreRejected) {
  for (const char* p : {"/.", "/..", "/books/.", "/books/..", "/../x", "/books/../x", "/books/...", "/books/ .. /x",
                        "/books/ . ", "/ /books", " /books", "/books/ "}) {
    EXPECT_EQ(canon(p), "REJECT") << p;
  }
}

TEST(WebPathCanonicalize, BackslashAndNulRejected) {
  EXPECT_EQ(canon("/books\\x.epub"), "REJECT");
  EXPECT_EQ(canon("\\"), "REJECT");
  EXPECT_EQ(canon(std::string("/books/x\0y.epub", 15)), "REJECT");
}

TEST(WebPathCanonicalize, RejectLeavesOutputUntouched) {
  std::string out = "keep";
  EXPECT_FALSE(WebPath::canonicalize("/a/../b", out));
  EXPECT_EQ(out, "keep");
}

// 整理只合併空段，不能讓一條受保護的路徑變成不受保護（或反過來）。
TEST(WebPathCanonicalize, ProtectionVerdictUnchanged) {
  for (const char* p :
       {"/.crossmosa//wifi.json", "//.crossmosa/settings.json", "/books/ .crossmosa/x", "/CROSSM~1/wifi.json",
        "/XTCache /a", "/System Volume Information/", "/books/x.epub", "/books//sub/", "/.hidden/", "/a/.b/c"}) {
    std::string out;
    ASSERT_TRUE(WebPath::canonicalize(p, out)) << p;
    EXPECT_EQ(ProtectedPath::isProtected(out.c_str()), ProtectedPath::isProtected(p)) << p << " -> " << out;
  }
  std::string out;
  ASSERT_TRUE(WebPath::canonicalize("/.crossmosa//wifi.json", out));
  EXPECT_TRUE(ProtectedPath::isProtected(out.c_str()));
}

TEST(WebPathComponent, AcceptsOrdinaryNames) {
  for (const char* n : {"x.epub", "範例書坊.epub", "a b", "volume..2.epub", "notes...txt", ".hidden", "x.epub."}) {
    EXPECT_TRUE(WebPath::isSafeComponent(n)) << n;
  }
}

TEST(WebPathComponent, RejectsSeparatorsAndUnnameable) {
  for (const char* n : {"", ".", "..", "...", " ", " .. ", "a/b", "/x", "x/", "a\\b", "..\\x"}) {
    EXPECT_FALSE(WebPath::isSafeComponent(n)) << '[' << n << ']';
  }
  EXPECT_FALSE(WebPath::isSafeComponent(std::string("x\0y", 3)));
}

}  // namespace

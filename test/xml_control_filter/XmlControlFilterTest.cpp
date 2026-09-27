// v345（帳本 D14）：XmlControlCharFilter —— XML 1.0 不准的控制字元在餵 expat 之前濾掉。
// 實例（a field report）：一本 EPUB 的 OPF 作者名中間夾了 0x08 → expat 把整份 OPF 判成不合法 → 開書回首頁。
#include <XmlParserUtils.h>
#include <gtest/gtest.h>

#include <string>

namespace {

std::string filtered(XmlControlCharFilter& f, std::string s) {
  s.resize(f.apply(s.data(), s.size()));
  return s;
}

struct Parsed {
  bool ok = false;
  std::string text;  // 所有字元資料（expat 一律交出 UTF-8）
};

void XMLCALL onChars(void* ud, const XML_Char* s, int len) { static_cast<Parsed*>(ud)->text.append(s, len); }

// 照韌體的餵法：XML_GetBuffer → memcpy → （濾）→ XML_ParseBuffer，一次 chunk 位元組。
Parsed parse(const std::string& doc, const bool useFilter, const size_t chunk) {
  Parsed out;
  XML_Parser p = XML_ParserCreate(nullptr);
  XML_SetUserData(p, &out);
  XML_SetCharacterDataHandler(p, onChars);
  XmlControlCharFilter filter;
  size_t off = 0;
  out.ok = true;
  do {
    const size_t take = doc.size() - off < chunk ? doc.size() - off : chunk;
    void* buf = XML_GetBuffer(p, static_cast<int>(take ? take : 1));
    memcpy(buf, doc.data() + off, take);
    const size_t fed = useFilter ? filter.apply(static_cast<char*>(buf), take) : take;
    off += take;
    if (XML_ParseBuffer(p, static_cast<int>(fed), off == doc.size()) == XML_STATUS_ERROR) {
      out.ok = false;
      break;
    }
  } while (off < doc.size());
  XML_ParserFree(p);
  return out;
}

const std::string kOpf =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<package><metadata><creator>\xE7\x8E\x8B\xE5\xB0\x8F\x08\xE6\x98\x8E"
    "</creator></metadata></package>";

}  // namespace

TEST(XmlControlFilter, CleanUtf8IsByteIdentical) {
  XmlControlCharFilter f;
  takeXmlControlDrops();
  const std::string s = "<p>\xE5\x93\x88\xE5\x88\xA9 Harry\t\n\r</p>";  // 哈利、TAB、LF、CR 都合法
  EXPECT_EQ(filtered(f, s), s);
  EXPECT_EQ(takeXmlControlDrops(), 0u);
}

TEST(XmlControlFilter, DropsIllegalControlsAndKeepsTabLfCr) {
  XmlControlCharFilter f;
  takeXmlControlDrops();
  std::string s = "<a>";
  for (int c = 0; c < 0x20; c++) s.push_back(static_cast<char>(c));
  s += "\x7F</a>";  // DEL 在 XML 1.0 合法，不動
  EXPECT_EQ(filtered(f, s), std::string("<a>\t\n\r\x7F</a>"));
  EXPECT_EQ(takeXmlControlDrops(), 29u);  // 32 − TAB − LF − CR
}

TEST(XmlControlFilter, ExpatRejectsTheBookWithoutFilterAndAcceptsWithIt) {
  for (const size_t chunk : {1, 7, 1024}) {
    EXPECT_FALSE(parse(kOpf, false, chunk).ok) << "chunk=" << chunk;
    const Parsed with = parse(kOpf, true, chunk);
    ASSERT_TRUE(with.ok) << "chunk=" << chunk;
    EXPECT_EQ(with.text, "\xE7\x8E\x8B\xE5\xB0\x8F\xE6\x98\x8E") << "chunk=" << chunk;  // 王小明
  }
}

TEST(XmlControlFilter, Utf16DocumentsAreNotTouched) {
  // U+4E08「丈」在 UTF-16LE 是 08 4E —— 低位元組正好是 0x08，濾掉就把字弄壞。
  const std::string le = std::string("\xFF\xFE<\0a\0>\0\x08\x4E<\0/\0a\0>\0", 18);
  const std::string be = std::string("\xFE\xFF\0<\0a\0>\x4E\x08\0<\0/\0a\0>", 18);
  const std::string leNoBom = le.substr(2);
  const std::string beNoBom = be.substr(2);
  for (const auto* doc : {&le, &be, &leNoBom, &beNoBom}) {
    XmlControlCharFilter f;
    EXPECT_EQ(filtered(f, *doc), *doc);
  }
  for (const auto* doc : {&le, &be}) {
    const Parsed r = parse(*doc, true, 1024);
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.text, "\xE4\xB8\x88");  // 丈
  }
}

TEST(XmlControlFilter, Utf16MarkSplitAcrossTheFirstTwoChunksIsStillDetected) {
  // 第一塊只有 1 個位元組：先原樣交出、記住它，下一塊的第一個位元組湊齊兩個才判斷。
  XmlControlCharFilter f;
  EXPECT_EQ(filtered(f, std::string("\xFF", 1)), std::string("\xFF", 1));
  const std::string rest = std::string("\xFE<\0\x08\x4E", 5);  // FF|FE ＝ UTF-16LE 的 BOM → 不濾
  EXPECT_EQ(filtered(f, rest), rest);

  XmlControlCharFilter g;  // UTF-8：第一塊 "<"，第二塊才有控制字元
  EXPECT_EQ(filtered(g, std::string("<")), std::string("<"));
  EXPECT_EQ(filtered(g, std::string("a\x08>")), std::string("a>"));
}

TEST(XmlControlFilter, LoneControlByteAsFirstChunkIsDropped) {
  // codex v345：第一塊只有一個位元組、而且就是控制字元 → 不能原樣交給 expat
  XmlControlCharFilter f;
  EXPECT_EQ(filtered(f, std::string("\x08")), std::string());
  EXPECT_EQ(filtered(f, std::string("<a>\x0B</a>")), std::string("<a></a>"));
  const std::string doc = std::string("\x08") + kOpf;
  EXPECT_FALSE(parse(doc, false, 1).ok);
  EXPECT_TRUE(parse(doc, true, 1).ok);
}

TEST(XmlControlFilter, Utf16WithoutBomStartingWithWhitespaceIsNotTouched) {
  // codex v345：沒有 BOM、第一個字是空白（LE "20 00"／BE "00 20"）也要認得是 UTF-16
  const std::string le = std::string(" \0<\0a\0>\0\x08\x4E<\0/\0a\0>\0", 18);
  const std::string be = std::string("\0 \0<\0a\0>\x4E\x08\0<\0/\0a\0>", 18);
  for (const auto* doc : {&le, &be}) {
    XmlControlCharFilter f;
    EXPECT_EQ(filtered(f, *doc), *doc);
  }
}

TEST(XmlControlFilter, DecisionIsPerDocument) {
  XmlControlCharFilter f;
  EXPECT_EQ(filtered(f, std::string("\xFF\xFE\x08\x4E", 4)), std::string("\xFF\xFE\x08\x4E", 4));
  f = XmlControlCharFilter{};  // 解析器每份文件開始時重設
  EXPECT_EQ(filtered(f, std::string("<a>\x08</a>")), std::string("<a></a>"));
}

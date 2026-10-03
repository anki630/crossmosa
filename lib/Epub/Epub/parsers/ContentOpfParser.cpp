#include "ContentOpfParser.h"

#include <FsHelpers.h>
#include <Logging.h>
#include <Serialization.h>
#include <XmlParserUtils.h>

#include <cctype>
#include <cstring>

#include "Epub/BookMetadataCache.h"

namespace {
constexpr char MEDIA_TYPE_NCX[] = "application/x-dtbncx+xml";
constexpr char MEDIA_TYPE_CSS[] = "text/css";
constexpr char MEDIA_TYPE_IMAGE_PREFIX[] = "image/";
constexpr char itemCacheFile[] = "/.items.bin";

bool startsWithImageMediaType(const std::string& mediaType) {
  constexpr size_t prefixLen = sizeof(MEDIA_TYPE_IMAGE_PREFIX) - 1;
  if (mediaType.size() < prefixLen) {
    return false;
  }

  for (size_t i = 0; i < prefixLen; ++i) {
    const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(mediaType[i])));
    if (c != MEDIA_TYPE_IMAGE_PREFIX[i]) {
      return false;
    }
  }

  return true;
}

// v350：itemref 的 properties 是以空白分隔的記號清單（EPUB 3.3 §5.4.2）。
//   回傳 1＝rendition:layout-pre-paginated、0＝rendition:layout-reflowable、-1＝兩者都沒寫（沿用全書預設）。
int spineLayoutOverride(const char* props) {
  static constexpr char kPre[] = "rendition:layout-pre-paginated";
  static constexpr char kFlow[] = "rendition:layout-reflowable";
  int result = -1;
  if (!props) return result;
  const char* p = props;
  while (*p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    const char* const start = p;
    while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') p++;
    const size_t n = static_cast<size_t>(p - start);
    if (n == sizeof(kPre) - 1 && strncmp(start, kPre, n) == 0) {
      result = 1;
    } else if (n == sizeof(kFlow) - 1 && strncmp(start, kFlow, n) == 0) {
      result = 0;
    }
  }
  return result;
}

// 前後空白去掉之後是不是剛好這個字（<meta> 的文字可能換行縮排）。
bool trimmedEquals(const std::string& s, const char* word) {
  size_t b = 0;
  size_t e = s.size();
  while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) b++;
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) e--;
  const size_t n = strlen(word);
  return e - b == n && s.compare(b, n, word) == 0;
}
}  // namespace

bool ContentOpfParser::setup() {
  parser = XML_ParserCreate(nullptr);
  xmlFilter_ = XmlControlCharFilter{};  // v345：每份文件重新判斷是不是 UTF-16
  if (!parser) {
    LOG_DBG("COF", "Couldn't allocate memory for parser");
    return false;
  }

  XML_SetUserData(parser, this);
  XML_SetElementHandler(parser, startElement, endElement);
  XML_SetCharacterDataHandler(parser, characterData);
  return true;
}

ContentOpfParser::~ContentOpfParser() {
  destroyXmlParser(parser);
  if (tempItemStore) {
    tempItemStore.close();
  }
  const auto itemCachePath = cachePath + itemCacheFile;
  if (Storage.exists(itemCachePath.c_str())) {
    Storage.remove(itemCachePath.c_str());
  }
}

size_t ContentOpfParser::write(const uint8_t data) { return write(&data, 1); }

size_t ContentOpfParser::write(const uint8_t* buffer, const size_t size) {
  if (!parser) return 0;

  const uint8_t* currentBufferPos = buffer;
  auto remainingInBuffer = size;

  while (remainingInBuffer > 0) {
    void* const buf = XML_GetBuffer(parser, 1024);

    if (!buf) {
      LOG_ERR("COF", "Couldn't allocate memory for buffer");
      destroyXmlParser(parser);
      return 0;
    }

    const auto toRead = remainingInBuffer < 1024 ? remainingInBuffer : 1024;
    memcpy(buf, currentBufferPos, toRead);
    // v345（帳本 D14）：交給 expat 的是濾掉控制字元之後的長度；下面的剩餘量照原始長度算

    if (XML_ParseBuffer(parser, static_cast<int>(xmlFilter_.apply(static_cast<char*>(buf), toRead)),
                        remainingSize == toRead) == XML_STATUS_ERROR) {
      LOG_DBG("COF", "Parse error at line %lu: %s", XML_GetCurrentLineNumber(parser),
              XML_ErrorString(XML_GetErrorCode(parser)));
      destroyXmlParser(parser);
      return 0;
    }

    currentBufferPos += toRead;
    remainingInBuffer -= toRead;
    remainingSize -= toRead;
  }

  return size;
}

void XMLCALL ContentOpfParser::startElement(void* userData, const XML_Char* name, const XML_Char** atts) {
  auto* self = static_cast<ContentOpfParser*>(userData);
  const char* const element = xmlLocalName(name);

  // v350：rendition:layout 的 <meta> 裡面不該有元素；真的有（結構異常但 XML 合法），只記層數，
  //   收尾只認同一層的 </meta>（複查：否則內層的 </meta> 會提早結束、後面的文字被丟掉）。
  if (self->state == IN_RENDITION_LAYOUT) {
    self->renditionNestDepth++;
    return;
  }

  if (self->state == START && strcmp(element, "package") == 0) {
    self->state = IN_PACKAGE;
    return;
  }

  if (self->state == IN_PACKAGE && strcmp(element, "metadata") == 0) {
    self->state = IN_METADATA;
    return;
  }

  if (self->state == IN_METADATA && strcmp(element, "title") == 0) {
    // Only capture the first dc:title element; subsequent ones are subtitles
    if (self->title.empty()) {
      self->state = IN_BOOK_TITLE;
    }
    return;
  }

  if (self->state == IN_METADATA && strcmp(element, "creator") == 0) {
    self->state = IN_BOOK_AUTHOR;
    return;
  }

  if (self->state == IN_METADATA && strcmp(element, "language") == 0) {
    self->state = IN_BOOK_LANGUAGE;
    return;
  }

  if (self->state == IN_PACKAGE && strcmp(element, "manifest") == 0) {
    self->state = IN_MANIFEST;
    if (!Storage.openFileForWrite("COF", self->cachePath + itemCacheFile, self->tempItemStore)) {
      LOG_ERR("COF", "Couldn't open temp items file for writing. This is probably going to be a fatal error.");
    }
    return;
  }

  if (self->state == IN_PACKAGE && strcmp(element, "spine") == 0) {
    self->state = IN_SPINE;
    // 直排偵測用（見標頭的說明）。屬性名沒有命名空間前綴的疑慮 —— 它是 spine 自己的屬性。
    for (int i = 0; atts[i]; i += 2) {
      if (strcmp(atts[i], "page-progression-direction") == 0) {
        self->pageProgressionRtl = strcmp(atts[i + 1], "rtl") == 0;
        break;
      }
    }
    if (!Storage.openFileForRead("COF", self->cachePath + itemCacheFile, self->tempItemStore)) {
      LOG_ERR("COF", "Couldn't open temp items file for reading. This is probably going to be a fatal error.");
    }

    // Sort the (unconditionally-built) item index so every idref lookup uses binary
    // search. Without this, small/medium manifests fell back to an O(spine × manifest)
    // linear rescan of .items.bin per itemref (up to ~200ms/item at large scale).
    if (!self->itemIndex.empty()) {
      std::sort(self->itemIndex.begin(), self->itemIndex.end(), [](const ItemIndexEntry& a, const ItemIndexEntry& b) {
        return a.idHash < b.idHash || (a.idHash == b.idHash && a.idLen < b.idLen);
      });
      self->useItemIndex = true;
      LOG_DBG("COF", "Using fast index for %zu manifest items", self->itemIndex.size());
    }
    return;
  }

  if (self->state == IN_PACKAGE && strcmp(element, "guide") == 0) {
    self->state = IN_GUIDE;
    // TODO Remove print
    LOG_DBG("COF", "Entering guide state.");
    if (!Storage.openFileForRead("COF", self->cachePath + itemCacheFile, self->tempItemStore)) {
      LOG_ERR("COF", "Couldn't open temp items file for reading. This is probably going to be a fatal error.");
    }
    return;
  }

  if (self->state == IN_METADATA && strcmp(element, "meta") == 0) {
    bool isCover = false;
    std::string coverItemId;
    bool isRenditionLayout = false;
    bool refines = false;

    for (int i = 0; atts[i]; i += 2) {
      if (strcmp(atts[i], "name") == 0 && strcmp(atts[i + 1], "cover") == 0) {
        isCover = true;
      } else if (strcmp(atts[i], "content") == 0) {
        coverItemId = atts[i + 1];
      } else if (strcmp(atts[i], "property") == 0 && strcmp(atts[i + 1], "rendition:layout") == 0) {
        isRenditionLayout = true;
      } else if (strcmp(atts[i], "refines") == 0) {
        refines = true;
      }
    }

    if (isCover) {
      self->coverItemId = coverItemId;
    }
    // v350：全書預設的版面。帶 refines 的是在講別的東西，不是全書預設 —— 不收（EPUB 3.3：rendition:layout 不可以
    //   用 refines；單項覆寫走 itemref 的 properties）。content 屬性不是標準寫法，只在元素沒有文字時當備援。
    if (isRenditionLayout && !refines) {
      self->renditionLayoutText.clear();
      self->renditionLayoutContent = coverItemId.size() <= 64 ? coverItemId : std::string();
      self->renditionNestDepth = 0;
      self->state = IN_RENDITION_LAYOUT;
    }
    return;
  }

  if (self->state == IN_MANIFEST && strcmp(element, "item") == 0) {
    std::string itemId;
    std::string href;
    std::string mediaType;
    std::string properties;

    for (int i = 0; atts[i]; i += 2) {
      if (strcmp(atts[i], "id") == 0) {
        itemId = atts[i + 1];
      } else if (strcmp(atts[i], "href") == 0) {
        href = FsHelpers::normalisePath(FsHelpers::decodeUriEscapes(self->baseContentPath + atts[i + 1]));
      } else if (strcmp(atts[i], "media-type") == 0) {
        mediaType = atts[i + 1];
      } else if (strcmp(atts[i], "properties") == 0) {
        properties = atts[i + 1];
      }
    }

    // Record index entry for fast lookup later
    if (self->tempItemStore) {
      ItemIndexEntry entry;
      entry.idHash = fnvHash(itemId);
      entry.idLen = static_cast<uint16_t>(itemId.size());
      entry.fileOffset = static_cast<uint32_t>(self->tempItemStore.position());
      self->itemIndex.push_back(entry);
    }

    // Write items down to SD card
    serialization::writeString(self->tempItemStore, itemId);
    serialization::writeString(self->tempItemStore, href);

    if (itemId == self->coverItemId) {
      // Some EPUBs set meta name="cover" to an XHTML wrapper item.
      // Only treat it as a cover image when the manifest media-type is image/*.
      if (startsWithImageMediaType(mediaType)) {
        self->coverItemHref = href;
      } else {
        LOG_DBG("COF", "Ignoring meta cover item '%s' with non-image media type: %s", itemId.c_str(),
                mediaType.c_str());
      }
    }

    if (mediaType == MEDIA_TYPE_NCX) {
      if (self->tocNcxPath.empty()) {
        self->tocNcxPath = href;
      } else {
        LOG_DBG("COF", "Warning: Multiple NCX files found in manifest. Ignoring duplicate: %s", href.c_str());
      }
    }

    // Collect CSS files
    if (mediaType == MEDIA_TYPE_CSS) {
      self->cssFiles.push_back(href);
    }

    // EPUB 3: Check for nav document (properties contains "nav")
    if (!properties.empty() && self->tocNavPath.empty()) {
      // Properties is space-separated, check if "nav" is present as a word
      if (properties == "nav" || properties.find("nav ") == 0 || properties.find(" nav") != std::string::npos) {
        self->tocNavPath = href;
        LOG_DBG("COF", "Found EPUB 3 nav document: %s", href.c_str());
      }
    }

    // EPUB 3: Check for cover image (properties contains "cover-image")
    if (!properties.empty() && self->coverItemHref.empty()) {
      if (properties == "cover-image" || properties.find("cover-image ") == 0 ||
          properties.find(" cover-image") != std::string::npos) {
        self->coverItemHref = href;
      }
    }
    return;
  }

  // NOTE: This relies on spine appearing after item manifest (which is pretty safe as it's part of the EPUB spec)
  // Only run the spine parsing if there's a cache to add it to
  // v350：或是 layoutOnly —— 只要固定版面的清單（舊韌體建過索引的書補 layout.bin），不寫 spine。
  //   idref 照樣要查（找不到的 itemref 不佔位置），算出來的索引才會跟既有 book.bin 的 spine 對齊。
  if (self->cache || self->layoutOnly) {
    if (self->state == IN_SPINE && strcmp(element, "itemref") == 0) {
      // v350：properties 可能寫在 idref 的前面或後面 —— 先整個掃一遍，再決定這一項是不是固定版面。
      const char* props = nullptr;
      for (int i = 0; atts[i]; i += 2) {
        if (strcmp(atts[i], "properties") == 0) props = atts[i + 1];
      }
      const int layoutOverride = spineLayoutOverride(props);
      const bool fixedLayout = layoutOverride >= 0 ? layoutOverride == 1 : self->globalPrePaginated;
      for (int i = 0; atts[i]; i += 2) {
        if (strcmp(atts[i], "idref") == 0) {
          const std::string idref = atts[i + 1];
          std::string href;
          bool found = false;

          if (self->useItemIndex) {
            // Fast path: binary search
            uint32_t targetHash = fnvHash(idref);
            uint16_t targetLen = static_cast<uint16_t>(idref.size());

            auto it = std::lower_bound(self->itemIndex.begin(), self->itemIndex.end(),
                                       ItemIndexEntry{targetHash, targetLen, 0},
                                       [](const ItemIndexEntry& a, const ItemIndexEntry& b) {
                                         return a.idHash < b.idHash || (a.idHash == b.idHash && a.idLen < b.idLen);
                                       });

            // Check for match (may need to check a few due to hash collisions)
            while (it != self->itemIndex.end() && it->idHash == targetHash) {
              self->tempItemStore.seek(it->fileOffset);
              std::string itemId;
              serialization::readString(self->tempItemStore, itemId);
              if (itemId == idref) {
                serialization::readString(self->tempItemStore, href);
                found = true;
                break;
              }
              ++it;
            }
          } else {
            // Fallback linear scan, only reached when the index is empty (no manifest
            // items). The fast binary-search path above is used for all real manifests.
            self->tempItemStore.seek(0);
            std::string itemId;
            while (self->tempItemStore.available()) {
              serialization::readString(self->tempItemStore, itemId);
              serialization::readString(self->tempItemStore, href);
              if (itemId == idref) {
                found = true;
                break;
              }
            }
          }

          if (found) {
            if (self->cache) self->cache->createSpineEntry(href);
            // v350：只記固定版面的那幾項（一般的書一個都沒有 ＝ 不配置）。索引＝這一項在 book.bin 的 spine 位置。
            //   記不下（超過 uint16、或記憶體不夠擴容）→ 標記 overflow，整本書的清單當作不知道（不寫 layout.bin）。
            if (fixedLayout && !self->layoutOverflow) {
              if (self->builtSpineCount > UINT16_MAX ||
                  !self->fixedLayoutSpines.push(static_cast<uint16_t>(self->builtSpineCount))) {
                self->layoutOverflow = true;
              }
            }
            self->builtSpineCount++;
          }
        }
      }
      return;
    }
  }
  // parse the guide
  if (self->state == IN_GUIDE && strcmp(element, "reference") == 0) {
    std::string type;
    std::string guideHref;
    for (int i = 0; atts[i]; i += 2) {
      if (strcmp(atts[i], "type") == 0) {
        type = atts[i + 1];
      } else if (strcmp(atts[i], "href") == 0) {
        guideHref = FsHelpers::normalisePath(FsHelpers::decodeUriEscapes(self->baseContentPath + atts[i + 1]));
      }
    }
    if (!guideHref.empty()) {
      // EPUB 2 guides often mark every content file as "text", so that type
      // does not identify a reliable first-reading location. Only use the
      // explicit "start" semantic; otherwise the reader opens at spine index 0.
      if (type == "start" && !self->hasExplicitStartReference) {
        LOG_DBG("COF", "Found %s reference in guide: %s", type.c_str(), guideHref.c_str());
        self->textReferenceHref = guideHref;
        self->hasExplicitStartReference = type == "start";
      } else if ((type == "cover" || type == "cover-page") && self->guideCoverPageHref.empty()) {
        LOG_DBG("COF", "Found cover reference in guide: %s", guideHref.c_str());
        self->guideCoverPageHref = guideHref;
      }
    }
    return;
  }
}

void XMLCALL ContentOpfParser::characterData(void* userData, const XML_Char* s, const int len) {
  auto* self = static_cast<ContentOpfParser*>(userData);

  if (self->state == IN_BOOK_TITLE) {
    self->title.append(s, len);
    return;
  }

  if (self->state == IN_BOOK_AUTHOR) {
    if (!self->author.empty()) {
      self->author.append(", ");  // Add separator for multiple authors
    }
    self->author.append(s, len);
    return;
  }

  if (self->state == IN_BOOK_LANGUAGE) {
    self->language.append(s, len);
    return;
  }

  // v350：`<meta property="rendition:layout">` 只會是 pre-paginated／reflowable —— 上限 64 bytes。
  //   ⚠️ 要截的是【這一次加進去的長度】：expat 一次回呼可以給很長一段，只看目前長度還是會一次配一大塊（複查）。
  if (self->state == IN_RENDITION_LAYOUT) {
    const size_t have = self->renditionLayoutText.size();
    if (have < 64 && len > 0) {
      self->renditionLayoutText.append(s, std::min(static_cast<size_t>(len), 64 - have));
    }
    return;
  }
}

void XMLCALL ContentOpfParser::endElement(void* userData, const XML_Char* name) {
  auto* self = static_cast<ContentOpfParser*>(userData);
  const char* const element = xmlLocalName(name);

  if (self->state == IN_SPINE && strcmp(element, "spine") == 0) {
    self->state = IN_PACKAGE;
    self->tempItemStore.close();
    return;
  }

  if (self->state == IN_GUIDE && strcmp(element, "guide") == 0) {
    self->state = IN_PACKAGE;
    self->tempItemStore.close();
    return;
  }

  if (self->state == IN_MANIFEST && strcmp(element, "manifest") == 0) {
    self->state = IN_PACKAGE;
    self->tempItemStore.close();
    return;
  }

  if (self->state == IN_BOOK_TITLE && strcmp(element, "title") == 0) {
    self->state = IN_METADATA;
    return;
  }

  if (self->state == IN_BOOK_AUTHOR && strcmp(element, "creator") == 0) {
    self->state = IN_METADATA;
    return;
  }

  if (self->state == IN_BOOK_LANGUAGE && strcmp(element, "language") == 0) {
    self->state = IN_METADATA;
    return;
  }

  if (self->state == IN_RENDITION_LAYOUT) {
    if (self->renditionNestDepth > 0) {  // 裡面的元素收尾（見 startElement），不是這個 <meta> 本身
      self->renditionNestDepth--;
      return;
    }
    // 走到這裡一定是同一層的 </meta>（XML 良構性保證）。文字優先；沒有文字才看 content 屬性。
    const bool hasText = !trimmedEquals(self->renditionLayoutText, "");
    self->globalPrePaginated =
        trimmedEquals(hasText ? self->renditionLayoutText : self->renditionLayoutContent, "pre-paginated");
    self->renditionLayoutText.clear();
    self->renditionLayoutContent.clear();
    self->state = IN_METADATA;
    return;
  }

  if (self->state == IN_METADATA && strcmp(element, "metadata") == 0) {
    self->state = IN_PACKAGE;
    return;
  }

  if (self->state == IN_PACKAGE && strcmp(element, "package") == 0) {
    self->state = START;
    return;
  }
}

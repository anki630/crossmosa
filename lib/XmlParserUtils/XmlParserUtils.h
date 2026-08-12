#pragma once

#include <expat.h>

#include <cstring>

// Safely tear down an expat parser: stop processing, clear callbacks, free, and null the pointer.
inline void destroyXmlParser(XML_Parser& parser) {
  if (!parser) return;
  XML_StopParser(parser, XML_FALSE);
  XML_SetElementHandler(parser, nullptr, nullptr);
  XML_SetCharacterDataHandler(parser, nullptr);
  XML_ParserFree(parser);
  parser = nullptr;
}

// Every parser in this tree creates its expat parser with XML_ParserCreate(nullptr), i.e. with
// namespace processing DISABLED, so element names arrive exactly as the document spelled them --
// prefix and all. Most EPUBs bind the relevant namespace as the default (bare <spine>, <navPoint>),
// but any toolchain that re-serialises the XML (Python ElementTree in particular) emits explicit
// prefixes like <ns0:spine>. Matching a hardcoded prefix list can only ever chase instances, since
// the prefix is arbitrary; compare the LOCAL name instead.
//
// This is the identity function on unprefixed names -- it returns the same pointer -- so documents
// that already parsed keep parsing byte-identically.
//
// ⚠️ When converting a comparison, remember to strip the prefix from the TARGET string too if it
// has one (ContentOpfParser's "dc:title"/"dc:creator"/"dc:language" did): comparing a local name
// against a prefixed literal never matches, and the resulting breakage is silent.
inline const char* xmlLocalName(const char* name) {
  const char* const colon = strrchr(name, ':');
  return colon ? colon + 1 : name;
}

#pragma once

// Path-protection rules shared by every network-facing filesystem surface
// (WebDAV, and now the SMB2 server). Originally lived only inside
// WebDAVHandler::isProtectedPath -- extracted here so SmbFileHandlers.cpp's
// create_cmd (write-intent opens) can enforce the exact same rules without a
// second, hand-copied ruleset that could silently drift from WebDAV's over
// time. Deliberately dependency-free (no Arduino/HAL types) so it compiles
// unmodified in the desktop SMB2 test harness (test/host/) as well as the
// device build.
#include <cstddef>

namespace ProtectedPath {

// True if `name` (a single path segment/basename -- no '/' separators) is
// something these surfaces should hide or refuse to write to: a dotfile/
// dot-directory (e.g. the ".crossmosa" data directory), or one of the
// device's own reserved directory names.
bool isProtectedName(const char* name);

// True if `name` is one of the device's own reserved names -- everything
// isProtectedName() covers EXCEPT the leading-dot rule.
//
// v78: for listings that honour the user's "show hidden files" preference and
// nothing else. Keeping this separate is not a nicety: v77 pointed the web file
// manager's listing filter at isProtectedName(), whose dot rule made EVERY
// dotfile permanently invisible, and the preference silently stopped working.
// Access control must keep using isProtectedName()/isProtected() -- a listing
// choosing to SHOW something never means it may be read.
bool isSystemName(const char* name);

// True if `path` (possibly multi-segment, '/'-separated, leading slash
// optional) has ANY segment for which isProtectedName() is true. This is
// the same segment-by-segment walk WebDAVHandler::isProtectedPath has always
// done (not just checking the last component) -- e.g. rejects both
// "/.crossmosa/foo" and "/foo/.crossmosa/bar".
bool isProtected(const char* path);

}  // namespace ProtectedPath

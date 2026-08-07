#pragma once

// Single, unambiguous entry point for the vendored libsmb2 dependency
// (lib/smb2/, see docs/third-party/libsmb2-vendoring.md). Every project file
// that needs libsmb2 should include *this* header — never smb2.h/libsmb2.h
// directly — so the fragile include-order requirement below lives in exactly
// one place instead of being a contract every consumer file must remember.
//
// Background, for whoever next touches this: smb2.h's public API gates its
// own `#include <stdint.h>` / `#include <time.h>` behind the autoconf-style
// macros HAVE_STDINT_H / HAVE_TIME_H, which are only ever defined by
// libsmb2's bundled lib/smb2/include/esp/config.h (reached via the
// `-Iinclude/esp` flag in lib/smb2/library.json + this project's
// `-DHAVE_CONFIG_H`). Upstream's own lib/smb2/lib/*.c files always
// `#include "config.h"` before `#include "smb2.h"`; without doing the same,
// a bare `#include <smb2.h>` fails with `'uint32_t' does not name a type`.
//
// "config.h" is about as generic a filename as exists in C — nothing stops a
// future vendored dependency or another project file from shadowing it
// depending on include-path order, and relying on every future consumer to
// get this include order right by hand is a standing invitation for exactly
// that kind of silent breakage. Routing everything through this one wrapper
// removes that risk: consumers depend on a distinctive, project-owned name
// instead of on getting a generic-filename include order right themselves.
#include "config.h" // resolves to lib/smb2/include/esp/config.h — do not reorder
#include <smb2.h>
#include <libsmb2.h>
// v65: the "raw" PDU-level API, for ONE thing -- smb2_cmd_error_reply_async()
// (libsmb2-raw.h:488), which is how a handler answers with a REAL NT status
// instead of the STATUS_NOT_IMPLEMENTED every dispatcher substitutes for a
// negative return. See replyStatus() in src/network/SmbFileHandlers.cpp for why
// that matters -- short version: "this file does not exist" and "this command is
// unsupported" were indistinguishable, which made mkdir and copy impossible
// from a real OS-level client.
//
// This is a header libsmb2 already ships; including it changes nothing under
// lib/smb2/, so it costs no vendored divergence -- scripts/verify_libsmb2_patch.py
// still reports exactly 3.
#include <libsmb2-raw.h>

// ---------------------------------------------------------------------------
// CrossMosa addition: crossmosa_smb2_finish_accept() is a small function
// appended to the END of lib/smb2/lib/libsmb2.c (see that file's own banner
// comment and docs/third-party/libsmb2-vendoring.md for the full rationale —
// short version: SmbServer's non-blocking driver loop needs it, and the
// two things it depends on, struct connect_data and smb2_negotiate_request_cb,
// have zero visibility outside that one file, so the function itself had to
// live there). Declaring its prototype here — not in any lib/smb2/ header —
// keeps every file under lib/smb2/include/ exactly as upstream ships it;
// this wrapper is already the project's single funnel for consuming libsmb2,
// so this is one more thing consumers get from it rather than a second place
// to remember.
//
// Semantics: given an smb2_context freshly returned by the existing public
// smb2_serve_port_async() and the owning smb2_server, finishes preparing it
// to receive an SMB2_NEGOTIATE request (allocates connect_data, registers
// the first PDU, copies the three size limits, sets owning_server). Returns
// 0 on success; on failure the context has already been closed via
// smb2_close_context() (matching smb2_serve_port()'s own error handling) and
// the caller should not use it further except to let its normal cull/destroy
// path reclaim it.
#ifdef __cplusplus
extern "C" {
#endif

int crossmosa_smb2_finish_accept(struct smb2_context* smb2, struct smb2_server* server);

#ifdef __cplusplus
}
#endif

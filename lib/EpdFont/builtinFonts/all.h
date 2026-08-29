#pragma once

// CrossMosa L1/L3：notoserif 全家族（16 面）與所有斜體（8 面）已移除。
// 要還原：把 include 加回來，並把 main.cpp 的 (&notosans_ 改回 (&notoserif_。
#include <builtinFonts/notosans_8_regular.h>
#include <builtinFonts/notosans_12_bold.h>
#include <builtinFonts/notosans_12_regular.h>
#include <builtinFonts/notosans_14_bold.h>
#include <builtinFonts/notosans_14_regular.h>
#include <builtinFonts/notosans_16_bold.h>
#include <builtinFonts/notosans_16_regular.h>
#include <builtinFonts/notosans_18_bold.h>
#include <builtinFonts/notosans_18_regular.h>
#include <builtinFonts/ubuntu_10_bold.h>
#include <builtinFonts/ubuntu_10_regular.h>
#include <builtinFonts/ubuntu_14_bold.h>
// CrossMosa：UI 第二字級由 12px 改為 14px（v11）。清單主文字太小是使用者回報的
// 首要問題，而「留 10 + 留 12 + 再加 14」會爆 flash，所以是【換掉】12px 而非新增。
// 變數名 ui12* 保留不動，資料指向 ubuntu_14（見 main.cpp）。
#include <builtinFonts/ubuntu_14_regular.h>

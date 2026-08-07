#pragma once

// NOTE (繁中自訂版): notoserif 家族的 16 個字型已移除，用來換取 UI 字型加入
// 5,413 個繁體漢字所需的 ~1.6 MB flash。main.cpp 裡的 notoserif* 字型物件已改指
// 向對應的 notosans 資料，因此設定中的「Serif」選項會以黑體呈現。
// 要還原：把下面的 include 加回來，並把 main.cpp 的 (&notosans_ 改回 (&notoserif_。
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
// UI_12_FONT_ID 的資料改用 14px（清單/標題放大用）。main.cpp 的 ui12*Font 指向這裡。
// ubuntu_12 已停用（其使用者全升到 14px）。10px（ubuntu_10）仍保留給狀態列/副標等窄處。
#include <builtinFonts/ubuntu_14_bold.h>
#include <builtinFonts/ubuntu_14_regular.h>

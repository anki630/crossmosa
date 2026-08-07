from pathlib import Path

Import("env")


PROJECT_DIR = Path(env.subst("$PROJECT_DIR"))
MARKER = "/* CrossPoint wolfSSL compatibility overrides */"
OVERRIDES = f"""

{MARKER}
#undef NO_DH
#ifndef HAVE_FFDHE_2048
#define HAVE_FFDHE_2048
#endif
#undef FP_MAX_BITS
#define FP_MAX_BITS 16384
/* v25: 除錯字串從未在執行期開啟,#undef 回收 ~30KB flash(673 條 WOLFSSL_MSG 字串+呼叫碼) */
/* v107: 查完了,#undef 放回來(v106 的追蹤已指名根因:EccMakeKey → -125 MEMORY_E)。
   若日後 TLS 又出問題,重開的方法是把這行改成 #define DEBUG_WOLFSSL,並確認
   HttpDownloader.cpp 有呼叫 wolfSSL_Debugging_ON() —— 光定義巨集不會輸出。 */
#undef DEBUG_WOLFSSL
"""


def patch_user_settings(path: Path) -> None:
    text = path.read_text()
    if MARKER in text:
        text = text.split(MARKER, 1)[0].rstrip()
    path.write_text(text + OVERRIDES + "\n")
    print(f"Patched wolfSSL settings: {path.relative_to(PROJECT_DIR)}")


for settings in PROJECT_DIR.glob(".pio/libdeps/*/Arduino-wolfSSL/src/user_settings.h"):
    patch_user_settings(settings)

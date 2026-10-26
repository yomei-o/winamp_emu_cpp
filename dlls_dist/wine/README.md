# Wine の comctl32.dll

Jw_cad が使うコモンコントロール（ツールバー・ステータスバー・ツールチップ・タブ・
ツリーなど）は、この DLL をエミュレータの中でそのまま動かして描きます。

| | |
|---|---|
| 出どころ | Debian bookworm `libwine_8.0~repack-4_i386.deb` の `usr/lib/i386-linux-gnu/wine/i386-windows/comctl32.dll` |
| 元の SHA-256 | `55c32bde0d63fc2b57e4f24cfc1072f0c022166a5941d9c26f0b001f17d00f60` |
| 加工 | `strip`（デバッグ情報を除いただけ。コードは無変更） |
| ライセンス | GNU LGPL 2.1 以降（`COPYING.LIB`） |
| ソース | Wine 8.0 — <https://dl.winehq.org/wine/source/8.0/wine-8.0.tar.xz>（`dlls/comctl32/`）、Debian のソースパッケージ <https://sources.debian.org/src/wine/8.0~repack-4/> |

`tools/build.sh` が `dlls/` にコピーし、jwemu はそこから読みます。

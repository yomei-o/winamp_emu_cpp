# RESUME — 作業の引き継ぎ

別の Claude（別のマシン）がこのリポの作業を続けるためのメモ。まず README.md と
これを読み、`git pull` してから始めること。

## 何をしているか

Winamp 2.95（`orig/winamp295.exe` の中身）を**無改変**で x86 エミュレータ上で動かし、WASM にして
ブラウザで鳴らす。エミュレータは jwcad_win_emu_cpp（commit 55bdc8f）からの取り込みで、
Winamp のために足したものは README の「Winamp のために足したもの」。

公開版: https://yomei-o.github.io/winamp_emu_cpp/

## いまの状態（2026-10-26）

* ネイティブ（ヘッドレス）とブラウザの両方で、起動・MP3 再生まで確認
* ブラウザ：`node tools/web_test.mjs` で、パネルの ▶ → 再生 → AudioWorklet が約 44100 フレーム/秒で
  消費（ARM64 ノートで 42000〜44000。時々わずかに追いつかない）
* 確認していないもの：マウスでの操作全般（ボタン・シーク・音量・EQ・窓のドラッグ）、右クリックメニュー、
  Preferences などのダイアログ、ファイルを開くダイアログ（GetOpenFileNameA は未実装 → 下）

## 最初にやること

1. `git pull`
2. `sh tools/build.sh` → `sh tools/run.sh 8 'C:\Music\doremi.mp3'` → tmp/shot.png で再生中の画面
3. `sh tools/build_wasm.sh` → `node tools/web_test.mjs` が全部 ok
   （ユーザーの方針：確認済みの成果は聞かずに commit → pull → push まで）

## 残りの作業（優先順）

1. **GetOpenFileNameA / GetSaveFileNameA**：Winamp の「L」（Open file）・プレイリストの ADD。
   W 版は `src/win32/shell_ole.cpp` の file_dialog にある。OPENFILENAMEA → W の変換と、
   OFN_ALLOWMULTISELECT|OFN_EXPLORER の複数選択（ディレクトリ\0名前\0名前\0\0）
2. マウス操作の確認：再生・停止・シーク・音量（waveOutSetVolume → audio_volume）・EQ・窓の移動と
   スナップ・ダブルクリック（ウィンドウシェード）・右クリックメニュー
3. ダイアログのフォント：MS Sans Serif が明朝っぽいセリフ体になる（初回のセットアップ画面で見えた）。
   サンセリフ（Liberation Sans 等）に割り当てる
4. 設定の保存：Winamp は winamp.ini を終了時にしか書かない。ブラウザではタブを閉じるだけなので残らない。
   定期的に IPC_WRITECONFIG（WM_USER 237）を送る、などで IndexedDB に残す
5. 速度：遅いマシン向け。プロファイル（tools/ にはまだ無い。jwcad の profile.sh 参照）。
   in_mp3 の内側ループ（x87）を速くするのが効くはず
6. 他のプラグイン：in_wave（MSACM32 が要る）、in_vorbis（Ogg）、in_mod など。インストーラには入っている
   （`tools/prepare_winamp.py` の WANTED に足す）。vis プラグインは DirectDraw なので後回し
7. WSOCK32 は序数インポートのまま未実装（呼ばれたら止まる）。ネット機能は inet_mode=2 で切ってある

## 分かっていること（ハマりどころ）

* **待ちの作り**：GetMessage・Sleep・WaitForSingleObject は、スレッドが call_guest の外（深さ 0）なら
  Blocked にして yield、中（ウィンドウプロシージャの中など）なら `run_others_while` で他スレッドを回しながら
  その場で待つ。jwcad 版はホストを直接待っていたので、Winamp だとデコードが止まる
* `run_others_while` は待つ側のスレッドに戻してから条件を評価する（ミューテックスの持ち主を正しく記録するため）
* waveOut の再生位置はホストの `audio_played()`（ブラウザでは Worklet が進めるカウンタ）。
  リセット時は「書いた分は全部再生済み」として扱う（WasmHost::audio_reset）
* out_wave は CALLBACK_EVENT。完了は `device_poll`（スケジューラが毎回呼ぶ）で拾ってイベントを立てる
* 初回起動のユーザー登録ダイアログ・更新確認・ミニブラウザは winamp.ini（prepare_winamp.py が書く）で止めている
* EM_JS から `stringToNewUTF8` は使えない（ランタイムに入っていない）。黙って失敗するので注意
* インストーラのブロックが一つ解凍できない（大きいもの。使っていない）
* ユーザーの好み：返事は日本語、確認済みの成果は commit → pull → push まで自動で

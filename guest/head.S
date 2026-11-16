/*
 * head.S - TCP/IP ドライバのヘッダ
 *
 * プログラムイメージの先頭に置かれる。.header セクションは
 * リンカスクリプト (x68k.ld) でイメージ先頭に配置される。
 *
 * アプリ側 (libsocket の search_ti_entry) は、常駐メモリの先頭にある
 * "TCP/IP" の署名を探し、先頭から 8 バイト目を入口として
 *   D0 = 関数番号, A0 = 引数配列へのポインタ
 * で呼び出し、結果を D0 で受け取る。
 */

    .section    ".header","ax"

    .global tcpip_magic
    .global jsr_entry
    .extern do_command

tcpip_magic:
    .ascii  "TCP/IP\x02\x00"

jsr_entry:
    pea.l   %pc@(do_command)
    .word   0xfff6              /* DOS _SUPER_JSR (スーパーバイザモードで呼ぶ) */
    addq.l  #4,%sp
    rts

    .end

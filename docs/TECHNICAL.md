# hfsnet 技術詳細

[English](TECHNICAL.en.md) | [日本語](TECHNICAL.md)

プロトコルとドライバ実装の詳細です。
使い方は [README](../README.md) を参照してください。

## ファイルプロトコル

共有フォルダに置くファイルは2つだけです。どちらもバイナリ形式、
多バイト値はビッグエンディアン（ネットワークバイトオーダ）です。

- `HFSREQ.BIN` — ゲスト → ホスト の要求（ゲストが書く）
- `HFSRES.BIN` — ホスト → ゲスト の応答（ホストが書く。一時名で書いてから
  リネームして差し替えるので、ゲストが書きかけを読むことはありません）

### 要求（HFSREQ.BIN、ヘッダ 20 バイト＋ペイロード）

| オフセット | サイズ | 内容 |
| --- | --- | --- |
| 0 | 2 | マジック `'H' 'N'` |
| 2 | 1 | 操作コード（下表） |
| 3 | 1 | ソケット番号（ゲスト側のスロット 0〜31） |
| 4 | 4 | 通し番号（ゲストが要求ごとに増やす。応答の照合に使う） |
| 8 | 2 | ポート番号（CONNECT / BIND で使用。UDP の SEND では宛先ポート） |
| 10 | 2 | 補助引数（OPEN のソケット種別（0 = SOCK_STREAM、1 = SOCK_DGRAM、2 = SOCK_RAW）、LISTEN の backlog、ACCEPT の受け入れ先スロット、SHUTDOWN の how） |
| 12 | 4 | IPv4 アドレス（CONNECT / BIND で使用。UDP の SEND では宛先アドレス） |
| 16 | 4 | ペイロード長（RECV では受け取り上限） |
| 20 | 可変 | ペイロード（SEND のデータ、RESOLVE のホスト名など） |

### 応答（HFSRES.BIN、ヘッダ 24 バイト＋ペイロード）

| オフセット | サイズ | 内容 |
| --- | --- | --- |
| 0 | 2 | マジック `'H' 'N'` |
| 2 | 1 | 操作コード（要求の写し） |
| 3 | 1 | ソケット番号（要求の写し） |
| 4 | 4 | 通し番号（要求の写し） |
| 8 | 4 | 結果（操作ごとに意味が違う。下表） |
| 12 | 2 | ポート番号（ACCEPT で相手のポート、UDP の RECV では送信元ポート） |
| 14 | 2 | 予約（0） |
| 16 | 4 | ペイロード長（STATE では受信可能バイト数） |
| 20 | 4 | 補助値（STATE では状態コード、RESOLVE では IPv4 アドレス、ACCEPT では相手の IPv4 アドレス、UDP の RECV では送信元 IPv4 アドレス） |
| 24 | 可変 | ペイロード（RECV のデータ） |

### 操作コードと結果の意味

| コード | 名前 | 結果（result）の意味 |
| --- | --- | --- |
| 1 | OPEN | 0 = 成功、-1 = 失敗 |
| 2 | CLOSE | 0 = 成功 |
| 3 | CONNECT | 0 = 接続完了、1 = 接続中、-1 = 失敗 |
| 4 | SEND | 送信できたバイト数、-2 = 今は送れない、-1 = 失敗 |
| 5 | RECV | 受信したバイト数、0 = 相手が閉じた（EOF）、-2 = まだデータが無い、-1 = 失敗 |
| 6 | STATE | 状態コード（下表）。len = 受信可能バイト数、aux = 送信可能バイト数（近似） |
| 7 | RESOLVE | 0 = 成功（aux に IPv4 アドレス）、-1 = 失敗 |
| 8 | BIND | 0 = 成功、-1 = 失敗 |
| 9 | LISTEN | 0 = 成功、-1 = 失敗 |
| 10 | ACCEPT | 0 = 成功、-2 = まだ接続が来ていない、-1 = 失敗 |
| 11 | SHUTDOWN | 0 = 成功、-1 = 失敗。how（補助引数）: 0 = 受信側、1 = 送信側、2 = 両方 |
| 12 | HOSTINFO | 0 = 成功（aux にホストの代表 IPv4 アドレス、取れなければ 0）。ゲストドライバが常駐時に1回だけ発行し、ホストの応答確認と IP アドレス表示に使う。旧いホストは未知の操作コードとして -1 を返す |

状態コード: 0 CLOSED / 1 LISTEN / 2 SYN_SENT / 3 SYN_RECEIVED /
4 ESTABLISHED / 5 CLOSE_WAIT / 6 FIN_WAIT / 7 TIME_WAIT / 8 CLOSING

### 同期の約束

- 同時に処理中の要求は1つだけです（Human68k のアプリは基本的に
  1つの流れでドライバを呼ぶため、これで足ります）
- ゲストは要求を書いて閉じてから、応答の通し番号が自分のものと一致するまで
  応答ファイルを開き直し続けます
- ホストは、通し番号が前回処理したものと違う要求だけを処理します。
  書きかけの要求（ヘッダの長さと実ファイル長が合わないもの）は読み直します

## ゲスト側ドライバが実装する TI 関数

番号は計測技研 tcpipdrv.h の関数番号表（`_TI_` の並び順）に従います。

実装済み: get_version / socket / bind / listen / accept / connect /
read_s / write_s / recvfrom / sendto / close_s / shutdown / socklen /
getsockname / getpeername / sockkick / usesock / recvline / sendline /
rrecvchar / recvchar / usflush / seteol / sockmode / setflush /
psocket / sockerr / sockstate / sock_top / ntoa_sock / gethostbyname /
get_MIB / add_arp_table / del_arp_table / search_arp_table /
get_arp_table_top / arp_request / get_iface_list / get_new_iface /
link_new_iface / rt_top / rt_lookup / rt_lookupb / rt_drop / rt_add /
dns_add / dns_drop / dns_get / set_domain_name / get_domain_name / rip

管理系（iface / 経路 / DNS / ARP / RIP）は、実通信がホスト側の
スタックにあるため、表示と設定のための台帳として実装しています。
インターフェースは lp0（127.0.0.1）と hf0（ホストの IP アドレス、
常駐時の HOSTINFO 応答で取得。ネットマスクはクラス別デフォルト）
の2つで、ifconfig / netstat -i / netstat -r が使えます。経路は
デフォルト・hf0 のサブネット・127.0.0.0/8 の3件だけを返します。
inetdconf のデフォルトルータ・DNS サーバ・ドメイン名・RIP は
ゲスト内に保持され表示に反映されますが、実通信には影響しません。
ARP テーブルは持ちません（get_arp_table_top は17本の空バケツ配列
へのポインタを返す、という ABI の約束どおりにします）。

ポインタを返す名前引き関数（gethostbyaddr / getnetbyname /
getnetbyaddr / getservbyname / getservbyport / getprotobyname /
getprotobynumber）は、引けない場合に -1 ではなく NULL を返します。
それ以外の未実装関数は -1 を返します。制限事項:

- 対応するソケット種別は TCP（SOCK_STREAM）、UDP（SOCK_DGRAM）、
  RAW（SOCK_RAW）です
- RAW は ICMP（インターネット制御メッセージ・プロトコル）エコー用で、
  ping.x が使います。実際の送受信は GUI 版ホストが担い、Python 版
  ホストは RAW のオープンを拒否します。そのため ping.x を使う場合は
  GUI 版ホストを使ってください（ping.x の統計表示には FLOAT2.X の
  常駐が必要です）
- sendto / recvfrom のアドレス指定は UDP では有効です
  （TCP では接続済みソケットとして扱います）
- MSG_PEEK などの flags は無視します
- getsockname は bind / connect で確定した実際のローカル側アドレスと
ポートを返します（どちらもしていない場合は 0.0.0.0:0）

ドライバの置き場所（メモリ常駐の入口）は、実在の inetd.x 互換ドライバ
（yunkya2 氏の joynetd など）と同じ作法です。プログラムイメージの先頭に
`"TCP/IP"` の署名を置き、その直後（先頭から8バイト目）が入口で、
関数番号を D0、引数配列へのポインタを A0 に入れて呼ばれ、結果が D0 に返ります。
ソケットの状態構造体（usock 配列）は sock_top で公開し、libsocket が
ノンブロッキング指定のフラグを直接読み書きする約束にも合わせています。

## ビルド

ゲスト側（elf2x68k を使用）:

```sh
cd guest
export PATH=<elf2x68k の bin ディレクトリ>:$PATH
make        # hfsnet.x ができる
```

GUI 版ホストは `gui/` で CMake を使ってビルドします。
Python 版ホストはビルド不要です。

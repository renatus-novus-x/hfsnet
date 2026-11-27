# 管理系 TI（iface / route / DNS / ARP / RIP）実装と検証記録

実装日・検証日: 2026-11-22
検証環境: XEiJ 0.26.10.08（:95 リグ、HFS ブート、フォルダ hfs-hostinfo/TCPPACK）
相方: host/hfsnethost.py
設計根拠: compat-check/MGMT-TI-DESIGN.md（TCPPACKB の network.h と
libnetwork、サンプル各ソースから要求仕様を特定）

## 実装（guest/hfsnet.c）

- TI 6 get_iface_list / TI 7 get_new_iface / TI 8 link_new_iface:
  iface を2つ返す。lp0（127.0.0.1、flags=23<UP,RUNNING,LOOPBACK>、
  mtu 16436）と hf0（ホスト IP、flags=17<UP,RUNNING,NOARP,BROADCAST>、
  mtu 1500）。hf0 のアドレスは常駐時の OP_HOSTINFO 応答で得たホスト IP。
  常駐時に取れなかった場合は、参照時に2秒だけ再問い合わせする。
  ネットマスクはホストから取得できないためクラス別デフォルト。
  netstat が呼ぶ update 関数ポインタにはドライバ内の空関数を入れる。
  カウンタ（raw/ip の送受信）はソケットの送受信成功回数を数える
- TI 9 rt_top / TI 10 rt_lookup / TI 11 rt_lookupb: 経路は3件のみ。
  デフォルト（*def で返す、gateway は台帳値）、hf0 のサブネット、
  127.0.0.0/8。TI 13 rt_add / TI 12 rt_drop はデフォルト経路分だけ
  台帳として受ける（inetdconf +router / -router 用）
- TI 14〜18 dns_add / dns_drop / dns_get / set_domain_name /
  get_domain_name: ゲスト内の台帳（最大4件）。名前解決はホスト側が
  行うため、これは inetdconf の表示・設定のための保持である
- TI 1〜5 ARP: search は NULL、add/del は 0、arp_request は -1。
  **get_arp_table_top は「17本のバケツ配列へのポインタ」を返す**
  （libnetwork の get_arp_array が _top[x] と添字参照する ABI。
  NULL や -1 を返すと arp.x -a が落ちる。当初 NULL を返して
  SIGSEGV（pc:73b8c、従来と同じ場所）になり、ソースで原因を特定した）
- TI 55 rip: 引数 0=ON / 1=OFF / 2=STAT に対し台帳のフラグを読み書き
  する。従来は未実装で -1 が返り、inetdconf に「rip: On」と
  誤表示されていた（実態は Off が正しい）

## 検証結果（XEiJ、画面は shots-hostinfo/）

- IFCONFIG.X hf0 → `hf0: flags=17<UP,RUNNING,NOARP,BROADCAST>` /
  `inet 198.19.0.2 netmask 0xffffff00 broadcast 198.19.0.255`（画面21）
  ※名前は小文字で指定する（大文字 HF0 は "no such interface"。
  inetd.x 系の名前も小文字が正規）
- IFCONFIG.X lp0 → `lp0: flags=23<UP,RUNNING,LOOPBACK>` /
  `inet 127.0.0.1 netmask 0xff000000`（画面22）
- NETSTAT.X -i → lp0 / hf0 の2行が正しい Mtu・Network・Address で
  表示され、update() 呼び出しでも落ちない（画面24）
- NETSTAT.X -r → default（hf0）、127.0.0.0（lp0）、198.19.0.0（hf0）
  の3行（画面24）。Gateway は台帳値 0.0.0.0 のため 0.0.0.0 表示
- NETSTAT.X（引数なし）→ 接続なしで無言終了（画面25）
- ARP.X -a → 空表で無言終了（画面26）。修正前は SIGSEGV だった
- INETDCONF.X → `default router: 0.0.0.0` / `rip: Off` /
  `domain name servers: None` / `domain name: None`（画面28）。
  従来は表示自体が SIGBUS で落ちていた
- INETDCONF.X +router 198.19.0.1 / +dns 198.19.0.53 の設定経路は
  T.BAT 経由で実行し、表示の読み戻しで `default router: 198.19.0.1` /
  `domain name servers: 198.19.0.53` になることを確認（画面38）
  ※リグのキー入力では '+' がゲストに届かない（XEiJ の VNC
  キーマップの制約）ため BAT を使った

## 追加検証（XEiJ、2026-11-22 21:01〜21:10）— ifconfig down の穴の修正

- 発端: iface の関数ポインタは update だけ設定し、残りは NULL
  だった。ifconfig の down は iface の stop ポインタを呼ぶため、
  0番地へのジャンプになる穴があった（ユーザー指摘で修正）
- 修正: stop に UP|RUNNING を落とす実処理を入れ、config /
  update / send / output / input は空関数で埋めた。
  fprint / sprint / fscan / sscan は「ハードウェアアドレスを
  扱えるか」を示す能力ポインタで、呼び出し側（arp.x の -s）が
  NULL を確認してから使う設計のため、NULL のままにした
  （空関数を入れると arp -s が無言で握り潰す形になるため是正）
- 検証ドライバ: guest/hfsnet.x md5 999d35bc5b08dfb0ddfba36c24545c0e
- IFCONFIG.X hf0 down → 落ちずにプロンプトへ戻り、表示は
  `hf0: flags=14<DOWN,NOARP,BROADCAST>` に変化（画面47）
- IFCONFIG.X hf0 up → `hf0: flags=15<UP,NOARP,BROADCAST>`。
  up で立つのは UP ビットのみで、RUNNING は戻らない
  （ifconfig 側の仕様。実スタックではドライバ側が立てる）
- README（日英）に「ゲスト側の設定について」の節を追加し、
  本来の X68000 での設定との本質的な違いを説明した

## 検証結果（XM6 TypeG、2026-11-22 18:50〜18:53）

環境: XM6 TypeG 3.38（:96、WindrvXM で共有フォルダを C: に接続、
ブートは xm6/boot.xdf、CONFIG.SYS で FLOAT2.X 常駐済み）。
相方は host/hfsnethost.py（hfs-xm6/TCPPACK 監視）。
画面は xm6/shots-xm6/mgmt-*.png、動画は compat-check/mgmt-xm6.mp4
（114 秒、ゲスト画面にクロップ済み。素材 mgmt-xm6-raw.mp4）。

- HFSNET.X C:\TCPPACK → installed 行に続き
  `hfsnet: host IP address: 198.19.0.2` を表示（画面 mgmt-04）
- IFCONFIG.X hf0 / lp0、NETSTAT.X -i、NETSTAT.X -r の表示は
  XEiJ と同一（画面 mgmt-08 に集約表示）
- INETDCONF.X → default router 0.0.0.0 / rip Off / DNS None /
  domain None（画面 mgmt-10）
- ARP.X -a → 空表で無言終了（画面 mgmt-10）
- xm6g.ini の [Windrv] は検証用に Enable=1 / Drives=1 /
  Path0=hfs-xm6 へ変更して実施（終了後に復元予定）

## 検証結果（WebX68k、2026-11-22 21:13〜21:18）

環境: WebX68k（vite の開発サーバ 5199、wxdriver 経由で操作）。
相方は host_shim.py（hfs-webx68k/FTPWEBX/TCPPACK 監視）。
画面は shots-webx68k/mgmt-wx-01..05*.png、動画は
compat-check/mgmt-webx.mp4（86 秒、ゲスト画面にクロップ済み。
素材 mgmt-webx68k-raw.mp4）。

- 接続先フォルダの経緯: WebX68k に保存済みのフォルダハンドルは
  FTP 検証時代の FTPWEBX を指しており、権限が失効していたため
  ゲストからは空のドライブに見えていた（CD \TCPPACK が
  「ディレクトリが見つかりません」になった主因）。
  再接続ボタンから権限を再許可（Allow on every visit）して
  FTPWEBX を C: に接続し、その配下に TCPPACK を複製して実施した
- もう1つの主因: `CD \TCPPACK` はカレントドライブ（A:）に対する
  操作になる。先に `C:` でドライブを切り替える必要がある
- HFSNET.X C:\TCPPACK → installed 行に続き
  `hfsnet: host IP address: 198.19.0.2` を表示
- IFCONFIG.X hf0（flags=17、198.19.0.2/24）/ lp0（flags=23、
  127.0.0.1）の表示は XEiJ・XM6 と同一
- NETSTAT.X -i / -r の表示も同一（経路表3行）
- INETDCNF.X → default router 0.0.0.0 / rip Off / DNS None /
  domain None
- ARP.X -a → 空表で無言終了

## 検証結果（ix68k、2026-11-26 17:25〜17:36）

環境: ix68k Release v20261125（Muse VM、Xvfb :93、SCSI HDD ブート、
HOSTDRV 共有フォルダ share/ が D:）。相方は host/hfsnethost.py
（share/TCPPACK 監視、poll 10 ms）。ゲストドライバは管理系検証済み
ビルド md5 999d35bc5b08dfb0ddfba36c24545c0e（表示バージョン 0.5.0）。
画面は ix68k 側 runtime/captures/mgmt-*.png、操作は
runtime/work/mgmt-drive.py（xdotool、日本語キーマップ）。

- HFSNET.X D:\TCPPACK（絶対パス）→ installed 行に続き
  `hfsnet: host IP address: 198.19.0.2` を表示
- IFCONFIG.X hf0 / lp0、NETSTAT.X -i / -r、NETSTAT.X（引数なし）、
  ARP.X -a、INETDCONF.X の表示はすべて XEiJ・XM6・WebX68k と同一
  （hf0 flags=17 / 198.19.0.2/24、経路表3行、router 0.0.0.0 /
  rip Off / DNS None / domain None、ARP は空表で無言終了）
- IFCONFIG.X hf0 down → flags=14<DOWN,NOARP,BROADCAST>、
  up → flags=15<UP,NOARP,BROADCAST>。XEiJ と同一
- INETDCONF.X +router 198.19.0.1 / +dns 198.19.0.53 の設定と
  読み戻しも一致。'+' は xdotool 経由で直接入力できた
  （XEiJ の VNC リグでは BAT 経由が必要だった点のみ相違）
- PING.X 127.0.0.1 と PING.X -c 3 127.0.0.1 は、ホスト側で
  ICMP の OPEN が result=-1 となり、**無言でプロンプトへ戻った**。
  XEiJ / XM6 の旧記録（RESULTS.md、RESULTS-xm6typeg.md）の
  SIGBUS にはならなかった。旧記録は現行ドライバ以前の可能性が
  あり、現行ドライバでの XEiJ 再確認は未実施。ICMP 自体が
  hfsnet の対象外である点は従来どおり
- 午前中の ix68k セッションで IFCONFIG 等が無出力だったのは、
  HFSNET.X を絶対パスで常駐させていなかったためと判明
  （本検証で全項目が正常動作）

## 未検証・残件

- 実機での管理系表示は未確認
- ifconfig の設定系（アドレス書き換え）は表示への反映のみで、
  実通信には影響しない（ホスト側スタックのもの）。TECHNICAL と
  README 既知の限界に明記済み（commit 033b75d）
- MIB（TI 23）は従来どおりダミー表のまま（実カウンタ化は未着手）

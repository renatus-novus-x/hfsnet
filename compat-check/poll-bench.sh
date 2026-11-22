#!/bin/bash
# poll-bench.sh - ホストのポーリング間隔と iperf スループットの関係を測る
# 前提: XEiJ :95 で A:\TCPPACK のプロンプト、hfsnet 常駐済み
# 使い方: poll-bench.sh [間隔ms ...]   既定: 10 5 2 1 0
set -u
cd "$(dirname "$0")"
ROOT=$HOME/workspace/x68k/hfsnet
FOLDER=$PWD/hfs-hostinfo/TCPPACK
OUT=$PWD/poll-bench-results.txt
D="python3 drive95.py"
HOSTPID=""

: > "$OUT"

cpu_ticks() {  # $1=pid -> utime+stime (1/100 s)
  awk '{print $14+$15}' /proc/$1/stat 2>/dev/null || echo 0
}

kill_host() {
  local p
  for p in $(pgrep python3); do
    if tr '\0' ' ' < /proc/$p/cmdline 2>/dev/null | grep -q hfsnethost; then
      kill $p 2>/dev/null
    fi
  done
  sleep 1
}

start_host() {  # $1=interval ms
  kill_host
  rm -f "$FOLDER/HFSREQ.BIN" "$FOLDER/HFSRES.BIN"
  nohup python3 "$ROOT/host/hfsnethost.py" "$FOLDER" "$1" \
    > "poll-bench-host-$1ms.log" 2>&1 &
  sleep 2
  HOSTPID=""
  local p
  for p in $(pgrep python3); do
    if tr '\0' ' ' < /proc/$p/cmdline 2>/dev/null | grep -q hfsnethost; then
      HOSTPID=$p
    fi
  done
  echo "host interval=$1 ms pid=$HOSTPID" | tee -a "$OUT"
}

guest() {  # $1=コマンド(BAT名)
  $D type "$1" > /dev/null
  $D raw 0xff0d > /dev/null
}

wait_exit() {  # $1=pid $2=timeout s
  local t=0
  while kill -0 $1 2>/dev/null; do
    sleep 1; t=$((t+1))
    [ $t -ge $2 ] && { kill $1 2>/dev/null; echo TIMEOUT; return 1; }
  done
  return 0
}

for IV in "$@"; do :; done
[ $# -eq 0 ] && set -- 10 5 2 1 0

for IV in "$@"; do
  start_host "$IV"
  # アイドル時 CPU を 5 秒測る
  T0=$(cpu_ticks $HOSTPID); sleep 5; T1=$(cpu_ticks $HOSTPID)
  echo "interval=$IV idle_cpu_ticks_5s=$((T1-T0))" >> "$OUT"
  for REP in 1 2; do
    # ---- 送信 (ゲスト -> ホスト) ----
    ./iperf-host-linux s 5001 > "poll-bench-send-$IV-$REP.log" 2>&1 &
    SRV=$!
    sleep 1
    C0=$(cpu_ticks $HOSTPID); W0=$(date +%s.%N)
    guest "IPERFC.BAT"
    wait_exit $SRV 45
    C1=$(cpu_ticks $HOSTPID); W1=$(date +%s.%N)
    echo "interval=$IV rep=$REP send: $(cat poll-bench-send-$IV-$REP.log | tr '\n' ' ') host_cpu_ticks=$((C1-C0)) wall=$(echo "$W1 $W0" | awk '{printf "%.1f", $1-$2}')" >> "$OUT"
    sleep 3
    # ---- 受信 (ホスト -> ゲスト) ----
    guest "IPERFS.BAT"
    sleep 6
    C0=$(cpu_ticks $HOSTPID)
    timeout 45 ./iperf-host-linux c 127.0.0.1 5001 10 64 > "poll-bench-recv-$IV-$REP.log" 2>&1
    C1=$(cpu_ticks $HOSTPID)
    echo "interval=$IV rep=$REP recv: $(cat poll-bench-recv-$IV-$REP.log | tr '\n' ' ') host_cpu_ticks=$((C1-C0))" >> "$OUT"
    sleep 5
  done
done

echo "BENCH-DONE" | tee -a "$OUT"

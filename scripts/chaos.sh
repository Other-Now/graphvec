#!/usr/bin/env bash
# Chaos test: 3-member etcd + 2 shards x 2 replicas, client load, faults.
#
#   scripts/chaos.sh <fault> <seconds> <outdir> [--unsafe]
#     fault = kill   kill -9 a random shard leader, restart it 1 s later
#             stop   SIGSTOP a shard leader for 3 s (> lease TTL), then SIGCONT:
#                    the paused leader wakes up deposed -- the fencing case
#             etcd   kill -9 a random etcd member, restart it 2 s later
#             none   no faults (baseline throughput)
#
# Needs: BUILD (dir with shardd, chaos_client), ETCD_BIN (dir with etcd,
# etcdctl), CHECKER (porcupine checker binary). Writes history.jsonl,
# faults.log, client.json, verify.json, check.json, logs/ into outdir.
set -u
FAULT=$1; DUR=$2; OUT=$3; UNSAFE=${4:-}
BUILD=${BUILD:-$HOME/gv-build/rel}
ETCD_BIN=${ETCD_BIN:-$HOME/gv-tools/etcd-v3.5.17-linux-amd64}
CHECKER=${CHECKER:-$BUILD/checker}
TTL=${TTL:-2}
W=$(mktemp -d /tmp/gvchaos.XXXX)
rm -rf "$OUT"; mkdir -p "$OUT/logs"
EPS=127.0.0.1:2379,127.0.0.1:2389,127.0.0.1:2399
CLUSTER=e0=http://127.0.0.1:2380,e1=http://127.0.0.1:2390,e2=http://127.0.0.1:2400
declare -A PID

now() { "$BUILD/chaos_client" now; }  # CLOCK_MONOTONIC, same clock as the history
etcdctl() { "$ETCD_BIN/etcdctl" --endpoints=$EPS "$@"; }

start_etcd() {  # $1 = member index
  local i=$1 c=$((2379 + $1 * 10)) p=$((2380 + $1 * 10))
  "$ETCD_BIN/etcd" --name e$i --data-dir "$W/etcd$i" \
    --listen-client-urls http://127.0.0.1:$c --advertise-client-urls http://127.0.0.1:$c \
    --listen-peer-urls http://127.0.0.1:$p --initial-advertise-peer-urls http://127.0.0.1:$p \
    --initial-cluster $CLUSTER --initial-cluster-state new --initial-cluster-token gv \
    >>"$OUT/logs/etcd$i.log" 2>&1 &
  PID[etcd$i]=$!
}

start_replica() {  # $1 = shard, $2 = replica
  local s=$1 r=$2
  local port=$((7100 + s * 10 + r)) peer=$((7100 + s * 10 + 1 - r))
  "$BUILD/shardd" --shard $s --replica $r --port $port --peer 127.0.0.1:$peer --etcd $EPS \
    --data "$W/s${s}r${r}" --ttl $TTL $UNSAFE >>"$OUT/logs/s${s}r${r}.log" 2>&1 &
  PID[s${s}r${r}]=$!
}

leader_of() { etcdctl get /gv/shard/$1/leader --print-value-only 2>/dev/null | cut -d' ' -f1; }

cleanup() { kill -9 ${PID[@]} 2>/dev/null; kill -CONT ${PID[@]} 2>/dev/null; wait 2>/dev/null; rm -rf "$W"; }
trap cleanup EXIT

for i in 0 1 2; do start_etcd $i; done
for _ in $(seq 50); do etcdctl endpoint health >/dev/null 2>&1 && break; sleep 0.2; done
for s in 0 1; do for r in 0 1; do start_replica $s $r; done; done
for _ in $(seq 100); do [ -n "$(leader_of 0)" ] && [ -n "$(leader_of 1)" ] && break; sleep 0.1; done
echo "leaders: shard0=r$(leader_of 0) shard1=r$(leader_of 1)"

if [ "$FAULT" = khop ]; then  # cross-shard k-hop BFS vs a local BFS; no faults
  "$BUILD/chaos_client" khop --etcd $EPS --shards 2 --keys 5000 --edges 20000 --hops 3 | tee "$OUT/khop.json"
  exit 0
fi
if [ "$FAULT" = zombie ]; then  # deterministic paused-leader test on shard 0; DUR = rounds
  "$BUILD/chaos_client" zombie --etcd $EPS --pids ${PID[s0r0]},${PID[s0r1]} --rounds "$DUR" \
    >"$OUT/zombie.json" 2>"$OUT/zombie.log"
  cat "$OUT/zombie.log" "$OUT/zombie.json"
  exit 0
fi

"$BUILD/chaos_client" run --etcd $EPS --shards 2 --clients 8 --duration "$DUR" \
  --history "$OUT/history.jsonl" >"$OUT/client.json" &
CLIENT=$!

: >"$OUT/faults.log"
END=$(( $(date +%s) + DUR - 4 ))
sleep 3
while [ "$(date +%s)" -lt "$END" ] && [ "$FAULT" != none ]; do
  s=$((RANDOM % 2))
  case $FAULT in
    kill)
      r=$(leader_of $s); [ -z "$r" ] && { sleep 1; continue; }
      kill -9 ${PID[s${s}r${r}]}; echo "$(now) kill $s $r" >>"$OUT/faults.log"
      wait ${PID[s${s}r${r}]} 2>/dev/null
      sleep 1; start_replica $s $r; sleep 4 ;;
    stop)
      r=$(leader_of $s); [ -z "$r" ] && { sleep 1; continue; }
      kill -STOP ${PID[s${s}r${r}]}; echo "$(now) stop $s $r" >>"$OUT/faults.log"
      sleep 3; kill -CONT ${PID[s${s}r${r}]}; echo "$(now) cont $s $r" >>"$OUT/faults.log"; sleep 3 ;;
    etcd)
      i=$((RANDOM % 3))
      kill -9 ${PID[etcd$i]}; echo "$(now) etcdkill - $i" >>"$OUT/faults.log"
      wait ${PID[etcd$i]} 2>/dev/null
      sleep 2
      # rejoin the existing cluster with the same data dir
      c=$((2379 + i * 10)); p=$((2380 + i * 10))
      "$ETCD_BIN/etcd" --name e$i --data-dir "$W/etcd$i" \
        --listen-client-urls http://127.0.0.1:$c --advertise-client-urls http://127.0.0.1:$c \
        --listen-peer-urls http://127.0.0.1:$p --initial-advertise-peer-urls http://127.0.0.1:$p \
        --initial-cluster $CLUSTER --initial-cluster-state existing \
        >>"$OUT/logs/etcd$i.log" 2>&1 &
      PID[etcd$i]=$!; sleep 3 ;;
  esac
done
wait $CLIENT
sleep 3  # let restarted replicas settle before verifying
"$BUILD/chaos_client" verify --etcd $EPS --shards 2 --history "$OUT/history.jsonl" >"$OUT/verify.json"
"$CHECKER" "$OUT/history.jsonl" "$OUT/linearizability.html" >"$OUT/check.json"
cat "$OUT/client.json" "$OUT/verify.json" "$OUT/check.json"

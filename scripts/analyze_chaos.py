#!/usr/bin/env python3
"""Summarise one chaos run directory (from scripts/chaos.sh).

Failover time for a fault on shard s at time t = first successful operation
on shard s that was *called* after t, minus t. The faulted leader cannot
answer anything called after t (it is dead or stopped), so that op was
served by the new leader.

usage: analyze_chaos.py <run dir> [<run dir> ...]   -> prints JSON per run
"""
import json
import os
import statistics
import sys


def summarize(d):
    ops = [json.loads(l) for l in open(os.path.join(d, "history.jsonl"))]
    faults = [l.split() for l in open(os.path.join(d, "faults.log")) if l.strip()]
    by_shard = {0: [], 1: []}
    for o in ops:
        if o["st"] == "ok":
            by_shard[o["s"]].append(o["call"])
    for v in by_shard.values():
        v.sort()
    gaps = []
    for t, kind, shard, _victim in faults:
        if kind not in ("kill", "stop"):
            continue
        t, s = int(t), int(shard)
        nxt = next((c for c in by_shard[s] if c > t), None)
        if nxt is not None:
            gaps.append((nxt - t) / 1e6)
    span = (max(o["ret"] for o in ops) - min(o["call"] for o in ops)) / 1e9
    lat = sorted((o["ret"] - o["call"]) / 1e6 for o in ops if o["st"] == "ok")
    res = {
        "run": os.path.basename(d.rstrip("/")),
        "faults": len(faults),
        "ops_ok": sum(o["st"] == "ok" for o in ops),
        "ops_fail": sum(o["st"] == "fail" for o in ops),
        "ops_info": sum(o["st"] == "info" for o in ops),
        "ok_ops_per_s": round(sum(o["st"] == "ok" for o in ops) / span),
        "latency_ms_p50": round(lat[len(lat) // 2], 2),
        "latency_ms_p99": round(lat[int(len(lat) * 0.99)], 2),
    }
    if gaps:
        res["failover_ms_p50"] = round(statistics.median(gaps))
        res["failover_ms_max"] = round(max(gaps))
    for name in ("verify.json", "check.json"):
        p = os.path.join(d, name)
        if os.path.exists(p) and os.path.getsize(p):
            res.update(json.load(open(p)))
    return res


if __name__ == "__main__":
    for d in sys.argv[1:]:
        print(json.dumps(summarize(d)))

#!/usr/bin/env python3
"""Shrink a non-linearizable history to the first failing window for one key.

usage: bisect_history.py history.jsonl key checker_binary [faults.log]
Finds the shortest prefix (ordered by call time) of that key's operations
that the checker rejects, then prints the operations around its end and
the faults near that time.
"""
import json
import subprocess
import sys
import tempfile


def illegal(ops, checker):
    with tempfile.NamedTemporaryFile("w", suffix=".jsonl", delete=False) as f:
        for o in ops:
            f.write(json.dumps(o) + "\n")
    return subprocess.run([checker, f.name], capture_output=True).returncode == 1


def main():
    path, key, checker = sys.argv[1], int(sys.argv[2]), sys.argv[3]
    ops = [json.loads(l) for l in open(path)]
    ops = sorted((o for o in ops if o["k"] == key), key=lambda o: o["call"])
    lo, hi = 1, len(ops)
    if not illegal(ops, checker):
        print("key is linearizable")
        return
    while lo < hi:
        mid = (lo + hi) // 2
        if illegal(ops[:mid], checker):
            hi = mid
        else:
            lo = mid + 1
    t0 = ops[0]["call"]
    end = ops[lo - 1]
    print(f"first illegal prefix: {lo} ops; last op called at +{(end['call'] - t0) / 1e9:.3f}s")
    for o in ops[max(0, lo - 25):lo]:
        print(f"  c{o['c']} s{o['s']} {o['op']:3} v={o['v']:<12} call=+{(o['call'] - t0) / 1e9:.4f} "
              f"ret=+{(o['ret'] - t0) / 1e9:.4f} {o['st']}")
    if len(sys.argv) > 4:
        for line in open(sys.argv[4]):
            t, *rest = line.split()
            dt = (int(t) - t0) / 1e9
            if abs(dt - (end["call"] - t0) / 1e9) < 6:
                print(f"  fault at +{dt:.3f}s: {' '.join(rest)}")


if __name__ == "__main__":
    main()

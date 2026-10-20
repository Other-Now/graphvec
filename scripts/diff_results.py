#!/usr/bin/env python3
"""Compare gvsh and Memgraph results query by query.

Rows are compared in order when the query has ORDER BY, otherwise as
multisets. Numbers compare with a relative tolerance of 1e-9 (avg() is a
float in both engines, but summation order differs); 2 and 2.0 are equal.

usage: diff_results.py ours.jsonl memgraph.jsonl [report.md]
"""
import json
import math
import sys


def norm(v):
    if isinstance(v, bool) or v is None or isinstance(v, str):
        return v
    if isinstance(v, (int, float)):
        return float(v)
    return v


def same_val(a, b):
    a, b = norm(a), norm(b)
    if isinstance(a, float) and isinstance(b, float):
        return math.isclose(a, b, rel_tol=1e-9, abs_tol=1e-9)
    return a == b


def same_rows(x, y, ordered):
    if len(x) != len(y):
        return False
    if not ordered:
        key = lambda r: json.dumps([round(norm(v), 6) if isinstance(norm(v), float) else v for v in r])
        x, y = sorted(x, key=key), sorted(y, key=key)
    return all(len(a) == len(b) and all(same_val(p, q) for p, q in zip(a, b)) for a, b in zip(x, y))


def main():
    ours = [json.loads(l) for l in open(sys.argv[1])]
    theirs = [json.loads(l) for l in open(sys.argv[2])]
    assert len(ours) == len(theirs), "query counts differ"
    bad, rows_total, ms_ours, ms_theirs = [], 0, 0.0, 0.0
    for a, b in zip(ours, theirs):
        assert a["query"] == b["query"]
        if "error" in a or "error" in b:
            bad.append((a["query"], f"error ours={a.get('error')} memgraph={b.get('error')}"))
            continue
        rows_total += len(a["rows"])
        ms_ours += a.get("ms", 0)
        ms_theirs += b.get("ms", 0)
        if not same_rows(a["rows"], b["rows"], "ORDER BY" in a["query"]):
            bad.append((a["query"], f"ours {len(a['rows'])} rows {a['rows'][:3]} vs memgraph "
                                    f"{len(b['rows'])} rows {b['rows'][:3]}"))
    lines = [f"queries: {len(ours)}  matched: {len(ours) - len(bad)}  mismatched: {len(bad)}  "
             f"rows compared: {rows_total}",
             f"total query time: graphvec {ms_ours:.0f} ms, memgraph {ms_theirs:.0f} ms (client-side, incl. Bolt)"]
    for q, why in bad[:50]:
        lines.append(f"- `{q}`\n  {why}")
    text = "\n".join(lines)
    print(text)
    if len(sys.argv) > 3:
        open(sys.argv[3], "w").write(text + "\n")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Load arxiv into Memgraph over Bolt and run the query file.

Writes the same JSON-lines format as `gvsh --queries` so diff_results.py can
compare them. Every paper gets labels :Paper and :<category>, properties
id/year/subject, and every citation a :CITES relationship -- exactly what
gvsh's loader builds.

usage: memgraph_run.py <arxiv_gv dir> <queries.cypher> <out.jsonl> [bolt uri]
"""
import csv
import json
import sys
import time

from neo4j import GraphDatabase


def batches(rows, n):
    for i in range(0, len(rows), n):
        yield rows[i:i + n]


def main():
    src, qfile, out = sys.argv[1:4]
    uri = sys.argv[4] if len(sys.argv) > 4 else "bolt://localhost:7687"
    drv = GraphDatabase.driver(uri, auth=("", ""))
    for _ in range(60):
        try:
            drv.verify_connectivity()
            break
        except Exception:
            time.sleep(1)
    t0 = time.time()
    with drv.session() as s:
        s.run("STORAGE MODE IN_MEMORY_ANALYTICAL").consume()
        s.run("MATCH (n) DETACH DELETE n").consume()
        s.run("CREATE INDEX ON :Paper(id)").consume()
        by_cat = {}
        with open(f"{src}/nodes.csv") as f:
            r = csv.reader(f)
            next(r)
            for i, y, sub, cat in r:
                by_cat.setdefault(cat, []).append({"id": int(i), "year": int(y), "subject": int(sub)})
        for cat, rows in by_cat.items():
            for b in batches(rows, 5000):
                s.run(f"UNWIND $rows AS r CREATE (:Paper:{cat} {{id: r.id, year: r.year, subject: r.subject}})",
                      rows=b).consume()
        with open(f"{src}/edges.csv") as f:
            r = csv.reader(f)
            next(r)
            edges = [{"s": int(a), "d": int(b)} for a, b in r]
        for b in batches(edges, 10000):
            s.run("UNWIND $rows AS r MATCH (a:Paper {id: r.s}), (b:Paper {id: r.d}) CREATE (a)-[:CITES]->(b)",
                  rows=b).consume()
        n = s.run("MATCH (n) RETURN count(n)").single()[0]
        e = s.run("MATCH ()-[r]->() RETURN count(r)").single()[0]
    print(f"memgraph loaded {n} nodes, {e} edges in {time.time() - t0:.1f}s", file=sys.stderr)

    with open(qfile) as f, open(out, "w") as o, drv.session() as s:
        qi = 0
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            rec = {"q": qi, "query": line}
            t = time.time()
            try:
                res = s.run(line)
                rec["cols"] = list(res.keys())
                rec["rows"] = [list(r.values()) for r in res]
                rec["ms"] = (time.time() - t) * 1000
            except Exception as ex:
                rec["error"] = str(ex)
            o.write(json.dumps(rec) + "\n")
            qi += 1
    print(f"ran {qi} queries", file=sys.stderr)


if __name__ == "__main__":
    main()

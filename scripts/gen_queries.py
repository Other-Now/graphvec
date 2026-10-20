#!/usr/bin/env python3
"""Generate the differential-test query set from the arxiv graph.

20 templates x N instances with random seeds, papers, years, subjects and
categories. Seeds are picked by degree so variable-length expansions stay
bounded (undirected *1..3 through a 13k-citation hub would not). Every
ORDER BY ends on a unique key, so LIMIT is deterministic in both engines.

usage: gen_queries.py <arxiv_gv dir> <out.cypher> [instances_per_template]
"""
import collections
import csv
import random
import sys

TEMPLATES = [
    ("out", "MATCH (a:Paper {{id: {id}}})-[:CITES]->(b) RETURN b.id ORDER BY b.id"),
    ("in_top", "MATCH (a:Paper {{id: {hub}}})<-[:CITES]-(b) WHERE b.year >= {y} "
               "RETURN b.id, b.year ORDER BY b.year DESC, b.id LIMIT 10"),
    ("var_out", "MATCH (a:Paper {{id: {id}}})-[:CITES*1..3]->(b) RETURN count(*) AS paths, count(DISTINCT b) AS ends"),
    ("label_pair", "MATCH (a:{c1})-[:CITES]->(b:{c2}) RETURN count(*)"),
    ("group", "MATCH (a:{c1})-[:CITES]->(b) RETURN b.subject AS s, count(*) AS c ORDER BY c DESC, s LIMIT 5"),
    ("two_hop_cmp", "MATCH (a:Paper {{id: {id}}})-[:CITES]->(b)-[:CITES]->(c) WHERE c.year < a.year - 2 RETURN count(*)"),
    ("triangle", "MATCH (a:Paper {{id: {id}}})-[:CITES]->(b)-[:CITES]->(c), (a)-[:CITES]->(c) "
                 "RETURN b.id, c.id ORDER BY b.id, c.id"),
    ("undirected", "MATCH (a:Paper {{id: {id}}})-[:CITES]-(b) RETURN count(DISTINCT b)"),
    ("var_undirected", "MATCH (a:Paper {{id: {low}}})-[:CITES*1..2]-(b) WHERE b.subject = {s} RETURN count(*)"),
    ("aggs", "MATCH (a:Paper {{id: {id}}})-[:CITES*1..2]->(b) "
             "RETURN min(b.year), max(b.year), sum(b.year), avg(b.year), count(b)"),
    ("label_prop", "MATCH (b:{c1}) WHERE b.year = {y} RETURN count(*)"),
    ("precedence", "MATCH (a:Paper {{id: {id}}})-[:CITES]->(b) WHERE b.year > {y} OR b.subject = {s} AND b.year < 2015 "
                   "RETURN b.id ORDER BY b.id"),
    ("distinct", "MATCH (a:Paper {{id: {id}}})-[:CITES*1..2]->(b) RETURN DISTINCT b.subject AS s ORDER BY s"),
    ("in_list", "MATCH (a:Paper {{id: {hub}}})<-[:CITES]-(b) WHERE b.subject IN [{s}, {s2}, 16] RETURN count(*)"),
    ("mutual", "MATCH (a:Paper {{id: {mutual}}})-[:CITES]->(b)-[:CITES]->(a) RETURN b.id ORDER BY b.id"),
    ("skip_limit", "MATCH (a:Paper {{id: {id}}})-[:CITES*1..2]->(b) RETURN DISTINCT b.id AS x ORDER BY x SKIP 2 LIMIT 5"),
    ("not", "MATCH (a:Paper {{id: {id}}})-[:CITES]->(b) WHERE NOT b.year >= {y} RETURN count(*)"),
    ("label_chain", "MATCH (a:{small})-[:CITES]->(b:{c2})-[:CITES]->(c:{small}) RETURN count(*)"),
    ("var_in", "MATCH (a:Paper {{id: {lowin}}})<-[:CITES*1..2]-(b) RETURN count(*), count(DISTINCT b)"),
    ("by_year", "MATCH (a:{c1}) WHERE a.year >= 2016 RETURN a.year AS y, count(*) AS n ORDER BY y"),
]


def main():
    src, out = sys.argv[1], sys.argv[2]
    per = int(sys.argv[3]) if len(sys.argv) > 3 else 15
    rnd = random.Random(2026)
    cats, subj = {}, []
    with open(f"{src}/nodes.csv") as f:
        r = csv.reader(f)
        next(r)
        for row in r:
            cats.setdefault(row[3], 0)
            cats[row[3]] += 1
            subj.append(int(row[2]))
    outd, ind = collections.Counter(), collections.Counter()
    edges = set()
    with open(f"{src}/edges.csv") as f:
        r = csv.reader(f)
        next(r)
        for s, d in r:
            s, d = int(s), int(d)
            outd[s] += 1
            ind[d] += 1
            edges.add((s, d))
    n = len(subj)
    ids = [i for i in range(n) if 1 <= outd[i] <= 30]
    hubs = [i for i in range(n) if ind[i] >= 50]
    low = [i for i in range(n) if 1 <= outd[i] + ind[i] <= 6]
    lowin = [i for i in range(n) if 1 <= ind[i] <= 15]
    mutual = sorted({s for s, d in edges if (d, s) in edges})
    names = sorted(cats)
    small = [c for c in names if cats[c] < 3000]

    lines = []
    for name, tpl in TEMPLATES:
        for _ in range(per):
            lines.append(tpl.format(
                id=rnd.choice(ids), hub=rnd.choice(hubs), low=rnd.choice(low), lowin=rnd.choice(lowin),
                mutual=rnd.choice(mutual), y=rnd.randint(2008, 2020), s=rnd.randrange(40), s2=rnd.randrange(40),
                c1=rnd.choice(names), c2=rnd.choice(names), small=rnd.choice(small)))
    with open(out, "w") as f:
        f.write("\n".join(lines) + "\n")
    print(f"{len(lines)} queries ({len(TEMPLATES)} templates x {per}); {len(mutual)} papers with mutual citations")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Convert OGB ogbn-arxiv (raw CSVs) into graphvec's load format.

Output directory gets:
  nodes.csv   id,year,subject,category   (category is a label like cs_LG)
  edges.csv   src,dst                     (src cites dst)
  feats.f32   169343 x 128 float32, row i = node i (raw little-endian)

Standard library only, so it runs on a bare CI image.
usage: prep_arxiv.py <ogb arxiv dir> <out dir>
"""
import csv
import gzip
import os
import struct
import sys


def rows(path):
    with gzip.open(path, "rt") as f:
        yield from csv.reader(f)


def main():
    src, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    raw = os.path.join(src, "raw")

    cats = {}
    for r in rows(os.path.join(src, "mapping", "labelidx2arxivcategeory.csv.gz")):
        if r[0] == "label idx":
            continue
        # "arxiv cs na" -> "cs_NA"
        parts = r[1].split()
        cats[int(r[0])] = f"{parts[1]}_{parts[2].upper()}"

    years = [int(r[0]) for r in rows(os.path.join(raw, "node_year.csv.gz"))]
    labels = [int(r[0]) for r in rows(os.path.join(raw, "node-label.csv.gz"))]
    assert len(years) == len(labels)
    with open(os.path.join(out, "nodes.csv"), "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["id", "year", "subject", "category"])
        for i, (y, l) in enumerate(zip(years, labels)):
            w.writerow([i, y, l, cats[l]])

    seen, loops, dups = set(), 0, 0
    with open(os.path.join(out, "edges.csv"), "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["src", "dst"])
        for r in rows(os.path.join(raw, "edge.csv.gz")):
            s, d = int(r[0]), int(r[1])
            if s == d:
                loops += 1
                continue
            if (s, d) in seen:
                dups += 1
                continue
            seen.add((s, d))
            w.writerow([s, d])

    n = 0
    with open(os.path.join(out, "feats.f32"), "wb") as f:
        for r in rows(os.path.join(raw, "node-feat.csv.gz")):
            f.write(struct.pack("<128f", *map(float, r)))
            n += 1
    assert n == len(years)
    print(f"nodes={len(years)} edges={len(seen)} dropped_self_loops={loops} dropped_dups={dups} "
          f"categories={len(cats)}")


if __name__ == "__main__":
    main()

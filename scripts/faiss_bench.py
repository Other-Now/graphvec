#!/usr/bin/env python3
"""FAISS IndexHNSWFlat on SIFT, same CSV columns as tools/ann_bench.

usage: faiss_bench.py --sift DIR [--n 1000000] [--nq 10000] [--M 16] [--efc 200]
                      [--threads 4] [--ef 10,20,...] [--out f.csv]
Build uses `threads` OpenMP threads; search runs single-threaded, one query
at a time, like the C++ harness.
"""
import argparse
import os
import resource
import time

import faiss
import numpy as np


def fvecs(path, n=None):
    a = np.fromfile(path, dtype=np.int32)
    d = a[0]
    a = a.reshape(-1, d + 1)[:, 1:]
    if n:
        a = a[:n]
    return np.ascontiguousarray(a.view(np.float32))


def ivecs(path, n=None):
    a = np.fromfile(path, dtype=np.int32)
    d = a[0]
    a = a.reshape(-1, d + 1)[:, 1:]
    return a[:n] if n else a


def rss_mb():
    with open("/proc/self/statm") as f:
        return int(f.read().split()[1]) * os.sysconf("SC_PAGE_SIZE") / 1048576


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--sift", required=True)
    p.add_argument("--n", type=int, default=1000000)
    p.add_argument("--nq", type=int, default=10000)
    p.add_argument("--M", type=int, default=16)
    p.add_argument("--efc", type=int, default=200)
    p.add_argument("--threads", type=int, default=4)
    p.add_argument("--ef", default="10,20,40,80,160,320")
    p.add_argument("--out")
    a = p.parse_args()

    xb = fvecs(os.path.join(a.sift, "sift_base.fvecs"), a.n)
    xq = fvecs(os.path.join(a.sift, "sift_query.fvecs"), a.nq)
    n, d = xb.shape
    k = 10
    if n == 1000000:
        gt = ivecs(os.path.join(a.sift, "sift_groundtruth.ivecs"), a.nq)[:, :k]
    else:
        flat = faiss.IndexFlatL2(d)
        flat.add(xb)
        _, gt = flat.search(xq, k)

    faiss.omp_set_num_threads(a.threads)
    rss0 = rss_mb()
    t0 = time.perf_counter()
    index = faiss.IndexHNSWFlat(d, a.M)
    index.hnsw.efConstruction = a.efc
    index.add(xb)
    build = time.perf_counter() - t0
    mem = rss_mb() - rss0

    faiss.omp_set_num_threads(1)
    lines = []
    for ef in map(int, a.ef.split(",")):
        index.hnsw.efSearch = ef
        faiss.cvar.hnsw_stats.reset()
        res = np.empty((len(xq), k), dtype=np.int64)
        t = time.perf_counter()
        for i in range(len(xq)):
            _, res[i] = index.search(xq[i : i + 1], k)
        el = time.perf_counter() - t
        hit = sum(len(set(res[i]) & set(gt[i])) for i in range(len(xq)))
        ndis = faiss.cvar.hnsw_stats.ndis / len(xq)
        lines.append(f"faiss,{n},{a.M},{a.efc},{a.threads},{build:.2f},{mem:.0f},{ef},"
                     f"{hit / (len(xq) * k):.4f},{len(xq) / el:.0f},{ndis:.0f}")
    header = "lib,n,M,efc,threads,build_s,index_rss_mb,ef,recall10,qps,dist_per_query"
    if a.out:
        new = not os.path.exists(a.out) or os.path.getsize(a.out) == 0
        with open(a.out, "a") as f:
            if new:
                f.write(header + "\n")
            f.write("\n".join(lines) + "\n")
    else:
        print(header)
        print("\n".join(lines))


if __name__ == "__main__":
    main()

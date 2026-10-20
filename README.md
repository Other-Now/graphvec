# graphvec

A small sharded **property-graph + vector store** in C++20, where each part is checked against an
established system:

| Part | Checked against | Result |
|---|---|---|
| Cypher subset (MATCH / var-length paths / WHERE / aggregates / ORDER BY / LIMIT / CREATE) | **Memgraph 3.12.0**, same 520 queries on the same graph | **520 / 520 identical** (1,665 rows) |
| HNSW index, written from scratch, AVX2 | **hnswlib 0.8.0** and **FAISS** `IndexHNSWFlat` on SIFT1M | same recall at every setting; same-machine A/B: search 0–11% slower than hnswlib, build ~3% faster |
| Filtered vector search (pre / post / in-graph filtering) | exact brute force over the matching set | post-filtering recall falls **0.98 → 0.18 → 0.011** as selectivity goes 100% → 2% → 0.1% |
| etcd-lease leadership, primary-backup replication, fencing tokens | **Porcupine** linearizability checker + lost-write audit under kill -9 / SIGSTOP / etcd-member kills | **0 acked writes lost, every history linearizable**; with fencing switched off, a paused leader serves **20/20 stale reads and loses 20/20 acked writes** |

Dataset: **ogbn-arxiv** (SNAP/OGB): 169,343 papers, 1,166,243 citations, a 128-d embedding per
paper, 40 subject categories. That gives a real graph and real vectors on the same nodes, so
"nearest papers *within two citation hops*" is a meaningful filtered query. SIFT1M is used for the
standard ANN benchmark.

CI numbers come from GitHub Actions `ubuntu-24.04` (AMD EPYC 7763, 4 vCPU). Local numbers come from an
i5-9300H laptop under WSL2. Raw outputs are in [`results/`](results/).

---

## Layout

```
include/graphvec/   graph.hpp store.hpp cypher.hpp hnsw.hpp distance.hpp vector_hook.hpp
                    etcd.hpp net.hpp ring.hpp
src/                graph.cpp (CSR + delta)  store.cpp (WAL, snapshots)  cypher_parser.cpp
                    cypher_exec.cpp (planner + executor)  etcd.cpp  net.cpp
tools/              gvsh (Cypher shell)  ann_bench  filter_bench  shardd  chaos_client  wal_crash
checker/            Porcupine linearizability checker (Go)
scripts/            prep_arxiv.py  gen_queries.py  memgraph_run.py  diff_results.py
                    faiss_bench.py  chaos.sh  analyze_chaos.py  bisect_history.py
```

## 1. Storage

* **CSR adjacency** (out and in) for reads, plus a **per-node delta** for writes, merged by
  `compact()` (counting sort by endpoint, edge-id order preserved so iteration order is stable).
  Nodes have a label bitmask (≤ 64 labels) and a small sorted property vector. There are hash
  indexes on property keys.
* **Write-ahead log**: `[len][crc32][lsn][record]`. Records name labels and keys as strings, so a
  record replays into any graph, including a replica (the replication stream *is* the WAL).
  Snapshots use cdc-store's commit pattern (temp file → fsync → rename → fsync dir). The log is
  truncated after the rename, and records ≤ the snapshot LSN are skipped on replay, so a crash
  between the two replays nothing twice. Recovery cuts a torn tail at the first bad CRC.
* **`wal_crash`**: a child commits as fast as it can and acks each LSN over a pipe, and the
  parent `kill -9`s it at a random 5–150 ms. CI: 300 trials, **301,377 acks, 0 lost, 0 corrupt
  states**. With fsync per commit: 100 trials, 33,553 acks, 0 lost. No trial ever produced a
  torn tail, since kill -9 cannot tear a `write()` that already returned. The torn-tail path is
  covered by a unit test that appends garbage.

## 2. Cypher subset, diffed against Memgraph

Hand-written lexer and recursive-descent parser ([`cypher_parser.cpp`](src/cypher_parser.cpp)).
The planner ([`cypher_exec.cpp`](src/cypher_exec.cpp)) works like this:

* Each pattern starts from the cheapest node by statistics: **index seek** (exact count from the
  index) < **label scan** (label cardinality) < all nodes. It then expands outwards in both
  directions. `MATCH (a:cs_DB)-[:CITES]->(b:cs_LG)` starts from `cs_DB` (2,369 nodes), not from
  `cs_LG`.
* `WHERE` is split into AND-conjuncts, and each one is **pushed down** to the first operator that
  binds all its variables.
* Cypher's **relationship uniqueness** holds across the whole `MATCH`, including both
  var-length hops of `-[*1..2]->()-[*1..2]->` and comma-separated pattern parts.
* Execution is push-based. `LIMIT` without `ORDER BY` stops the scan early, and `ORDER BY … LIMIT
  k` is a top-k partial sort.
* `ORDER BY vec_dist(n, $q) LIMIT k` over a single-node pattern is **rewritten into one filtered
  vector search**: labels and `WHERE` produce the match set, and the vector layer picks prefilter
  or in-graph filtering.

```
gvsh --vectors --explain -e "MATCH (n:cs_DB) WHERE n.year >= 2018
                             RETURN n.id, vec_dist(n, $q) AS d ORDER BY vec_dist(n, $q) LIMIT 5"
NodeByLabelScan(n:cs_DB) est_rows=2369 filter[n.year >= 2018]
VectorTopK(k=5, matches=754, strategy=prefilter)          -> 1.6 ms
```

**Differential test** (`difftest` CI job): `gen_queries.py` instantiates **26 templates × 20**
with random seeds, years, subjects and categories (seeds picked by degree so `*1..3` stays
bounded). Both engines load the same graph: graphvec via `gvsh`, Memgraph 3.12.0 in Docker over
Bolt. The results are diffed (ordered when there is an `ORDER BY`, otherwise as multisets, floats
to 1e-9). Templates cover var-length out/in/undirected, var-length cycles back to a bound start,
two var-length hops in one pattern, uniqueness across comma parts, untyped `*1..3`, triangles,
`DISTINCT`, `IN`, `OR`/`AND` precedence, `NOT`, `SKIP/LIMIT`, min/max/sum/avg/count(DISTINCT),
and arithmetic grouping keys. **520/520 match.** The first 300-query version also matched on its
first run, which is why the harder templates were added. The test has not found a semantic bug
yet.

## 3. HNSW vs hnswlib vs FAISS (SIFT1M)

[`hnsw.hpp`](include/graphvec/hnsw.hpp):
* Flat level-0 neighbour array (M0 = 2M), per-node upper levels, and the paper's neighbour-selection
  heuristic (Alg. 4).
* Level drawn from a hash of the id, so no shared RNG is needed.
* Thread-safe insertion with a mutex per node; the global lock is held only by an insert that
  raises the top level.
* Epoch-tagged visited set.
* AVX2+FMA / NEON L2 kernels ([`distance.hpp`](include/graphvec/distance.hpp)), with the same
  structure as gemm-lab's axpy.

`ann_bench` links hnswlib's headers, so both indexes are timed by the same harness, compiler and
flags. FAISS runs through its Python wheel with the same protocol: 4 build threads, then
single-threaded search one query at a time, 10,000 queries, recall@10 against the shipped ground
truth.

CI, M=16, efConstruction=200 ([`results/ci/ann_sift1m_run1.csv`](results/ci/), `_run2`):

| ef | graphvec recall / QPS | hnswlib recall / QPS | FAISS recall / QPS |
|---|---|---|---|
| 10 | 0.7086 / 33,446 | 0.7078 / 27,148 | 0.7244 / 15,341 |
| 40 | 0.9270 / 12,313 | 0.9273 / 10,895 | 0.9360 / 7,448 |
| 80 | 0.9741 / 6,639 | 0.9746 / 6,120 | 0.9798 / 4,595 |
| 160 | 0.9929 / 3,479 | 0.9931 / 3,339 | 0.9949 / 2,656 |
| 320 | 0.9980 / 1,760 | 0.9983 / 1,833 | 0.9987 / 1,466 |
| build / RSS | 133 s / 689 MB | 144 s / 728 MB | 178 s / 639 MB |

Recall tracks hnswlib to the third decimal at every `ef` and at every build setting in the sweep
(M ∈ {8, 16, 32}, efC ∈ {100, 200, 400}): at ef=80, 0.9185 / 0.9198 for M=8 and 0.9873 / 0.9873
for M=32. That says the graphs are equivalent.

**QPS needs care.** Each matrix job runs on a different runner, and a second CI run moved QPS by
~10% (graphvec 6,639 → 5,984, hnswlib 6,120 → 5,739 at ef=80). So the cross-job table shows
"same speed", not "faster than hnswlib". The `ann_ab` job alternates both libraries on one
machine, twice each ([`ann_sift1m_same_runner_ab.csv`](results/ci/ann_sift1m_same_runner_ab.csv)):

| ef | graphvec QPS (run 1 / 2) | hnswlib QPS (run 1 / 2) | gap |
|---|---|---|---|
| 10 | 25,949 / 26,814 | 26,681 / 26,617 | ~0–3% |
| 80 | 5,124 / 5,215 | 5,815 / 5,730 | **~10%** |
| 160 | 2,932 / 2,627 | 3,105 / 3,066 | 5–14% |
| 320 | 1,489 / 1,482 | 1,618 / 1,573 | ~7% |
| build | 150 s / 148 s | 155 s / 153 s | graphvec ~3% faster |

So **hnswlib's search is ~10% faster at mid/high ef, at identical recall and identical distance
work**. The gap is per-distance memory cost, not graph quality. The likely cause, not yet
verified with counters: hnswlib stores each node's level-0 links *next to its vector* in one
block and prefetches the next candidate's links and visited tag. graphvec keeps links and
vectors in separate arrays, which costs an extra cache miss per expanded node. The laptop
couldn't resolve this: an early local run showed 28% and repeated A/B runs there showed ~0%. Its
noise floor (host memory pressure) was bigger than the effect. One runner and alternating runs
were needed to see it.

## 4. Filtered search: where post-filtering collapses

`filter_bench`: 500 query papers, top-10, "papers similar to P" (P itself excluded), recall
against the exact filtered answer. Strategies ([`vector_hook.hpp`](include/graphvec/vector_hook.hpp)):

* **prefilter**: brute force over the matching ids. Exact; cost ∝ matches.
* **postfilter**: plain HNSW search for `ef` candidates, then drop non-matching ones.
* **inline**: the HNSW traversal walks every node but admits only matching nodes to the result
  set (hnswlib's filter-functor approach).

CI ([`results/ci/filter.csv`](results/ci/filter.csv)), uncorrelated random filters:

| selectivity | postfilter ef=100 | postfilter ef=1000 | inline ef=100 (recall / QPS / dists) | prefilter QPS |
|---|---|---|---|---|
| 100% | 0.984 | 0.998 | 0.984 / 5,933 / 1.8k | 121 |
| 10% | 0.828 | 0.998 | 0.998 / 872 / 11k | 1,469 |
| 2% | **0.185** | 0.991 | 0.999 / 212 / 36k | 8,457 |
| 0.5% | 0.043 | 0.453 | 0.998 / 63 / 83k | 48,708 |
| 0.1% | **0.011** | 0.126 | 0.996 / **21** / **147k of 169k** | 183,508 |

* **Post-filtering collapses** once matches are rarer than ~1 in `ef`. A 10× over-fetch only moves
  the cliff by 10×.
* **Inline filtering keeps recall ≥ 0.99 but degenerates into a full graph walk.** When few nodes
  match, the result set never fills, the beam's stopping rule never fires, and at 0.1% it visits
  147k of 169k nodes, i.e. slower than brute force.
* **So the right plan switches on cardinality**: prefilter below the crossover, inline above it.
  The crossover is ~20% selectivity on the CI EPYC (prefilter still wins at 16%: 950 vs 559 QPS)
  and ~10% on the laptop. It depends on hardware, which is why the Cypher hook's threshold is a
  parameter (16k matches here).
* **Correlation matters.** The graph predicate "within 2 citation hops of P"
  (`MATCH (p {id:$P})-[:CITES*1..2]-(q)`, mean 2.05% of nodes, 2.8 ms to evaluate in Cypher)
  gives post-filtering 0.48 recall vs 0.18 for a random filter of the same 2% selectivity:
  neighbours in the citation graph are also neighbours in embedding space. Category filters
  (unrelated to the query paper) behave like random ones.

## 5. Sharding, replication, fencing

* Keys are hash-sharded over a consistent-hash ring (from cdc-store) across **2 shards × 2
  replicas**.
* Edges live with their source shard, and a k-hop expand fans out `NBRS` RPCs to the owning
  shards. `khop` check: 50 seeds × 3 hops, 884 RPCs (411 cross-shard), **0 mismatches** vs a local
  BFS.
* A **3-member etcd** cluster (Raft) holds the leader key and ISR per shard. The C++ client talks
  to etcd's JSON gateway over raw HTTP, so there is no gRPC dependency, and it fails over between
  members.
* **Leadership**:
  * A replica in the ISR campaigns with a transaction: "leader key absent AND ISR unchanged" →
    put the leader key on a 2 s lease.
  * The key's `create_revision` is the **fencing token**; every new leadership gets a larger one.
* **Writes**:
  * The leader ships the WAL record to the backup and waits for ACK, then commits and acks.
  * If the backup is unreachable, the leader removes it from the ISR with a transaction guarded on
    *its own token*, so a deposed leader cannot do that.
  * The backup rejects records with a token lower than one it has seen (`FENCED`).
  * A rejoining replica takes a full snapshot copy and is then re-added to the ISR.
* **Reads**: the leader confirms with a linearizable etcd read that its token is still current.

Chaos (`scripts/chaos.sh`, CI, 120 s each, 8 clients doing PUT/GET on 10 hot keys plus write-once
keys). Each history is checked by **Porcupine** (per-key register model; timeouts are treated as
"may have happened") and audited for lost acknowledged writes:

| fault | faults | ok ops | acked write-once keys | lost | linearizable | failover |
|---|---|---|---|---|---|---|
| kill -9 the shard leader | 23 | 261,621 | 39,540 | **0** | yes (39,570 keys) | p50 2.23 s, max 2.54 s |
| SIGSTOP the leader 3 s (> lease) | 19 | 226,566 | 34,055 | **0** | yes | 3.02 s seen by clients* |
| kill -9 an etcd member | 23 | 441,435 | 66,556 | **0** | yes | n/a |

\*Election itself takes ~2.2 s (the zombie test measures it directly). Clients with 1 s timeouts
re-resolve at +1 s and +2 s, both before the new leader exists, so they only reach it after +3 s.

**The fencing experiment.** Random SIGSTOPs with fencing disabled (`--unsafe`) found **nothing**
(886,571 ops, linearizable). The paused leader's overdue keepalive fires first on SIGCONT, and it
steps down ~1 ms later, before any client reaches it. So `chaos_client zombie` builds the bad
case on purpose:
1. Hold a connection to the leader.
2. Pause it past its lease and wait for the new leader.
3. Commit `key = 2` through the new leader.
4. Send `GET key` and `PUT key 3` to the paused one, then resume it.

| CI | rounds | stale reads | writes acked by the zombie | acked writes lost |
|---|---|---|---|---|
| fencing on | 19 (1 skipped by the full-ISR guard) | 0 | 0 | 0 |
| fencing off | 20 | **20** | **20** | **20** |

(Locally: fenced 20/20 rounds clean, unfenced 10/10 stale + lost.)

**The price**: the per-request etcd round trip is the dominant cost. Safe kill runs do 2.2k ok
ops/s at p50 1.7 ms; the same run unfenced does 26.7k at p50 0.15 ms (CI). Leader leases with a
bounded clock-skew assumption would remove most of that cost; that is not built.

## 6. Profiling: the one optimization

`perf record -g` on `gvsh --repeat 5` with a GROUP BY over all 1.17M citations. Flame graphs:
[before](results/local/perf/agg_before.svg), [after](results/local/perf/agg_after.svg). The
aggregation sink was dominated by malloc/free: a fresh key vector per row, and
`unordered_map::emplace` allocating a node **even when the group already exists**. On top of that,
a full `stable_sort` of 169k groups ran to return 3 rows. The fix: reuse one key buffer, `find()`
before inserting, and `partial_sort` when there is a LIMIT. Medians of 3 alternating runs of the
old and new binaries (local):

| query | before | after |
|---|---|---|
| `RETURN b.subject, count(*) ORDER BY …` (40 groups, allocation fix only) | 288 ms | **189 ms** (1.52×) |
| `RETURN a.id, count(*) ORDER BY cites DESC LIMIT 3` (169k groups) | 331 ms | **196 ms** (1.69×) |

All 520 difftest queries return identical results before and after.

## Things that went wrong (and what they taught)

1. **Every key "non-linearizable".** The first kill -9 run failed Porcupine on all 10 hot keys.
   `scripts/bisect_history.py` shrank key 0's history to its first illegal prefix. The cause was an
   op whose return came **0.54 s before its call**: WSL2 steps `CLOCK_REALTIME` when it resyncs
   with the Windows host. Linearizability is defined on real-time order, so the history must use
   `CLOCK_MONOTONIC` (and so must the fault log). The system was fine; the measurement was wrong.
2. **The unfenced control found nothing**, and the first zombie test didn't either. The client's
   timed-out request was still queued in the paused leader's socket, and on resume the leader
   served it *first*, using up the ~2 ms window before the probe. A fresh connection per step
   fixed the harness. Random fault injection is a poor way to hit narrow windows, and a targeted
   test hits this one 20/20.
3. **Postfilter 8× slower than inline at identical distance counts** (572 vs 4,804 QPS). In
   isolation it ran at 4.8k. The benchmark kept a private 169k-id list per query: 340 MB on a
   laptop with < 1 GB free, so the phase after each big brute-force scan paid page faults. Sharing
   static predicates fixed it (572 → 3,258 locally).
4. **A CI zombie job hung for 25 minutes.** A round started while the previous zombie was still
   re-syncing, so the ISR held only the leader. Pausing it left no eligible replica, which is the
   correct, unavailable behaviour, but the harness waited forever. It now waits for a full ISR
   and bounds every wait.
5. **Unfenced chaos jobs were cancelled mid-run** on 4-vCPU runners: an unthrottled cluster
   starved the runner agent. They now run under `nice`.

## Not built (honest scope)

* **Not Raft.** Shard replication is primary-backup with an etcd-managed ISR, so consensus is
  delegated to etcd. With 2 replicas, losing the only in-sync replica makes the shard unavailable
  (no unclean election, same trade-off as Kafka's `unclean.leader.election=false`).
* There is no Kafka ingestion.
* fsync is off by default: commits survive kill -9 but not power loss; `--fsync` exists and is
  crash-tested.
* The distributed layer is a key/value + edge API (`PUT/GET/EDGE/NBRS`) with a client-side k-hop
  BFS. Full Cypher runs single-node only, and there is no distributed query planner.
* The Cypher subset has no OPTIONAL MATCH, WITH, UNWIND, MERGE, SET/DELETE, path variables or
  relationship properties, and returning a node returns its internal id. Labels are capped at 64.
* There is no delete in the graph store, and the HNSW index is in-memory only (not persisted, no
  deletes).

## Reproduce

```bash
cmake -S . -B build && cmake --build build -j
./build/unit_tests && ./build/wal_crash --trials 100
# data
curl -LO http://snap.stanford.edu/ogb/data/nodeproppred/arxiv.zip && python3 -m zipfile -e arxiv.zip .
python3 scripts/prep_arxiv.py arxiv arxiv_gv
./build/gvsh --data arxiv_gv --explain -e "MATCH (a:Paper {id: 1000})-[:CITES*1..3]->(b) RETURN count(*)"
./build/filter_bench --data arxiv_gv --out filter.csv
# SIFT1M from ftp://ftp.irisa.fr/local/texmex/corpus/sift.tar.gz
./build/ann_bench --sift sift --lib mine && ./build/ann_bench --sift sift --lib hnswlib
# chaos (needs etcd v3.5 binaries and `go build` in checker/)
BUILD=build ETCD_BIN=etcd-v3.5.17-linux-amd64 CHECKER=build/checker scripts/chaos.sh kill 60 out/kill
scripts/chaos.sh zombie 20 out/zombie && scripts/chaos.sh zombie 20 out/zombie-unsafe --unsafe
```

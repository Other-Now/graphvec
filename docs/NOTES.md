# graphvec: interview notes

Study sheet. Every number here is in `results/` or the README; re-read those before quoting.

## 30-second pitch

"I built a small sharded graph + vector database in C++20 to learn the three things I hadn't
touched: graph query engines, vector search and consensus-backed replication. Each part is checked
against a real system. The Cypher subset returns identical results to Memgraph on 520 generated
queries. My HNSW matches hnswlib's recall on SIFT1M at every setting and is within ~10% of its speed. The
replication layer uses etcd leases and fencing tokens, and Porcupine says every chaos history is
linearizable with zero lost acknowledged writes. When I switch fencing off, a paused leader serves
stale reads and loses acked writes 20 times out of 20."

## The three best stories

### 1. "Every key failed the linearizability check", and it was the clock
- **Symptom:** the first kill -9 chaos run failed Porcupine on all 10 hot keys.
- **Method:** wrote `bisect_history.py` to find the shortest failing prefix of one key's history,
  then printed the ops around it.
- **Finding:** a GET returned a value whose PUT was *called later*, and that PUT's return time was
  0.54 s *before* its call time.
- **Root cause:** WSL2 steps `CLOCK_REALTIME` when it resyncs with the Windows host.
  Linearizability is about real-time order, so wall-clock timestamps can manufacture violations.
- **Fix:** `CLOCK_MONOTONIC` everywhere (history, fault log, server logs). The same run was then
  clean.
- **Lesson:** in distributed testing, validate the measurement before the system. This is also why
  real systems don't trust wall clocks for ordering, and why leases need a bounded-skew assumption.

### 2. Fencing tokens: random chaos found nothing, a targeted test found everything
- **Design:** the leader key's etcd `create_revision` is the fencing token. The backup rejects
  lower tokens. ISR changes are etcd transactions guarded on the leader's token. Reads confirm the
  token with a linearizable etcd read.
- **Control:** `--unsafe` turns all of that off. Random SIGSTOP chaos: 886k ops, still
  linearizable. Why? On SIGCONT the overdue keepalive fails within ~1 ms and the zombie steps
  down; meanwhile every client has already re-routed to the new leader through etcd.
- **First zombie attempt also showed nothing.** A client's earlier timed-out request was still
  queued in the paused leader's socket, and the zombie served *that* first, using up the window.
  A fresh connection per step fixed it.
- **Deterministic zombie test:** pause the leader past its lease → new leader commits key=2 →
  send GET + PUT to the zombie → resume it. Unsafe: 20/20 stale reads, 20/20 writes acked by the
  zombie and then lost when it re-synced. Safe: 0/0/0.
- **Talking points:** this is the "GC pause / long STW" split-brain from Kleppmann's fencing
  article. A lease alone isn't enough; the *resource* (here, the backup) must check the token.

### 3. Filtered vector search: where each strategy breaks
- Post-filtering recall goes 0.98 (no filter) → 0.83 (10%) → 0.18 (2%) → 0.011 (0.1%). A 10×
  over-fetch (ef=1000) just moves the cliff 10× further out.
- In-graph filtering keeps recall ≥ 0.99, but at 0.1% it visits 147k of 169k nodes: the result
  set never fills, so the beam never stops. It ends up slower than brute force.
- Brute force over the matching set is exact and wins below a crossover: ~20% selectivity on CI
  hardware, ~10% on my laptop. So planners (Qdrant, Weaviate, pgvector's iterative scans) switch
  strategy on estimated cardinality, and the threshold should be calibrated per machine.
- Correlation: a *graph* predicate (2 citation hops from the query paper) gave post-filtering
  0.48 recall vs 0.18 for a random filter of the same 2% size. Citation neighbours are
  embedding neighbours.

## Other measured stories (shorter)
- **Benchmark page faults:** postfilter measured 8× slower than inline at identical distance
  counts. In isolation it was fine. The harness kept a 169k-id list per query (340 MB) on a laptop
  with < 1 GB free. Shared predicates: 572 → 3,258 QPS.
- **"How much slower than hnswlib?" needed three tries.** Local first run: 28%. Local repeats:
  ~0% (the laptop's noise floor). Cross-job CI: ±10% runner noise. The same-runner alternating
  A/B on CI: **hnswlib ~10% faster at ef 80–320**, identical recall, graphvec builds ~3% faster.
  Likely cause (unverified): hnswlib co-locates level-0 links with the vector and prefetches the
  next candidate's links and visited tag; mine keeps them in separate arrays, so there's an extra
  miss per expansion. Next step: perf stat L1/LLC misses per query with `ann_bench --perf-ctl`,
  then try the interleaved layout.
- **Cypher GROUP BY (perf flame graph):** `unordered_map::emplace` allocates a node even when the
  key exists, plus a per-row key vector, plus a full sort for LIMIT 3. find-before-insert, a
  reused buffer and partial_sort: 288 → 189 ms and 331 → 196 ms.
- **CI hang:** the zombie harness waited forever when the ISR held only the leader being paused.
  That is the correct unavailability (no unclean election), but the harness had no bounded wait.

## Likely questions

**Why primary-backup + etcd instead of Raft?** It's smaller to get right, and it's how Kafka
(ISR in ZooKeeper/KRaft) and many systems split the work: consensus for *metadata* (who leads,
who's in sync), plain replication for data. The trade-off is that with 2 replicas, losing the only
in-sync replica means unavailability; I chose that over unclean election. Raft would give majority
quorums (tolerating 1 of 3 down without that trade-off) at the cost of log matching, terms and
election timeouts.

**Why is failover ~2.2 s?** The lease TTL is 2 s plus a 100 ms watch poll. A lower TTL fails over
faster but triggers false failovers on GC/IO pauses. Clients saw 3 s on SIGSTOP because their 1 s
timeout re-resolved twice before the new leader existed.

**How do you make reads cheaper?** Every read does an etcd linearizable read: 2.2k vs 26.7k ops/s
unfenced. Options:
- Leader leases with bounded clock drift: serve reads locally while `now < lease_start + TTL −
  max_drift`.
- Read-index, i.e. confirm leadership with the backup instead of etcd.
- Batch the leadership confirmation across concurrent reads.

**What does Porcupine actually check?** It searches for a sequential order of the operations,
consistent with each op's [call, return] interval, that a register model accepts. Ops whose
outcome is unknown (timeouts) can take effect at any time after their call, or never; definite
failures (NOTLEADER) are dropped. It's NP-complete in general, so it partitions by key.

**What was hard in the Cypher semantics?** Relationship uniqueness across the whole MATCH
(var-length paths, multiple pattern parts), three-valued logic (`NULL OR NULL` is null and filters
the row out), count() on empty input returning one row, ties under LIMIT (the generator always
ends ORDER BY on a unique key). The test did *not* find a bug. I say that plainly and explain the
harder templates I added because of it.

**HNSW internals you can explain:**
- Levels are drawn as `-ln(U)·mL` with mL = 1/ln(M).
- Search descends greedily on the upper layers, then runs a beam of width ef on level 0.
- The neighbour-selection heuristic keeps a candidate only if it is closer to the base than to
  every neighbour kept so far. That keeps edges spread across directions and preserves navigability.
- M0 = 2M.
- Concurrency: a per-node mutex on neighbour lists, copy-under-lock during build, and the global
  lock only for raising the top level.

**What would you do next?** Raft for shard replication, checked with the same Porcupine harness;
lease-based local reads; a filtered-HNSW variant that keeps the graph connected under selective
filters (ACORN-style neighbour expansion); librdkafka ingestion with offsets in the WAL; HNSW
persistence.

## Numbers to have ready
- arxiv: 169,343 nodes, 1,166,243 edges, loads in ~0.6 s.
- difftest: 520/520 vs Memgraph 3.12.0, 1,665 rows, 26 templates.
- SIFT1M, M16/efC200, ef=80: recall 0.9741 (hnswlib 0.9746, FAISS 0.9798). Same-runner A/B:
  hnswlib ~10% faster search, mine ~3% faster build. RSS 689 / 728 / 639 MB.
- Filtered: postfilter 0.18 recall at 2%, 0.011 at 0.1%. Inline visits 147k/169k at 0.1%.
  Crossover ~20% (CI).
- Chaos: kill 39,540 acked write-once keys, 0 lost, linearizable, failover p50 2.23 s. Also stop
  and etcd-member kills, all clean.
- Zombie: 0/0/0 fenced vs 20/20/20 unfenced.
- WAL: 300 kill -9 trials, 301,377 acks, 0 lost.
- Profiling: GROUP BY 288 → 189 ms, 331 → 196 ms.

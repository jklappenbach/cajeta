# cajeta-rag — retrieval-augmented generation: finding, ranking, and assembling context

## 1. Definition

### 1.1 Purpose

`cajeta-docs` turns documents into structured, provenance-carrying pieces.
`cajeta-ml-v3` produces embeddings. `nucleo.frame` will hold a vector index
(`nucleo-frame-spec` §9.4.3). Nothing connects them: there is no retrieval, no
ranking, no context assembly, and no citation.

`dev.cajeta.rag` is that layer. **Docs understands and decomposes text; rag
finds it.**

Finding is not only similarity. A corpus carries structure — code imports and
calls, HTML links, citations — and that structure is a retrieval signal, often a
stronger one than token overlap. §9–§11 make it first-class and extensible.

### 1.2 What this owns, and what it does not

| Concern | Owner |
|---|---|
| Parsing, structure, chunking, tokenization, TF-IDF | `cajeta-docs` |
| Per-document extraction — symbols, imports, links | `cajeta-docs` |
| Embedding models | `cajeta-ml-v3` |
| Vector index (HNSW/IVF) | `nucleo.frame` §9.4.3 |
| Graph algorithms (PageRank, traversal) | `dev.cajeta.graph` |
| **BM25, inverted index, hybrid fusion, reranking, assembly, citation** | **this spec** |
| **Corpus-level reference resolution, the relation graph, symbol index, graph-aware ranking** | **this spec** (§9–§11) |
| Generation (calling a model) | the application |

The seam between docs and rag is **local versus global**. Docs says "this file
imports `foo.bar`" — a per-document fact available from one parse. Only the
corpus knows what `foo.bar` resolves to, so resolution, the graph, and every
signal over it live here (§10.2).

**This library does not generate.** It produces a ranked, cited,
budget-respecting context. What consumes that is the caller's business — which
keeps rag usable for search and question-answering alike, not only for LLM
prompting.

### 1.3 Scope

Lexical retrieval (BM25 + inverted index); dense retrieval over the vector
index; hybrid fusion; reranking; diversity; context assembly under a token
budget; citation; retrieval evaluation. Plus the structural half: a pluggable
enhancement seam (§9), the typed relation graph with graph-aware ranking and
neighbourhood expansion (§10), and code as a first-class corpus — symbol index,
syntactic chunking requirements, and per-language processors (§11).

### 1.4 Non-goals

- **1.4.1** **Calling a language model.** §1.2.
- **1.4.2** Prompt templating and agent orchestration.
- **1.4.3** Training or fine-tuning retrievers.
- **1.4.4** Document parsing or chunking — `cajeta-docs`. That includes the
  per-language *parsing* behind §11: this spec states what a code corpus
  requires and owns everything corpus-level, but the parsers themselves are
  readers, and readers belong to docs.
- **1.4.7** **Compiling or type-checking code.** §11's processors extract
  structure. Name resolution is best-effort and lexical; full semantic
  resolution is a compiler's job and is not attempted.
- **1.4.8** **Fetching anything.** Links are resolved within the corpus only
  (§11.4.2); this library opens no connections.
- **1.4.5** Implementing the vector index — `nucleo.frame`.
- **1.4.6** Conversation memory and multi-turn state.

### 1.5 Systems

`dev.cajeta.docs` (chunks + provenance + tokenization + per-language
extraction), `dev.cajeta.ml` (embeddings via ml-v3; `Metrics`),
`dev.cajeta.graph` (the relation graph and its algorithms, §10.4),
`cajeta.nucleo.frame` (vector index, corpus tables),
`cajeta.nucleo.sparse.CsrMatrix` (postings), `cajeta.math` (distance kernels),
`cajeta.collection`, `dev.cajeta.unit`.

---

## 2. Feature: the corpus

- **2.1** When a corpus from `cajeta-docs` output is built, each entry carries
  its text, its embedding where present, and its **full provenance** (`cajeta-
  docs` §6.2).
- **2.2** When documents are added or removed, the indexes update or the
  limitation is stated — a corpus that can only be built once is a real
  constraint and must not be discovered at runtime.
- **2.3** When a document is re-ingested at a new version (`cajeta-docs` §6.4),
  its stale entries can be retired without rebuilding the whole corpus.
- **2.4** When a corpus is persisted, it round-trips — re-embedding a large
  corpus to restart is not acceptable.
- **2.5** When entries carry metadata, it is filterable at query time (§5.5).

---

## 3. Feature: lexical retrieval

Moved here from the earlier text-library draft: scoring a query against a corpus
is *finding*, not text processing.

- **3.1** When scoring with BM25, **the variant is named** — Okapi BM25, BM25+,
  or BM25L — with `k1` and `b` exposed and their defaults documented. Lucene
  and `rank_bm25` disagree; "BM25" alone is not a specification.
- **3.2** When an inverted index is built, term → posting list is available
  with document frequencies and per-document term counts.
- **3.3** When a query runs, the result is the top-`k` without scanning the
  corpus — the point of an inverted index.
- **3.4** When documents vary greatly in length, BM25's length normalization
  applies. This is precisely what it offers over TF-IDF and must be tested with
  a mixed-length corpus.
- **3.5** When a query term is absent, it contributes nothing rather than
  erroring.
- **3.6** When a query is tokenized, the **same analysis chain** used to index
  is applied, from `cajeta-docs` §8. A query analyzed differently from the
  corpus silently retrieves badly.

> §3.2's postings may be able to reuse or generalize `cajeta.search.ngram.Index`,
> which already implements deduplicated postings for fuzzy key lookup. Check
> before building a second one — see §9.2.

---

## 4. Feature: dense retrieval

- **4.1** When a query is embedded and searched, the result is the nearest
  entries by vector similarity via `nucleo.frame`'s index.
- **4.2** When a similarity is chosen, cosine and inner product are available,
  using `cajeta.math`'s kernels.
- **4.3** When the corpus is small, exact search is available — an approximate
  index on a thousand vectors is needless machinery and harder to debug.
- **4.4** When an approximate index is used, its recall trade-off is
  configurable and **its approximate nature is explicit at the call site**, not
  buried in configuration.
- **4.5** When query and corpus embeddings come from different models, it is
  detected and refused — mismatched embedding spaces produce confident
  nonsense, which is the worst failure mode available.

---

## 5. Feature: hybrid retrieval

- **5.1** When hybrid is retrieved, lexical and dense results are fused into
  one ranking.
- **5.2** When results are fused, **reciprocal rank fusion** is available and
  is the default, because it needs no score calibration between two
  incomparable scales.
- **5.3** When results are fused by weighted score instead, the normalization
  applied to each scale is stated — raw BM25 and cosine scores are not
  comparable, and naive weighting silently favours whichever has the larger
  range.
- **5.4** When retrieval runs, **which retriever contributed** each result is
  visible, so a bad ranking is diagnosable.
- **5.5** When a query filters by metadata, filtering is applied **during**
  retrieval rather than after, so a filtered query still returns `k` results
  instead of however many survive.

---

## 6. Feature: reranking and diversity

- **6.1** When reranking runs, a second-stage scorer reorders the top-`n`
  candidates, and the two-stage shape (cheap wide recall, expensive narrow
  precision) is the documented pattern.
- **6.2** When a cross-encoder from `cajeta-ml-v3` is used, it plugs in as a
  reranker; when none is available, a lexical reranker is used.
- **6.3** When results are near-duplicates, **maximal marginal relevance** is
  available to trade relevance against diversity, with the trade-off parameter
  exposed.
- **6.4** When several chunks come from the same document, per-document
  contribution can be capped — otherwise one long document crowds out the
  corpus.
- **6.5** When adjacent chunks are both retrieved, they can be merged back into
  their contiguous span using `cajeta-docs` provenance, rather than delivered
  as fragments with a seam in the middle.

---

## 7. Feature: context assembly

- **7.1** When context under a **token budget** is assembled, the budget is
  respected, counted with the model's own tokenizer (`cajeta-docs` §10.10).
- **7.2** When results exceed the budget, the truncation policy is explicit —
  drop lowest-ranked, or truncate within a result — never silent.
- **7.3** When context is assembled, each piece carries an attributable
  reference back to its source location.
- **7.4** When ordering matters, it is chosen — by rank, or restored to
  document order, which reads better when several chunks come from one source.
- **7.5** When nothing clears the relevance floor, that is reported as **"no
  relevant context"** rather than returning the least-bad results. A confident
  answer built on irrelevant context is worse than an admission of ignorance.

---

## 8. Feature: citation and evaluation

- **8.1** When a context is produced, every piece resolves to document, page,
  and character range (`cajeta-docs` §6.2). **An uncitable result is
  indistinguishable from a fabricated one.**
- **8.2** When citations are needed for display, they render from provenance
  without re-reading the source document.
- **8.3** When retrieval against labelled relevance is evaluated, **recall@k**,
  **precision@k**, **MRR**, and **NDCG@k** are available.
- **8.4** When configurations are compared, the same evaluation runs across
  lexical, dense, and hybrid so the choice rests on measurement rather than
  intuition.
- **8.5** When evaluation runs, per-query results are available, not only
  aggregates — a mean hides the queries that fail completely.


## 9. Feature: retrieval enhancement — the extension seam

Token similarity is one signal. A real corpus carries others: code imports and
calls, HTML links, citations, folder layout, authorship, recency. This section
is the seam that lets any of them participate in ranking. §10 and §11 are the
built-in instances, not special cases — they are written against this seam like
anything a caller adds.

- **9.1** When a corpus is built, **relation extractors** can be registered per
  format, each emitting typed edges from a document to what it references
  (§10.1).
- **9.2** When retrieval runs, **signals** can be registered that participate in
  ranking using anything the corpus holds — the relation graph, metadata, or a
  caller's own data.
- **9.3** When a signal is registered, it declares which of three kinds it is:
  a **filter** (removes candidates), a **scorer** (reorders them), or an
  **expander** (adds candidates the retrievers never returned, §10.6). The three
  compose differently and a single "hook" would hide that.
- **9.4** When several signals contribute, **each one's contribution to a
  result's final score is visible** (§5.4). A blended score with no attribution
  cannot be debugged, and a bad ranking is the failure mode that matters.
- **9.5** When a custom signal is added, no built-in retriever changes and no
  existing behaviour shifts — a corpus with no registered signals ranks exactly
  as §3–§6 specify.
- **9.6** When a signal throws or times out, it is reported and skipped rather
  than failing the query — a degraded ranking beats no answer, but a silent
  degradation does not.
- **9.7** When a signal needs corpus-wide state, it is computed once at index
  time and reused, not recomputed per query.
- **9.8** When an expander adds a candidate, that candidate still carries full
  provenance (§8.1). **No signal may introduce an uncitable result.**

---

## 10. Feature: the relation graph

Documents reference each other. Code imports and calls; HTML links; papers cite.
Those edges are a retrieval signal at least as strong as token overlap, and in
code they are frequently stronger — the answer is often a function the query
never names.

- **10.1** When documents are ingested, extractors emit **typed, directed
  edges** — `imports`, `calls`, `links-to`, `cites`, or a caller's own type —
  each carrying the source location that produced it, so an edge is as citable
  as a chunk.
- **10.2** When an edge names a *reference* rather than a resolved document,
  resolution happens at **corpus level** — only the corpus knows what
  `foo.bar` or `../index.html` points at. Extraction is per-document; resolution
  is not.
- **10.3** When a reference does not resolve, it is kept as a **dangling edge**
  with its raw target rather than dropped. An unresolved import is information —
  it names an external dependency — not an error.
- **10.4** When the graph is built, it is a `dev.cajeta.graph` graph. This
  library does not implement a second graph (§12.6).
- **10.5** When documents are ranked by structural importance, **PageRank** over
  the relation graph is available as a scorer — the standard way to turn "many
  things point at this" into a ranking. See §12.7: `dev.cajeta.graph` has
  eigenvector centrality but not PageRank today.
- **10.6** When a result is retrieved, **neighbourhood expansion** can pull in
  its graph neighbours — the callees a function needs to be intelligible, the
  page a link points at.
- **10.7** When expansion runs, it is bounded by **depth and by token budget**
  (§7.1), and expanded results are **marked as expanded** rather than presented
  as direct hits. A caller must be able to tell what was asked for from what was
  inferred.
- **10.8** When edges are typed, expansion selects by type — follow `calls` but
  not `links-to`.
- **10.9** When the graph is traversed, direction is respected and **both
  directions are available**: callers and callees answer different questions,
  and so do inbound and outbound links.
- **10.10** When a corpus has no relations, every feature here is inert and
  retrieval degrades cleanly to §3–§6. **The graph is an enhancement, never a
  requirement.**

---

## 11. Feature: code as a first-class corpus

Code is the sharpest test of §9 and §10: token similarity alone retrieves badly
over source, because names repeat, bodies look alike, and the relevant unit is a
definition rather than a passage.

### 11.1 The symbol index — exact resolution, not similarity

- **11.1.1** When source is indexed, a **symbol index** maps qualified names to
  their **definition sites**, so a query naming a symbol resolves to its
  definition exactly. This is a third retriever beside lexical (§3) and dense
  (§4), not a tuning of either.
- **11.1.2** When a symbol has several definitions — overloads, or the same name
  in different scopes — **all** are returned with their scopes, never one chosen
  arbitrarily.
- **11.1.3** When a symbol is requested, its **references** are available as
  well as its definition, since "where is this used" and "where is this defined"
  are different questions.

### 11.2 Chunking on syntactic boundaries

`cajeta-docs` §10 owns the chunking mechanism. These are the requirements a code
corpus places on it.

- **11.2.1** When code is chunked, splits fall on **function, method, and class
  boundaries** — never mid-body.
- **11.2.2** When a language admits one public class per file (Java, C#), the
  file is the natural unit and the chunker treats it as such. When a language
  has no classes (C) or does not centre them (Go, Rust, Python, JavaScript),
  function, method, and impl-block boundaries are used instead. **The unit is a
  property of the language, not a global setting.**
- **11.2.3** When a chunk is a function, it carries its enclosing context —
  file, module or namespace, enclosing type, and signature — so an isolated
  chunk stays interpretable, mirroring the heading path in `cajeta-docs` §10.5.
- **11.2.4** When a single function exceeds the chunk budget, the policy is
  explicit, prefers statement boundaries, and the resulting chunks still name
  the function they came from.
- **11.2.5** When a doc comment precedes a definition, it is chunked **with**
  it. The prose describing a function is usually the best lexical match for a
  natural-language question about it, and separating them destroys that.

### 11.3 Language processors

- **11.3.1** When a language processor runs, it emits **definitions,
  references, imports, and doc comments**. Those become §11.1's symbol index and
  §10's edges — one extraction pass feeds both.
- **11.3.2** When the library ships, default processors cover **Cajeta, C, C++,
  C#, Java, Python, JavaScript, and TypeScript**. Processors are hand-written
  (§12.8), so the list is deliberately bounded: eight languages that are
  maintainable rather than sixteen that are not. Every other language reaches
  the corpus through §11.3.4's text fallback, which is a supported outcome, not
  a gap. Adding a language later is purely additive.
- **11.3.3** When **Cajeta** source is indexed, the processor uses the
  compiler's own front end rather than approximating it — cajeta is the one
  language where an exact parse is already available, and an approximation would
  be indefensible.
- **11.3.4** When a language has **no** processor, a generic fallback indexes it
  as text with line-level provenance. An unsupported language degrades; it never
  breaks the corpus.
- **11.3.5** When a processor cannot parse a file — a syntax error, a partial
  edit, an unsupported dialect — it **recovers and indexes what it could**,
  reporting the failure. Real corpora contain files that do not compile, and
  refusing them loses the repository.
- **11.3.6** When a processor is added by a caller, it registers through §9.1
  like any other extractor, with no change to this library.

### 11.4 Markup relations

- **11.4.1** When **HTML** is indexed, `href` and `src` targets become
  `links-to` edges, resolved against the document's base URI (§10.2).
- **11.4.2** When a link is followed for expansion, only corpus-internal targets
  are used. **This library never fetches a URL** — that is a network operation
  the corpus does not own.
- **11.4.3** When **Markdown** is indexed, its links and cross-references are
  extracted the same way, so a document set built from Markdown carries the same
  graph as one built from HTML.

---

## 12. Open questions (resolve at plan time)

- **12.1** *(resolved 2026-08-01 — a view.)* The corpus is a view over a
  `nucleo.frame.Table`, not its own storage. The frame already handles columnar
  storage, filtering, and persistence — which §12.3's decision now depends on —
  and `cajeta-dqe` makes a distributed corpus nearly free as a result.
- **12.2** *(action, not a decision.)* Before building §3.2's postings,
  **read `cajeta.search.ngram.Index`** and determine whether it generalizes.
  Different purpose — character grams for fuzzy keys versus term frequencies
  over documents — but the same structure. This is the mistake the distance
  kernels nearly made (roadmap §5.1). Outcome recorded in the plan either way.
- **12.3** *(resolved 2026-08-01 — persistent from the start.)* Indexes are
  **on-disk from v1**, not in-memory with persistence retrofitted. RAG over a
  real corpus needs it, and retrofitting storage under a working index is
  disruptive in a way that adding it up front is not.

  This is a materially larger build and it is why rag sits **last** in the order
  (roadmap §6). It leans on §12.1: the corpus is a `nucleo.frame` view, so
  columnar persistence is inherited rather than invented. What must still be
  designed here is persistence of the *indexes* — postings, the vector index,
  the symbol index, and the relation graph — including their invalidation under
  §2.3's incremental re-ingest (§12.10).
- **12.4** *(resolved 2026-08-01 — seam only.)* Reranking ships the seam plus a
  **lexical reranker**, not a default cross-encoder. Shipping a model means
  shipping weights and a pretrained-import dependency; §6.2 already lets a
  `cajeta-ml-v3` cross-encoder plug in when one exists.
- **12.6** *(resolved 2026-08-01 — use `dev.cajeta.graph`.)* The relation graph
  is a `dev.cajeta.graph` graph. Typed edges are added **there** rather than
  building a second graph here — the same discipline as §12.2. Follow-on for
  that spec: edges currently carry attributes, not types, so a typed-edge
  representation is a small addition it must absorb.
- **12.7** *(resolved 2026-08-01 — added upstream.)* `dev.cajeta.graph` had no
  PageRank, only undamped eigenvector centrality. **PageRank is now
  `ml-graph-analytics` §4.4.1–4.4.3**, with rank-sink redistribution explicitly
  required — without teleportation, leaf utilities with no outbound edges absorb
  the score, which is the common case in a dependency graph. Nothing to
  implement here.
- **12.8** *(resolved 2026-08-01 — hand-written.)* The §11.3 processors are
  **hand-written in Cajeta**. No tree-sitter binding, no C dependency, no
  vendored grammars to version. The cost is accepted and is real: each language
  is permanent maintenance, and the language list narrows accordingly (§11.3.2).

  **Consequence:** §11.3.2's sixteen-language list is not achievable this way
  and has been cut to eight — Cajeta, C, C++, C#, Java, Python, JavaScript,
  TypeScript. Everything else reaches the corpus through §11.3.4's text
  fallback, which is a supported outcome rather than a gap. Adding a language
  later is additive and needs no change here.
- **12.9** *(resolved 2026-08-01 — a retriever.)* The symbol index is a third
  retriever beside lexical and dense, fused through §5's ranking. An exact
  symbol hit must be able to outrank everything else, which a filter cannot
  express.
- **12.10** **Still open — must be specified before implementation.** Does the
  corpus graph survive incremental re-ingest (§2.3)? Edges from a retired
  document must be retired with it, and dangling edges into it re-dangled. §12.3
  makes this sharper, not easier: with persistent indexes the stale state
  survives a restart, so a bug here is durable rather than transient. This is
  the likeliest source of a stale-graph defect and the one open item in this
  spec that is not a plan-time detail.
- **12.5** *(resolved 2026-08-01 — keep it, thin.)* §7 assembly stays here, but
  stays deliberately thin and unopinionated about output format. It is the least
  reusable part of the library and must not grow into a templating system.

---

## 13. Acceptance criteria (spec-level)

- **13.1** Every returned piece resolves full provenance (§8.1), asserted by
  test — the property citation depends on.
- **13.2** BM25 is verified against a hand-computable example small enough to
  check on paper, since implementations disagree (§3.1).
- **13.3** BM25 ranks a short relevant document above a long padded one (§3.4).
- **13.4** Query analysis matches corpus analysis (§3.6) — verified by a query
  whose terms would be tokenized differently under a mismatched chain.
- **13.5** Mismatched embedding spaces are **refused**, not scored (§4.5).
- **13.6** Metadata filtering during retrieval returns `k` results where post-
  filtering would return fewer (§5.5).
- **13.7** Hybrid fusion beats both single retrievers on a labelled set (§8.4)
  — if it does not, the fusion is wrong and shipping it would be worse than
  either alone.
- **13.8** Assembly never exceeds the token budget, counted with the model's
  own tokenizer (§7.1).
- **13.9** An empty result set is reported as "no relevant context" rather than
  the least-bad results (§7.5).
- **13.10** Evaluation reports per-query results, not only aggregates (§8.5).
- **13.11** A corpus with **no registered signals ranks identically** to one
  built before §9 existed (§9.5) — the enhancement seam is additive, proven by
  test rather than asserted.
- **13.12** Every result added by an expander carries full provenance (§9.8),
  asserted by the same test that covers §13.1.
- **13.13** Each signal's contribution to a final score is recoverable (§9.4),
  verified by reconstructing a ranking from its parts.
- **13.14** An unresolved import survives as a dangling edge naming its raw
  target (§10.3) — verified with a corpus that imports something outside it.
- **13.15** Code chunks never split mid-function (§11.2.1), asserted across
  every shipped language processor, not only the easy ones.
- **13.16** A file that does not parse still contributes what it could, and the
  failure is reported (§11.3.5) — verified with a deliberately broken source
  file in each language.
- **13.17** A doc comment and the definition it describes land in the same chunk
  (§11.2.5).
- **13.18** On a code corpus, symbol-index plus graph signals beat lexical-plus-
  dense alone on a labelled set (§8.4). If they do not, §9–§11 are not earning
  their complexity and should be cut.

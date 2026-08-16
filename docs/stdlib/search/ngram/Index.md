# Index\<T\>

`cajeta.search.ngram.Index` — an n-gram inverted index for fuzzy candidate
generation. Maps key strings to stored values of type `T` and answers
`candidates(query)`: every value whose key shares at least one n-gram with the
query, deduplicated. This is a recall-oriented prefilter — a superset the
caller (or [`fuzzy.Matcher`](../fuzzy/Matcher.md)) then scores with an edit
distance. Gram extraction is lowercased for recall; a key shorter than `n` is
indexed under itself as a single gram. `add` is O(len(key)); `candidates`
visits only the postings of the query's grams (sub-linear in the key count).

```cajeta
Index<int32> idx = heap Index<int32>();           // trigrams (n = 3)
idx.add("apple", 1);
idx.add("zebra", 2);
ArrayList<int32> hits #= idx.candidates("aple");   // [1] — shares "ple"
```

## Methods

| Signature | |
|---|---|
| `Index()` ⚑ | Construct a trigram index (n = 3) |
| `Index(int32 gramSize)` ⚑ | Construct an index with the given gram size |
| `void add(String key, T value)` | Index `value` under every n-gram of `key` |
| `#ArrayList<T> candidates(String query)` | Every value whose key shares at least one n-gram with `query`, deduplicated; empty when nothing shares a gram |

⚑ = `@EntryPoint`

## See also

- [Matcher](../fuzzy/Matcher.md) — combines this prefilter with edit-distance scoring
- [Distance](../distance/Distance.md) — the scoring half
- Source: [`runtime/src/cajeta/search/ngram/Index.cajeta`](../../../../runtime/src/cajeta/search/ngram/Index.cajeta)

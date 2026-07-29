# Matcher\<T\>

`cajeta.search.fuzzy.Matcher` — typo-tolerant key→value lookup: composes an
[`ngram.Index`](../ngram/Index.md) (recall prefilter) with
[`Distance.damerau`](../distance/Distance.md) (scoring) and a length-scaled
threshold, returning results ranked closest-first. The index uses bigrams
(n = 2): a single adjacent transposition (the common typo `Fiel`→`File`)
destroys every trigram of a short key but leaves a shared bigram, so bigrams
keep recall on exactly the typos `damerau` scores as distance 1. Ties rank by
key, so identical inputs always produce identical output — exact matches
(distance 0) rank first.

```cajeta
Matcher<int32> m = heap Matcher<int32>();
m.add("File", 10);
m.add("Edit", 20);
ArrayList<Match<int32>> hits = m.match("Fiel");   // [File, distance 1]
int32 top = hits.get(0).value();                  // 10
```

## Methods

| Signature | |
|---|---|
| `Matcher()` ⚑ | Construct an empty matcher (bigram index for transposition recall) |
| `void add(String key, T value)` | Index `value` under `key`, remembering the key for later scoring |
| `#ArrayList<Match<T>> match(String query)` | Every stored value whose key is within the length-scaled edit-distance threshold, ranked by ascending distance then key |

Each result is a `Match<T>` — `value()` (the stored value), `key()` (the key
it was indexed under), `distance()` (edit distance from the query; 0 = exact).
A `Match` owns a copy of its key, so a result list outlives the `Matcher`.

⚑ = `@EntryPoint`

## See also

- [Index](../ngram/Index.md) — the n-gram prefilter behind `match`
- [Distance](../distance/Distance.md) — the scoring functions
- Source: [`runtime/src/cajeta/search/fuzzy/Matcher.cajeta`](../../../../runtime/src/cajeta/search/fuzzy/Matcher.cajeta)

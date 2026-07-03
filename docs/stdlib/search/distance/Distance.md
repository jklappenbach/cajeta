# Distance

`cajeta.search.distance.Distance` — string edit-distance functions, the
scoring half of fuzzy matching. Both are pure, symmetric in their arguments,
and allocate only `O(min-axis)` working rows. `damerau` is optimal string
alignment (OSA): an adjacent transposition costs 1, so
`damerau("flie", "file") == 1` where `levenshtein` is 2 — the right, cheap
model for typo-tolerant lookup. Distances are computed over the string's byte
surface (no Unicode collation).

```cajeta
int32 d1 = Distance.levenshtein("kitten", "sitting");   // 3
int32 d2 = Distance.damerau("flie", "file");            // 1 — transposition
```

## Methods

| Signature | |
|---|---|
| `static int32 levenshtein(String a, String b)` ⚑ | Classic edit distance (insert/delete/substitute, each cost 1); two rolling rows, O(len(b)) space |
| `static int32 damerau(String a, String b)` ⚑ | OSA distance: like `levenshtein` but an adjacent transposition costs 1; three rolling rows, O(len(b)) space |

⚑ = `@EntryPoint`

## See also

- [Matcher](../fuzzy/Matcher.md) — composes `damerau` with an n-gram prefilter
- [Index](../ngram/Index.md) — the recall half of fuzzy matching
- Source: [`runtime/src/cajeta/search/distance/Distance.cajeta`](../../../../runtime/src/cajeta/search/distance/Distance.cajeta)

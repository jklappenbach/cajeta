# cajeta.search

This section measures edit distance, fuzzy matching and n-gram indexing.

| Page | |
|---|---|
| [Distance](distance/Distance.md) | String edit distances |
| [Matcher](fuzzy/Matcher.md) | Typo-tolerant key lookup over an n-gram prefilter |
| [Index](ngram/Index.md) | The n-gram inverted index |

## Standing

Geometric mean 7x behind over 5 benchmarks, no wins. The n-gram index is within 3x to 5x of a Rust HashMap; the distance functions 5x to 8.5x behind rapidfuzz through `byteAt`; the fuzzy matcher 26x behind nucleo-matcher with no top-k cut-off.

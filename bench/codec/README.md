# cajeta.codec and dev.cajeta.codec

This section measures JSON on the nativejson-benchmark corpus, CSV, Base64 and Base32, and DEFLATE from the codec library.

| Page | |
|---|---|
| [JSON](json/Json.md) | The JSON reader, binder, DOM and writer, measured on the nativejson-benchmark corpus the way that suite measures: tokenize to end of document, bind with skipping, build a DOM, serialize, round-trip, and the JSON_checker conformance set |
| [CSV](csv/Csv.md) | The CSV reader and writer on a generated 100 MB file with quoted fields and embedded commas |
| [Base64](Base64.md) | Base64 encode and decode, read against the SIMD implementations as well as the standard libraries |
| [Base32](Base32.md) | Base32 encode and decode |
| [Deflate (dev.cajeta.codec)](Deflate.md) | DEFLATE and INFLATE from the codec library: the pure-cajeta streaming path that permessage-deflate uses, and the native one-shot path that gzip and content-coding use |

## Standing

Geometric mean 11x behind over 20 benchmarks, no wins. Tokenizing and bind-skip JSON are within 2x of serde_json; the DOM, serializer, Base64 and CSV are 4x to 40x behind the fast libraries and 300x behind simdutf on Base64 encode. The deflate rows did not run. The Base64 gap is simdutf's SIMD path; the rest is not yet investigated.

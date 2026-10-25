# cajeta.codec and dev.cajeta.codec

JSON on the nativejson-benchmark corpus, CSV, Base64 and Base32, and DEFLATE from the codec library.

| Page | |
|---|---|
| [JSON](json/Json.md) | The JSON reader, binder, DOM and writer, measured on the nativejson-benchmark corpus the way that suite measures: tokenize to end of document, bind with skipping, build a DOM, serialize, round-trip, and the JSON_checker conformance set |
| [CSV](csv/Csv.md) | The CSV reader and writer on a generated 100 MB file with quoted fields and embedded commas |
| [Base64](Base64.md) | Base64 encode and decode, read against the SIMD implementations as well as the standard libraries |
| [Base32](Base32.md) | Base32 encode and decode |
| [Deflate (dev.cajeta.codec)](Deflate.md) | DEFLATE and INFLATE from the codec library: the pure-cajeta streaming path that permessage-deflate uses, and the native one-shot path that gzip and content-coding use |

## Standing

Written after the first published run.

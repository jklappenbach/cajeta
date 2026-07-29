# Csv

`cajeta.codec.csv.Csv` — typed CSV facade: the `Csv.parse<T>` entry point that
binds CSV rows to a declared type `T` (or `T[]` for the whole file). Like
`Json.parse<T>`, this is a Tier-1 synthesizer: the compiler walks `T`'s
declared fields and emits a per-`T` header→field bind over `CsvReader` at the
call site — no runtime reflection, no intermediate value tree. The header row
names the columns; each header is matched to a field of `T` (by field name, an
`@CsvColumn("name")` rename, a class-level `@CsvNamingStrategy`, or an
`@CsvAlias`). Columns with no matching field are skipped; a field with no
matching column is left default unless marked `@CsvRequired`, which fails
loud. Supported field types: `int32` / `int64` / `float64` / `boolean` /
`String`. If the synthesizer does not engage, the fallback body throws
`CsvParseException` rather than returning a silently default-zeroed result.

```cajeta
import cajeta.codec.csv.Csv;

public class Trade {
    public int32 id;
    public float64 price;
}

public class CsvExample {
    public void run() {
        String text = "id,price\n1,9.5\n2,3.25\n";
        Trade[] trades = Csv.parse<Trade[]>(text);
        float64 first = trades[0].price;   // 9.5
    }
}
```

## Methods

| Signature | |
|---|---|
| `static T parse<T>(int8[] bytes, int64 length)` ⚑ | Tier-1 templated parse: bind CSV `bytes` (first `length` valid) to `T` |
| `static T parse<T>(String s)` ⚑ | Tier-1 `String` convenience — forwards to the byte-buffer variant |

⚑ = `@EntryPoint`

## See also

- [Json](../json/Json.md) — the JSON Tier-1 synthesizer this mirrors
- [Base64](../Base64.md) — the other `cajeta.codec` codec
- Source: [`runtime/src/cajeta/codec/csv/Csv.cajeta`](../../../../runtime/src/cajeta/codec/csv/Csv.cajeta)

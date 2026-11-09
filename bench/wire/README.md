# cajeta.wire

This section measures wire codecs and the zero-copy struct view against protobuf, FlatBuffers and Cap'n Proto.

| Page | |
|---|---|
| [Encoder](Encoder.md) | The bidirectional wire codec protocol for a T, and the zero-copy struct view that reads a wire record without decoding it. cajeta.wire ships the interfaces. The measured codec is a generated protobuf-wire-format implementation whose byte count equals protobuf's own, produced from the same record description every competitor uses |
| [SchemaEncoder](SchemaEncoder.md) | The schema-driven encoder. protobuf and Cap'n Proto compile their schemas ahead of time, so they are measured through their runtime-schema paths, and Avro through GenericDatum |
| [CompressStream](CompressStream.md) | A compressing wire stream, round-tripped |

## Standing

Geometric mean 2.2x behind over 6 benchmarks, 2 wins and 4 losses. The schema encoder wins both rows by 3x to 4x; the field encoder is 10x to 37x behind bincode and FlatBuffers; the zlib round trip 2.6x behind flate2.

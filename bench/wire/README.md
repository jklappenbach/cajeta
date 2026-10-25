# cajeta.wire

Wire codecs and the zero-copy struct view against protobuf, FlatBuffers and Cap'n Proto.

| Page | |
|---|---|
| [Encoder](Encoder.md) | The bidirectional wire codec protocol for a T, and the zero-copy struct view that reads a wire record without decoding it. cajeta.wire ships the interfaces; the measured codec is a generated protobuf-wire-format implementation whose byte count equals protobuf's own, produced from the same record description every competitor uses |
| [SchemaEncoder](SchemaEncoder.md) | The schema-driven encoder. protobuf and Cap'n Proto compile their schemas ahead of time, so they are measured through their runtime-schema paths; Avro through GenericDatum |
| [CompressStream](CompressStream.md) | A compressing wire stream, round-tripped |

## Standing

Written after the first published run.

// C++ wire/ competitors (bench/wire/Encoder.md, SchemaEncoder.md, CompressStream.md):
// protobuf v36.2 (generated WireRecord; DynamicMessage for the schema rows),
// FlatBuffers v25.12.19 (object API; one WireBatch buffer for the view read; the
// driver compiles record.fbs under namespace benchwire_fb, since protobuf's
// generated WireRecord already lives in benchwire),
// Cap'n Proto v1.5.0 (generated builders/readers; DynamicStruct for the schema
// rows), zlib and libzstd streams. The 100-field record and every expected
// constant come from bench/scripts/gen_wire.py (schema/expect.tsv, path in
// $WIRE_EXPECT). Pool of 1024 records; N = 1e6 encodes/decodes (smoke 1e5),
// M = 1e6 view records (smoke 1e5), 64 MiB stream (smoke 8 MiB).
#include "row.h"
#include "wire_fields.h"

#include "record.pb.h"
#include "record_generated.h"
#include "record.capnp.h"

#include <capnp/dynamic.h>
#include <capnp/message.h>
#include <capnp/serialize.h>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/dynamic_message.h>
#include <zlib.h>
#include <zstd.h>

#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <sstream>

static volatile int64_t g_sink = 0;
static const int POOL = 1024;

#define KIND_F64 0
#define KIND_I32 1
#define KIND_I64 2
static double vf(int r, int k) { return r * 0.5 + k * 0.25; }
static int32_t vi(int r, int k) { return (r * 31 + k * 17) % 65536; }
static int64_t vl(int r, int k) { return (int64_t)(r + 1) * ((int64_t)k * k * k + 17); }

static std::map<std::string, long long> load_expect() {
    std::map<std::string, long long> m;
    std::string text;
    if (!row::read_file(row::env("WIRE_EXPECT", "expect.tsv"), text)) return m;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        auto t = line.find('\t');
        if (t != std::string::npos) m[line.substr(0, t)] = std::stoll(line.substr(t + 1));
    }
    return m;
}

// ---- per-format fill / checksum through the X-macro field list ----
#define PB_SET_F64(k) m.set_f##k(vf(r, k));
#define PB_SET_I32(k) m.set_f##k(vi(r, k));
#define PB_SET_I64(k) m.set_f##k(vl(r, k));
#define PB_SET(k, kind) PB_SET_##kind(k)
static void pb_fill(benchwire::WireRecord& m, int r) { WIRE_FIELDS(PB_SET) }
#define PB_SUM_F64(k) s += (int64_t)(m.f##k() * 4.0);
#define PB_SUM_I32(k) s += m.f##k();
#define PB_SUM_I64(k) s += m.f##k();
#define PB_SUM(k, kind) PB_SUM_##kind(k)
static int64_t pb_sum(const benchwire::WireRecord& m) { int64_t s = 0; WIRE_FIELDS(PB_SUM) return s; }

#define FB_SET_F64(k) t.f##k = vf(r, k);
#define FB_SET_I32(k) t.f##k = vi(r, k);
#define FB_SET_I64(k) t.f##k = vl(r, k);
#define FB_SET(k, kind) FB_SET_##kind(k)
static void fb_fill(benchwire_fb::WireRecordT& t, int r) { WIRE_FIELDS(FB_SET) }
#define FB_SUM_F64(k) s += (int64_t)(t.f##k * 4.0);
#define FB_SUM_I32(k) s += t.f##k;
#define FB_SUM_I64(k) s += t.f##k;
#define FB_SUM(k, kind) FB_SUM_##kind(k)
static int64_t fb_sum(const benchwire_fb::WireRecordT& t) { int64_t s = 0; WIRE_FIELDS(FB_SUM) return s; }

#define CP_SET_F64(k) b.setF##k(vf(r, k));
#define CP_SET_I32(k) b.setF##k(vi(r, k));
#define CP_SET_I64(k) b.setF##k(vl(r, k));
#define CP_SET(k, kind) CP_SET_##kind(k)
static void cp_fill(WireRecord::Builder b, int r) { WIRE_FIELDS(CP_SET) }
#define CP_SUM_F64(k) s += (int64_t)(v.getF##k() * 4.0);
#define CP_SUM_I32(k) s += v.getF##k();
#define CP_SUM_I64(k) s += v.getF##k();
#define CP_SUM(k, kind) CP_SUM_##kind(k)
static int64_t cp_sum(WireRecord::Reader v) { int64_t s = 0; WIRE_FIELDS(CP_SUM) return s; }

#define KIND_OF_F64 KIND_F64
#define KIND_OF_I32 KIND_I32
#define KIND_OF_I64 KIND_I64
#define KIND_ENTRY(k, kind) KIND_OF_##kind,
static const int KINDS[101] = {-1, WIRE_FIELDS(KIND_ENTRY)};

static void fixed_record(int r, unsigned char* p) {
    for (int k = 1; k <= 100; k++) {
        if (KINDS[k] == KIND_I32) { int32_t v = vi(r, k); std::memcpy(p, &v, 4); p += 4; }
        else if (KINDS[k] == KIND_I64) { int64_t v = vl(r, k); std::memcpy(p, &v, 8); p += 8; }
        else { double v = vf(r, k); std::memcpy(p, &v, 8); p += 8; }
    }
}

int main() {
    const bool smoke = row::env("CAJETA_BENCH_SMOKE", "0") == "1";
    const char* tag = smoke ? "smoke" : "full";
    const long long N = smoke ? 100000 : 1000000;
    const long long M = smoke ? 100000 : 1000000;
    int warmup = row::env_int("PROFILE_WARMUP", 1);
    int trials = row::env_int("PROFILE_TRIALS", 5);
    if (trials < 1) trials = 1;
    auto ex = load_expect();
    auto E = [&](const std::string& k) { return ex.count(k) ? ex[k] : -1LL; };
    const std::string sz = smoke ? std::string("; smoke size N=1e5 in place of 1e6") : std::string();
    const std::string pbv = std::string("v36.2 @2c74169");
    const std::string fbv = std::string("v25.12.19 @7e16302");
    const std::string cpv = std::string("v1.5.0 @373e61e");

    // ---------------- protobuf (generated) ----------------
    std::vector<benchwire::WireRecord> pbpool(POOL);
    for (int r = 0; r < POOL; r++) pb_fill(pbpool[r], r);
    {
        long long total = 0;
        auto s = row::measure(warmup, trials, [&] {
            total = 0;
            for (long long i = 0; i < N; i++) {
                std::string b = pbpool[i % POOL].SerializeAsString();
                total += (long long)b.size();
                g_sink = g_sink + (unsigned char)b[b.size() - 1];
            }
        });
        row::emit("encoder-encode", N, "protobuf", pbv, warmup, trials, s, total == E(std::string("pb_total_") + tag),
                  "WireRecord::SerializeAsString (generated proto3 code); check: total bytes " + std::to_string(total) +
                  " == pb_total" + sz, "", "records/s");
    }
    std::vector<std::string> pbenc(POOL);
    for (int r = 0; r < POOL; r++) pbenc[r] = pbpool[r].SerializeAsString();
    {
        long long sum = 0;
        bool parsed = true;
        auto s = row::measure(warmup, trials, [&] {
            sum = 0;
            for (long long i = 0; i < N; i++) {
                benchwire::WireRecord m;
                parsed &= m.ParseFromString(pbenc[i % POOL]);
                sum += pb_sum(m);
            }
        });
        row::emit("encoder-decode", N, "protobuf", pbv, warmup, trials, s,
                  parsed && sum == E(std::string("decode_sum_") + tag),
                  "WireRecord::ParseFromString into a fresh message (generated code); check: field checksum == decode_sum" + sz,
                  "", "records/s");
    }

    // ---------------- FlatBuffers (object API) ----------------
    std::vector<benchwire_fb::WireRecordT> fbpool(POOL);
    for (int r = 0; r < POOL; r++) fb_fill(fbpool[r], r);
    long long fbsize = 0;
    {
        long long total = 0;
        bool same = true;
        auto s = row::measure(warmup, trials, [&] {
            total = 0;
            for (long long i = 0; i < N; i++) {
                flatbuffers::FlatBufferBuilder fbb(1024);
                fbb.Finish(benchwire_fb::WireRecord::Pack(fbb, &fbpool[i % POOL]));
                flatbuffers::DetachedBuffer b = fbb.Release();
                if (fbsize == 0) fbsize = (long long)b.size();
                same &= (long long)b.size() == fbsize;
                total += (long long)b.size();
                g_sink = g_sink + b.data()[b.size() / 2];
            }
        });
        row::emit("encoder-encode", N, "flatbuffers", fbv, warmup, trials, s, same && total == N * fbsize,
                  "WireRecord::Pack + Finish + Release (object API); check: total bytes == N * " +
                  std::to_string(fbsize) + " (every record the same size)" + sz, "", "records/s");
    }
    std::vector<flatbuffers::DetachedBuffer> fbenc;
    for (int r = 0; r < POOL; r++) {
        flatbuffers::FlatBufferBuilder fbb(1024);
        fbb.Finish(benchwire_fb::WireRecord::Pack(fbb, &fbpool[r]));
        fbenc.push_back(fbb.Release());
    }
    {
        long long sum = 0;
        auto s = row::measure(warmup, trials, [&] {
            sum = 0;
            for (long long i = 0; i < N; i++) {
                std::unique_ptr<benchwire_fb::WireRecordT> t(benchwire_fb::GetWireRecord(fbenc[i % POOL].data())->UnPack());
                sum += fb_sum(*t);
            }
        });
        row::emit("encoder-decode", N, "flatbuffers", fbv, warmup, trials, s, sum == E(std::string("decode_sum_") + tag),
                  "GetWireRecord(buf)->UnPack() into a WireRecordT (object API: a real decode; FlatBuffers' native mode is "
                  "the zero-copy read of struct-view-read); check: field checksum == decode_sum" + sz, "", "records/s");
    }

    // ---------------- Cap'n Proto (generated) ----------------
    long long cpsize = 0;
    {
        long long total = 0;
        bool same = true;
        auto s = row::measure(warmup, trials, [&] {
            total = 0;
            for (long long i = 0; i < N; i++) {
                capnp::MallocMessageBuilder msg;
                cp_fill(msg.initRoot<WireRecord>(), (int)(i % POOL));
                kj::Array<capnp::word> w = capnp::messageToFlatArray(msg);
                long long n = (long long)w.size() * 8;
                if (cpsize == 0) cpsize = n;
                same &= n == cpsize;
                total += n;
                g_sink = g_sink + (int64_t)w.asBytes()[n / 2];
            }
        });
        row::emit("encoder-encode", N, "capnproto", cpv, warmup, trials, s, same && total == N * cpsize,
                  "MallocMessageBuilder + set 100 fields + messageToFlatArray (Cap'n Proto builds in place: the 'encode' is "
                  "the build plus the flat copy); check: total bytes == N * " + std::to_string(cpsize) + sz, "", "records/s");
    }
    std::vector<kj::Array<capnp::word>> cpenc;
    for (int r = 0; r < POOL; r++) {
        capnp::MallocMessageBuilder msg;
        cp_fill(msg.initRoot<WireRecord>(), r);
        cpenc.push_back(capnp::messageToFlatArray(msg));
    }
    {
        long long sum = 0;
        auto s = row::measure(warmup, trials, [&] {
            sum = 0;
            for (long long i = 0; i < N; i++) {
                capnp::FlatArrayMessageReader rd(cpenc[i % POOL].asPtr());
                sum += cp_sum(rd.getRoot<WireRecord>());
            }
        });
        row::emit("encoder-decode", N, "capnproto", cpv, warmup, trials, s, sum == E(std::string("decode_sum_") + tag),
                  "FlatArrayMessageReader + getRoot + read all 100 fields (Cap'n Proto has no decode step: the read is "
                  "zero-copy and validated lazily); check: field checksum == decode_sum" + sz, "", "records/s");
    }

    // ---------------- struct-view-read: FlatBuffers and Cap'n Proto zero-copy ----------------
    const std::string vsz = smoke ? std::string("; smoke size M=1e5 in place of 1e6") : std::string();
    {
        flatbuffers::FlatBufferBuilder fbb(M * 760 + 1024);
        std::vector<flatbuffers::Offset<benchwire_fb::WireRecord>> offs;
        offs.reserve(M);
        for (long long r = 0; r < M; r++) offs.push_back(benchwire_fb::WireRecord::Pack(fbb, &fbpool[r % POOL]));
        auto vec = fbb.CreateVector(offs);
        fbb.Finish(benchwire_fb::CreateWireBatch(fbb, vec));
        std::vector<uint8_t> buf(fbb.GetBufferPointer(), fbb.GetBufferPointer() + fbb.GetSize());
        std::vector<flatbuffers::Offset<benchwire_fb::WireRecord>>().swap(offs);
        long long sum = 0;
        auto s = row::measure(warmup, trials, [&] {
            sum = 0;
            auto recs = flatbuffers::GetRoot<benchwire_fb::WireBatch>(buf.data())->recs();
            for (flatbuffers::uoffset_t i = 0; i < recs->size(); i++) {
                auto t = recs->Get(i);
                sum += t->f1() + t->f2() + (int64_t)(t->f5() * 4.0);
            }
        });
        row::emit("struct-view-read", M, "flatbuffers", fbv, warmup, trials, s, sum == E(std::string("view_sum_") + tag),
                  "GetRoot<WireBatch>(buf)->recs()->Get(i)->f1/f2/f5 over one " + std::to_string(buf.size()) +
                  "-byte buffer (zero-copy; no verifier in the timed loop); check: f1+f2+(int64)(f5*4) == view_sum" + vsz,
                  "", "records/s");
    }
    {
        kj::Array<capnp::word> flat;
        {
            capnp::MallocMessageBuilder msg((unsigned)(M * 92 + 16), capnp::AllocationStrategy::FIXED_SIZE);
            auto recs = msg.initRoot<WireBatch>().initRecs((unsigned)M);
            for (long long r = 0; r < M; r++) cp_fill(recs[(unsigned)r], (int)(r % POOL));
            flat = capnp::messageToFlatArray(msg);
        }
        capnp::ReaderOptions opt;
        opt.traversalLimitInWords = (uint64_t)1 << 40;
        long long sum = 0;
        auto s = row::measure(warmup, trials, [&] {
            sum = 0;
            capnp::FlatArrayMessageReader rd(flat.asPtr(), opt);
            auto recs = rd.getRoot<WireBatch>().getRecs();
            for (auto t : recs) sum += t.getF1() + t.getF2() + (int64_t)(t.getF5() * 4.0);
        });
        row::emit("struct-view-read", M, "capnproto", cpv, warmup, trials, s, sum == E(std::string("view_sum_") + tag),
                  "FlatArrayMessageReader over one " + std::to_string(flat.size() * 8) +
                  "-byte message; getRecs()[i].getF1/F2/F5 (zero-copy); check: f1+f2+(int64)(f5*4) == view_sum" + vsz,
                  "", "records/s");
    }

    // ---------------- schemaencoder: protobuf DynamicMessage ----------------
    {
        google::protobuf::FileDescriptorProto fdp;
        benchwire::WireRecord::descriptor()->file()->CopyTo(&fdp);
        google::protobuf::DescriptorPool pool;
        const google::protobuf::FileDescriptor* fd = pool.BuildFile(fdp);
        const google::protobuf::Descriptor* d = fd ? fd->FindMessageTypeByName("WireRecord") : nullptr;
        google::protobuf::DynamicMessageFactory factory(&pool);
        const google::protobuf::Message* proto = d ? factory.GetPrototype(d) : nullptr;
        if (!proto) {
            row::Stats z;
            row::emit("schemaencoder-encode", N, "protobuf", pbv, 0, 0, z, false, "descriptor build failed", "", "records/s");
        } else {
            std::vector<const google::protobuf::FieldDescriptor*> fs(101);
            for (int k = 1; k <= 100; k++) fs[k] = d->FindFieldByNumber(k);
            std::vector<std::unique_ptr<google::protobuf::Message>> dpool;
            for (int r = 0; r < POOL; r++) {
                std::unique_ptr<google::protobuf::Message> m(proto->New());
                const google::protobuf::Reflection* rf = m->GetReflection();
                for (int k = 1; k <= 100; k++) {
                    if (KINDS[k] == KIND_F64) rf->SetDouble(m.get(), fs[k], vf(r, k));
                    else if (KINDS[k] == KIND_I32) rf->SetInt32(m.get(), fs[k], vi(r, k));
                    else rf->SetInt64(m.get(), fs[k], vl(r, k));
                }
                dpool.push_back(std::move(m));
            }
            long long total = 0;
            auto s = row::measure(warmup, trials, [&] {
                total = 0;
                for (long long i = 0; i < N; i++) {
                    std::string b = dpool[i % POOL]->SerializeAsString();
                    total += (long long)b.size();
                    g_sink = g_sink + (unsigned char)b[b.size() - 1];
                }
            });
            row::emit("schemaencoder-encode", N, "protobuf", pbv, warmup, trials, s, total == E(std::string("pb_total_") + tag),
                      "DynamicMessage built from a runtime DescriptorPool (the schema as a FileDescriptorProto, no generated "
                      "accessors) + SerializeAsString; check: total bytes == pb_total" + sz, "", "records/s");
            std::vector<std::string> denc(POOL);
            for (int r = 0; r < POOL; r++) denc[r] = dpool[r]->SerializeAsString();
            long long sum = 0;
            bool parsed = true;
            auto s2 = row::measure(warmup, trials, [&] {
                sum = 0;
                for (long long i = 0; i < N; i++) {
                    std::unique_ptr<google::protobuf::Message> m(proto->New());
                    parsed &= m->ParseFromString(denc[i % POOL]);
                    const google::protobuf::Reflection* rf = m->GetReflection();
                    for (int k = 1; k <= 100; k++) {
                        if (KINDS[k] == KIND_F64) sum += (int64_t)(rf->GetDouble(*m, fs[k]) * 4.0);
                        else if (KINDS[k] == KIND_I32) sum += rf->GetInt32(*m, fs[k]);
                        else sum += rf->GetInt64(*m, fs[k]);
                    }
                }
            });
            row::emit("schemaencoder-decode", N, "protobuf", pbv, warmup, trials, s2,
                      parsed && sum == E(std::string("decode_sum_") + tag),
                      "DynamicMessage::ParseFromString + Reflection getters by FieldDescriptor; check: field checksum == "
                      "decode_sum" + sz, "", "records/s");
        }
    }

    // ---------------- schemaencoder: Cap'n Proto DynamicStruct ----------------
    {
        capnp::StructSchema schema = capnp::Schema::from<WireRecord>();
        std::vector<capnp::StructSchema::Field> fs;
        for (int k = 1; k <= 100; k++) fs.push_back(schema.getFieldByName(("f" + std::to_string(k)).c_str()));
        long long total = 0;
        bool same = true;
        auto s = row::measure(warmup, trials, [&] {
            total = 0;
            for (long long i = 0; i < N; i++) {
                int r = (int)(i % POOL);
                capnp::MallocMessageBuilder msg;
                capnp::DynamicStruct::Builder b = msg.initRoot<capnp::DynamicStruct>(schema);
                for (int k = 1; k <= 100; k++) {
                    if (KINDS[k] == KIND_F64) b.set(fs[k - 1], vf(r, k));
                    else if (KINDS[k] == KIND_I32) b.set(fs[k - 1], vi(r, k));
                    else b.set(fs[k - 1], vl(r, k));
                }
                kj::Array<capnp::word> w = capnp::messageToFlatArray(msg);
                long long n = (long long)w.size() * 8;
                same &= n == cpsize;
                total += n;
                g_sink = g_sink + (int64_t)w.asBytes()[n / 2];
            }
        });
        row::emit("schemaencoder-encode", N, "capnproto", cpv, warmup, trials, s, same && total == N * cpsize,
                  "DynamicStruct::Builder over capnp::StructSchema, fields set by StructSchema::Field + messageToFlatArray; "
                  "check: total bytes == N * " + std::to_string(cpsize) + sz, "", "records/s");
        long long sum = 0;
        auto s2 = row::measure(warmup, trials, [&] {
            sum = 0;
            for (long long i = 0; i < N; i++) {
                capnp::FlatArrayMessageReader rd(cpenc[i % POOL].asPtr());
                capnp::DynamicStruct::Reader v = rd.getRoot<capnp::DynamicStruct>(schema);
                for (int k = 1; k <= 100; k++) {
                    capnp::DynamicValue::Reader x = v.get(fs[k - 1]);
                    if (KINDS[k] == KIND_F64) sum += (int64_t)(x.as<double>() * 4.0);
                    else sum += x.as<int64_t>();
                }
            }
        });
        row::emit("schemaencoder-decode", N, "capnproto", cpv, warmup, trials, s2, sum == E(std::string("decode_sum_") + tag),
                  "DynamicStruct::Reader get(field) for all 100 fields; check: field checksum == decode_sum" + sz, "",
                  "records/s");
    }

    // ---------------- compressstream-roundtrip: zlib and zstd ----------------
    {
        const size_t SZ = smoke ? ((size_t)8 << 20) : ((size_t)64 << 20);
        const size_t PIECE = 65536, FLUSH = 1 << 20;
        std::vector<unsigned char> data(SZ + 720);
        for (size_t off = 0, r = 0; off < SZ; off += 720, r++) fixed_record((int)(r % POOL), data.data() + off);
        data.resize(SZ);
        bool crc_ok = (long long)crc32(0, data.data(), (uInt)SZ) == E(std::string("stream_crc32_") + tag);
        const std::string ssz = smoke ? std::string("; smoke size 8 MiB in place of 64 MiB") : std::string();
        std::vector<unsigned char> back;
        long long clen = 0;
        bool err = false;
        // zlib: raw DEFLATE (windowBits -15) level 1
        auto sz_ = row::measure(warmup, trials, [&] {
            std::vector<unsigned char> comp;
            comp.reserve(SZ / 2);
            z_stream z{};
            deflateInit2(&z, 1, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY);
            unsigned char obuf[65536];
            auto pump = [&](const unsigned char* p, size_t n, int flush) {
                z.next_in = const_cast<unsigned char*>(p);
                z.avail_in = (uInt)n;
                do {
                    z.next_out = obuf;
                    z.avail_out = sizeof obuf;
                    int rc = deflate(&z, flush);
                    if (rc == Z_STREAM_ERROR) err = true;
                    comp.insert(comp.end(), obuf, obuf + (sizeof obuf - z.avail_out));
                } while (z.avail_out == 0);
            };
            for (size_t off = 0; off < SZ; off += PIECE) {
                size_t n = std::min(PIECE, SZ - off);
                pump(data.data() + off, n, ((off + n) % FLUSH == 0 && off + n < SZ) ? Z_SYNC_FLUSH : Z_NO_FLUSH);
            }
            pump(nullptr, 0, Z_FINISH);
            deflateEnd(&z);
            clen = (long long)comp.size();
            back.clear();
            back.reserve(SZ);
            z_stream u{};
            inflateInit2(&u, -15);
            for (size_t off = 0; off < comp.size(); off += PIECE) {
                u.next_in = comp.data() + off;
                u.avail_in = (uInt)std::min(PIECE, comp.size() - off);
                do {
                    u.next_out = obuf;
                    u.avail_out = sizeof obuf;
                    int rc = inflate(&u, Z_NO_FLUSH);
                    if (rc != Z_OK && rc != Z_STREAM_END && rc != Z_BUF_ERROR) err = true;
                    back.insert(back.end(), obuf, obuf + (sizeof obuf - u.avail_out));
                } while (u.avail_out == 0);
            }
            inflateEnd(&u);
        });
        bool eq = !err && crc_ok && back.size() == SZ && std::memcmp(back.data(), data.data(), SZ) == 0;
        row::emit("compressstream-roundtrip", (long long)SZ, "zlib", std::string(zlibVersion()) + " (system)", warmup, trials,
                  sz_, eq, "deflateInit2 raw (windowBits -15) level 1; 64 KiB writes; Z_SYNC_FLUSH every 1 MiB; Z_FINISH; "
                  "inflate in 64 KiB pieces; compressed " + std::to_string(clen) + " B; check: byte equality" + ssz,
                  "zlib", "MB/s");
        // zstd level 1, ZSTD_e_flush every 1 MiB
        err = false;
        auto sz2 = row::measure(warmup, trials, [&] {
            std::vector<unsigned char> comp;
            comp.reserve(SZ / 2);
            ZSTD_CCtx* c = ZSTD_createCCtx();
            ZSTD_CCtx_setParameter(c, ZSTD_c_compressionLevel, 1);
            std::vector<unsigned char> obuf(ZSTD_CStreamOutSize());
            auto pump = [&](const unsigned char* p, size_t n, ZSTD_EndDirective mode) {
                ZSTD_inBuffer in{p, n, 0};
                for (;;) {
                    ZSTD_outBuffer out{obuf.data(), obuf.size(), 0};
                    size_t rem = ZSTD_compressStream2(c, &out, &in, mode);
                    if (ZSTD_isError(rem)) { err = true; return; }
                    comp.insert(comp.end(), obuf.data(), obuf.data() + out.pos);
                    if (mode == ZSTD_e_continue ? in.pos == in.size : rem == 0) return;
                }
            };
            for (size_t off = 0; off < SZ; off += PIECE) {
                size_t n = std::min(PIECE, SZ - off);
                pump(data.data() + off, n, ((off + n) % FLUSH == 0 && off + n < SZ) ? ZSTD_e_flush : ZSTD_e_continue);
            }
            pump(nullptr, 0, ZSTD_e_end);
            ZSTD_freeCCtx(c);
            clen = (long long)comp.size();
            back.clear();
            back.reserve(SZ);
            ZSTD_DCtx* dc = ZSTD_createDCtx();
            std::vector<unsigned char> dbuf(ZSTD_DStreamOutSize());
            for (size_t off = 0; off < comp.size(); off += PIECE) {
                ZSTD_inBuffer in{comp.data() + off, std::min(PIECE, comp.size() - off), 0};
                while (in.pos < in.size) {
                    ZSTD_outBuffer out{dbuf.data(), dbuf.size(), 0};
                    size_t rc = ZSTD_decompressStream(dc, &out, &in);
                    if (ZSTD_isError(rc)) { err = true; break; }
                    back.insert(back.end(), dbuf.data(), dbuf.data() + out.pos);
                }
            }
            for (;;) {   // drain what the decoder still holds
                ZSTD_inBuffer in{nullptr, 0, 0};
                ZSTD_outBuffer out{dbuf.data(), dbuf.size(), 0};
                size_t rc = ZSTD_decompressStream(dc, &out, &in);
                if (ZSTD_isError(rc)) { err = true; break; }
                back.insert(back.end(), dbuf.data(), dbuf.data() + out.pos);
                if (out.pos == 0) break;
            }
            ZSTD_freeDCtx(dc);
        });
        eq = !err && crc_ok && back.size() == SZ && std::memcmp(back.data(), data.data(), SZ) == 0;
        row::emit("compressstream-roundtrip", (long long)SZ, "zstd", std::string(ZSTD_versionString()) + " (system)", warmup,
                  trials, sz2, eq, "ZSTD_compressStream2 level 1; 64 KiB writes; ZSTD_e_flush every 1 MiB; ZSTD_e_end; "
                  "ZSTD_decompressStream in 64 KiB pieces; compressed " + std::to_string(clen) + " B; check: byte equality" + ssz,
                  "zstd", "MB/s");
    }
    return 0;
}

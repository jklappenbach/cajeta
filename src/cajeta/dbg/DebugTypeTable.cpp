#include "cajeta/dbg/DebugTypeTable.h"

#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DerivedTypes.h>

#include <algorithm>
#include <cstdio>
#include <deque>
#include <fstream>
#include <set>

#include "cajeta/dbg/DebugVars.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/type/CajetaType.h"

namespace cajeta::dbg {

namespace {
    const char* kStringFqn = "cajeta.lang.String";
    const char* kArrayListFqn = "cajeta.collection.ArrayList";
    const char* kHashMapFqn = "cajeta.collection.HashMap";

    // The base type name with any generic arguments stripped:
    // "cajeta.collection.ArrayList<int32>" -> "cajeta.collection.ArrayList".
    std::string typeBase(const std::string& t) {
        auto lt = t.find('<');
        return lt == std::string::npos ? t : t.substr(0, lt);
    }

    CollectionKind collectionKindOf(const std::string& type) {
        const std::string base = typeBase(type);
        if (base == kArrayListFqn) return CollectionKind::ArrayList;
        if (base == kHashMapFqn) return CollectionKind::HashMap;
        return CollectionKind::None;
    }

    // Inline-vs-pointer storage of a field by its declared type, mirroring how
    // the layout stores it: a value type and a primitive are inline bytes; a
    // String, reference class or array holds the instance/header pointer at the
    // slot base.
    Storage storageOfType(const cajeta::CajetaTypePtr& t) {
        if (auto klass = std::dynamic_pointer_cast<cajeta::CajetaClass>(t))
            return klass->isValueType() ? Storage::Inline : Storage::Pointer;
        return Storage::Inline;  // primitive
    }

    // A class's STATIC fields, inherited-then-own, deduped. The symbol is the
    // declaring class's canonical + "." + name — the exact global
    // getOrCreateStaticFieldGlobal emits — so the DECLARING class names the
    // symbol even on a subclass's inherited view.
    void collectStatics(cajeta::CajetaClass* cls,
                        std::vector<StaticFieldRecord>& out,
                        std::set<void*>& seen) {
        if (!cls) return;
        for (auto& parent : cls->getSuperClasses())
            collectStatics(parent.get(), out, seen);
        for (auto& prop : cls->getPropertyList()) {
            if (!prop->isStatic()) continue;
            if (!seen.insert(prop.get()).second) continue;
            StaticFieldRecord sr;
            sr.name = prop->getName();
            sr.type = prop->getType() ? prop->getType()->toCanonical() : "";
            sr.symbol = cls->getQName()
                ? cls->getQName()->toCanonical() + "." + prop->getName() : "";
            if (!sr.symbol.empty()) out.push_back(std::move(sr));
        }
    }

    // A class's non-static fields in layout order — inherited (ancestors,
    // recursively) before own — deduped so a shared vbase field appears once.
    void collectFields(cajeta::CajetaClass* cls,
                       std::vector<cajeta::StructurePropertyPtr>& out,
                       std::set<void*>& seen) {
        if (!cls) return;
        for (auto& parent : cls->getSuperClasses())
            collectFields(parent.get(), out, seen);
        for (auto& prop : cls->getPropertyList()) {
            if (prop->isStatic()) continue;
            if (seen.insert(prop.get()).second) out.push_back(prop);
        }
    }

    // Geometry of one array element, mirroring CajetaArray::getElementLlvmType
    // so the stride stored here is the stride the JIT'd code stores at. Array
    // types are not interned in the name map, so the element name is stripped
    // and resolved; no CajetaArray is constructed (that would mutate the live
    // type registry).
    bool resolveElem(const std::string& arrayType, const llvm::DataLayout& dl,
                     ElemRecord& out) {
        if (arrayType.empty() || arrayType.back() != ']') return false;
        auto lb = arrayType.find_last_of('[');
        if (lb == std::string::npos) return false;
        std::string elemName = arrayType.substr(0, lb);
        if (elemName.empty()) return false;

        // A nested array element (`int32[][]` -> `int32[]`) is itself a heap
        // reference: a pointer slot.
        if (elemName.back() == ']') {
            out.type = elemName;
            out.storage = Storage::Pointer;
            out.stride = dl.getPointerSize();
            return true;
        }

        auto elem = cajeta::CajetaType::find(elemName);
        if (!elem) {
            // Unresolved user type: assume a pointer slot so a decoder only ever
            // reads a pointer, never a fabricated inline width.
            out.type = elemName;
            out.storage = Storage::Pointer;
            out.stride = dl.getPointerSize();
            return true;
        }
        out.type = elem->toCanonical();

        if (auto klass = std::dynamic_pointer_cast<cajeta::CajetaClass>(elem)) {
            CajetaTypeFlags flags = klass->getTypeFlags();
            bool pointerSlot =
                klass->isWildcardInstantiation() ||
                (!klass->isInterface() && (flags & STRUCT_FLAG) == 0 &&
                 (flags & PRIMITIVE_FLAG) == 0);
            if (pointerSlot) {
                out.storage = Storage::Pointer;
                out.stride = dl.getPointerSize();
            } else {
                // STRUCT_FLAG classes keep their own struct slot. A value type
                // is stored inline; String (a reference class with a struct
                // slot) keeps the String* at the slot base — a pointer read.
                llvm::Type* elemLlvm = klass->getLlvmType();
                if (!elemLlvm) return false;
                out.storage = klass->isValueType() ? Storage::Inline
                                                   : Storage::Pointer;
                out.stride = dl.getTypeAllocSize(elemLlvm);
            }
        } else {
            llvm::Type* elemLlvm = elem->getLlvmType();
            if (!elemLlvm) return false;
            out.storage = Storage::Inline;
            out.stride = dl.getTypeAllocSize(elemLlvm);
        }
        return out.stride > 0;
    }
} // namespace

void DebugTypeTable::addRoot(const std::string& canonical) {
    if (canonical.empty()) return;
    if (std::find(roots_.begin(), roots_.end(), canonical) == roots_.end())
        roots_.push_back(canonical);
}

void DebugTypeTable::put(TypeRecord rec) {
    if (rec.canonical.empty()) return;
    std::string key = rec.canonical;
    records_[key] = std::move(rec);
}

void DebugTypeTable::putAs(const std::string& key, TypeRecord rec) {
    if (key.empty() || rec.canonical.empty()) return;
    records_[key] = std::move(rec);
}

void DebugTypeTable::putVtable(const std::string& symbol, VtableEntry entry) {
    if (symbol.empty() || entry.canonical.empty()) return;
    vtables_[symbol] = std::move(entry);
}

const TypeRecord* DebugTypeTable::find(const std::string& canonical) const {
    auto it = records_.find(canonical);
    return it == records_.end() ? nullptr : &it->second;
}

void DebugTypeTable::clear() {
    records_.clear();
    roots_.clear();
    bounded_.clear();
    vtables_.clear();
    stringAbi_ = StringAbi{};
}

std::vector<std::string> DebugTypeTable::names() const {
    std::vector<std::string> out;
    out.reserve(records_.size());
    for (const auto& [name, _] : records_) out.push_back(name);
    std::sort(out.begin(), out.end());
    return out;
}

namespace {
    // The String decode ABI from the live layout, derived exactly as
    // deriveEntryArgsABI derives it for makeEntryArgs — nothing hardcoded.
    StringAbi deriveStringAbi(const llvm::DataLayout& dl) {
        StringAbi abi;
        auto klass = std::dynamic_pointer_cast<cajeta::CajetaClass>(
            cajeta::CajetaType::find(kStringFqn));
        if (!klass) return abi;
        auto* st = llvm::dyn_cast_or_null<llvm::StructType>(klass->getLlvmType());
        if (!st || st->isOpaque() || st->getNumElements() < 4) return abi;
        const llvm::StructLayout* sl = dl.getStructLayout(st);
        abi.size = dl.getTypeAllocSize(st);
        abi.offLenTag = sl->getElementOffset(1);
        abi.offAux = sl->getElementOffset(2);
        abi.offBase = sl->getElementOffset(3);
        abi.valid = true;
        return abi;
    }
} // namespace

void DebugTypeTable::buildFromTypeWorld(const llvm::DataLayout& dl,
                                        const BuildOptions& opts) {
    if (!stringAbi_.valid) stringAbi_ = deriveStringAbi(dl);

    struct Pending { std::string name; size_t depth; };
    std::deque<Pending> work;
    std::set<std::string> queued;
    for (const auto& r : roots_) {
        if (queued.insert(r).second) work.push_back({r, 0});
    }
    // Whole-world closure (runtime-type-inspection §3.1.1): a base- or
    // Object-typed row can hold ANY subtype at runtime, so every compiled
    // class is carried, not just the declared-local closure. Enqueued after
    // the roots so root-reachable records keep discovery priority under the
    // bound; the existing bound + logged-drop machinery still applies.
    for (const auto& [name, t] : cajeta::CajetaType::getCanonicalMap()) {
        if (!t) continue;
        if (!std::dynamic_pointer_cast<cajeta::CajetaClass>(t)) continue;
        if (queued.insert(name).second) work.push_back({name, 0});
    }

    // A record is filed under the name it was ASKED for as well as its canonical
    // name: a debug local's declared type string is the warm lookup key, and it
    // need not already be canonical. A plain class is filed under its SHORT name
    // too — `CajetaType::of` resolved short names, and the bridge must keep
    // resolving everything it resolved before (§4.1.3). First writer wins, so a
    // short name shared across packages never overwrites another's layout.
    auto file = [&](const std::string& asked, TypeRecord rec) {
        const std::string canonical = rec.canonical;
        auto shortName = [&]() -> std::string {
            if (canonical.find('<') != std::string::npos) return "";
            if (!canonical.empty() && canonical.back() == ']') return "";
            auto dot = canonical.find_last_of('.');
            return dot == std::string::npos ? "" : canonical.substr(dot + 1);
        }();
        if (!shortName.empty() && !records_.count(shortName))
            records_[shortName] = rec;
        if (asked != canonical) records_[asked] = rec;
        records_[canonical] = std::move(rec);
    };
    auto note = [&](const std::string& name, size_t depth) {
        if (name.empty()) return;
        if (queued.insert(name).second) work.push_back({name, depth});
    };
    auto bound = [&](const std::string& name) {
        if (std::find(bounded_.begin(), bounded_.end(), name) == bounded_.end())
            bounded_.push_back(name);
    };

    while (!work.empty()) {
        const Pending item = work.front();
        work.pop_front();
        const std::string& name = item.name;
        if (records_.count(name)) continue;   // already carried

        // A bound never truncates silently (spec §5.1.3): the dropped type is
        // recorded and reported, so a gap in inspection has an explanation.
        if (item.depth > opts.maxDepth || records_.size() >= opts.maxRecords) {
            bound(name);
            continue;
        }

        // Primitives are leaves rendered by width.
        if (isPrimitiveTypeName(name)) {
            TypeRecord rec;
            rec.canonical = name;
            rec.kind = TypeKind::Leaf;
            rec.isValueType = true;   // the slot holds the bytes
            file(name, std::move(rec));
            continue;
        }

        // The `[]` suffix is authoritative: an array is an array whether or not
        // its name interns in the registry.
        if (name.back() == ']') {
            ElemRecord elem;
            if (!resolveElem(name, dl, elem)) continue;
            TypeRecord rec;
            rec.canonical = elem.type + "[]";
            rec.kind = TypeKind::Array;
            rec.elem = elem;
            file(name, std::move(rec));
            note(elem.type, item.depth + 1);
            continue;
        }

        auto ct = cajeta::CajetaType::find(name);
        if (!ct) continue;   // unresolved: absent, never faked
        const std::string canonical = ct->toCanonical();

        // String is a leaf decoded by its own ABI, not by field walking.
        if (canonical == kStringFqn) {
            TypeRecord rec;
            rec.canonical = canonical;
            rec.kind = TypeKind::Leaf;
            rec.isValueType = false;
            rec.isString = true;
            file(name, std::move(rec));
            continue;
        }

        auto klass = std::dynamic_pointer_cast<cajeta::CajetaClass>(ct);
        if (!klass) {
            // A non-class, non-primitive type has no walkable layout: an empty
            // Object record, which renders as `{…}` — what the live decode
            // already does for one.
            TypeRecord rec;
            rec.canonical = canonical;
            rec.kind = TypeKind::Object;
            file(name, std::move(rec));
            continue;
        }

        TypeRecord rec;
        rec.canonical = canonical;
        rec.isValueType = klass->isValueType();
        rec.collectionKind = collectionKindOf(canonical);
        rec.kind = rec.collectionKind == CollectionKind::None
            ? TypeKind::Object : TypeKind::Collection;

        // Static fields (runtime-type-inspection §4.1.1): inherited-then-own,
        // symbol-addressed, resolved per session.
        {
            std::set<void*> seenStatics;
            collectStatics(klass.get(), rec.statics, seenStatics);
            for (const auto& sr : rec.statics) note(sr.type, item.depth + 1);
        }

        // Vtable map (runtime-type-inspection §2.1.1): symbols from the REAL
        // globals. Primary at offset 0; each secondary ($as$) vtable carries
        // its sub-object offset so a base-view pointer can be rebased.
        if (auto* vg = klass->getVirtualTableGlobal()) {
            putVtable(vg->getName().str(), {canonical, 0});
        }
        for (auto& [parentCanon, sg] : klass->secondaryVTablesRef()) {
            if (!sg) continue;
            uint64_t off = 0;
            if (auto parent = std::dynamic_pointer_cast<cajeta::CajetaClass>(
                    cajeta::CajetaType::find(parentCanon)))
                off = klass->getSubObjectByteOffset(parent.get());
            putVtable(sg->getName().str(), {canonical, off});
        }

        auto* st = llvm::dyn_cast_or_null<llvm::StructType>(klass->getLlvmType());
        if (st && !st->isOpaque()) {
            const llvm::StructLayout* sl = dl.getStructLayout(st);
            std::vector<cajeta::StructurePropertyPtr> fields;
            std::set<void*> seen;
            collectFields(klass.get(), fields, seen);
            for (const auto& f : fields) {
                int slot = klass->getFieldLlvmIndex(f);
                if (slot < 0 || slot >= static_cast<int>(st->getNumElements()))
                    continue;
                FieldRecord fr;
                fr.name = f->getName();
                fr.type = f->getType() ? f->getType()->toCanonical() : "";
                fr.storage = storageOfType(f->getType());
                // The offset comes from the DataLayout end to end — never
                // index*8 — so a field after an interior secondary-vtable word
                // lands on its own bytes.
                fr.offset = sl->getElementOffset(slot);
                note(fr.type, item.depth + 1);
                rec.fields.push_back(std::move(fr));
            }
        }
        file(name, std::move(rec));
    }

    if (!bounded_.empty()) {
        std::fprintf(stderr,
                     "[cajeta] debug type table: %zu reachable type(s) dropped "
                     "by the closure bound (first: %s); those values will not "
                     "expand at a stop\n",
                     bounded_.size(), bounded_.front().c_str());
    }
}

DebugTypeTable& globalDebugTypeTable() {
    static DebugTypeTable table;
    return table;
}

// ---- sidecar (spec §3.1) ------------------------------------------------
//
// Line-oriented, versioned, tab-separated — the dbgloc sidecar's style:
//
//   cajeta-typeinfo-v1
//   abi\t<valid>\t<size>\t<offLenTag>\t<offAux>\t<offBase>
//   rec\t<key>\t<canonical>\t<kind>\t<isValueType>\t<isString>\t<collKind>
//      \t<elemType>\t<elemStride>\t<elemStorage>\t<nFields>
//      [\t<fName>\t<fType>\t<fOffset>\t<fStorage>]*
//
// One `rec` line per MAP ENTRY (alias keys included), so a load reproduces the
// exact lookup surface the cold build had. Strings are escaped for tab/newline/
// backslash. The major rides the header: a reader that does not know it
// refuses the whole table rather than misreading (§3.1.2).

namespace {
    // v2 (runtime-type-inspection 1.2.3): rec lines gained a statics tail and
    // the vtab line kind. The v1 reader refuses unknown line kinds by design,
    // so the header major gates the whole format; a v1 slot misses under -g
    // and heals by recompiling once.
    const char* kSidecarMagic = "cajeta-typeinfo-v2";

    std::string escapeField(const std::string& s) {
        std::string out;
        out.reserve(s.size());
        for (char c : s) {
            switch (c) {
                case '\\': out += "\\\\"; break;
                case '\t': out += "\\t"; break;
                case '\n': out += "\\n"; break;
                default: out += c;
            }
        }
        return out;
    }

    bool unescapeField(const std::string& s, std::string& out) {
        out.clear();
        out.reserve(s.size());
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] != '\\') { out += s[i]; continue; }
            if (++i >= s.size()) return false;  // dangling escape
            switch (s[i]) {
                case '\\': out += '\\'; break;
                case 't': out += '\t'; break;
                case 'n': out += '\n'; break;
                default: return false;
            }
        }
        return true;
    }

    // Parse a non-negative integer field strictly (whole field, no sign).
    bool parseU64(const std::string& s, uint64_t& out) {
        if (s.empty()) return false;
        out = 0;
        for (char c : s) {
            if (c < '0' || c > '9') return false;
            out = out * 10 + static_cast<uint64_t>(c - '0');
        }
        return true;
    }

    bool parseEnum(const std::string& s, uint64_t maxInclusive, uint64_t& out) {
        return parseU64(s, out) && out <= maxInclusive;
    }
} // namespace

bool writeTypeSidecar(const std::string& path, const DebugTypeTable& table) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << kSidecarMagic << '\n';

    const StringAbi& abi = table.stringAbi();
    out << "abi\t" << (abi.valid ? 1 : 0) << '\t' << abi.size << '\t'
        << abi.offLenTag << '\t' << abi.offAux << '\t' << abi.offBase << '\n';

    // names() is sorted, so the byte stream is deterministic for a given table.
    for (const auto& name : table.names()) {
        const TypeRecord* rec = table.find(name);
        if (!rec) continue;
        out << "rec\t" << escapeField(name)
            << '\t' << escapeField(rec->canonical)
            << '\t' << static_cast<int>(rec->kind)
            << '\t' << (rec->isValueType ? 1 : 0)
            << '\t' << (rec->isString ? 1 : 0)
            << '\t' << static_cast<int>(rec->collectionKind)
            << '\t' << escapeField(rec->elem.type)
            << '\t' << rec->elem.stride
            << '\t' << static_cast<int>(rec->elem.storage)
            << '\t' << rec->fields.size();
        for (const auto& f : rec->fields) {
            out << '\t' << escapeField(f.name)
                << '\t' << escapeField(f.type)
                << '\t' << f.offset
                << '\t' << static_cast<int>(f.storage);
        }
        // v2: the statics tail — count + {name, type, symbol} triples.
        out << '\t' << rec->statics.size();
        for (const auto& sf : rec->statics) {
            out << '\t' << escapeField(sf.name)
                << '\t' << escapeField(sf.type)
                << '\t' << escapeField(sf.symbol);
        }
        out << '\n';
    }
    // v2: the vtable map — one line per vtable global.
    for (const auto& [sym, e] : table.vtables()) {
        out << "vtab\t" << escapeField(sym)
            << '\t' << escapeField(e.canonical)
            << '\t' << e.subObjectByteOffset << '\n';
    }
    return out.good();
}

bool loadTypeSidecar(const std::string& path, DebugTypeTable& into) {
    // All-or-nothing: parse into a scratch table and only then swap it in, so
    // a failure at ANY line leaves `into` empty — never a partial misread.
    into.clear();

    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::string line;
    if (!std::getline(in, line) || line != kSidecarMagic) return false;

    DebugTypeTable scratch;
    bool sawAbi = false;

    while (std::getline(in, line)) {
        if (line.empty()) continue;
        std::vector<std::string> f;
        size_t start = 0;
        while (true) {
            size_t tab = line.find('\t', start);
            f.push_back(line.substr(start, tab - start));
            if (tab == std::string::npos) break;
            start = tab + 1;
        }

        if (f[0] == "abi") {
            if (f.size() != 6) return false;
            uint64_t valid;
            StringAbi abi;
            if (!parseEnum(f[1], 1, valid) || !parseU64(f[2], abi.size) ||
                !parseU64(f[3], abi.offLenTag) || !parseU64(f[4], abi.offAux) ||
                !parseU64(f[5], abi.offBase))
                return false;
            abi.valid = valid == 1;
            scratch.setStringAbi(abi);
            sawAbi = true;
            continue;
        }

        if (f[0] == "rec") {
            if (f.size() < 11) return false;
            std::string key;
            TypeRecord rec;
            uint64_t kind, isVal, isStr, collKind, elemStorage, nFields;
            if (!unescapeField(f[1], key) ||
                !unescapeField(f[2], rec.canonical) ||
                !parseEnum(f[3], static_cast<uint64_t>(TypeKind::Collection), kind) ||
                !parseEnum(f[4], 1, isVal) ||
                !parseEnum(f[5], 1, isStr) ||
                !parseEnum(f[6], static_cast<uint64_t>(CollectionKind::HashMap), collKind) ||
                !unescapeField(f[7], rec.elem.type) ||
                !parseU64(f[8], rec.elem.stride) ||
                !parseEnum(f[9], 1, elemStorage) ||
                !parseU64(f[10], nFields))
                return false;
            rec.kind = static_cast<TypeKind>(kind);
            rec.isValueType = isVal == 1;
            rec.isString = isStr == 1;
            rec.collectionKind = static_cast<CollectionKind>(collKind);
            rec.elem.storage = static_cast<Storage>(elemStorage);

            size_t base = 11;
            if (f.size() < base + nFields * 4 + 1) return false;  // truncated
            for (uint64_t i = 0; i < nFields; i++) {
                size_t at = base + i * 4;
                FieldRecord fr;
                uint64_t storage;
                if (!unescapeField(f[at], fr.name) ||
                    !unescapeField(f[at + 1], fr.type) ||
                    !parseU64(f[at + 2], fr.offset) ||
                    !parseEnum(f[at + 3], 1, storage))
                    return false;
                fr.storage = static_cast<Storage>(storage);
                rec.fields.push_back(std::move(fr));
            }
            base += nFields * 4;
            // v2: the statics tail — count + {name, type, symbol} triples.
            uint64_t nStatics;
            if (!parseU64(f[base], nStatics)) return false;
            base += 1;
            if (f.size() != base + nStatics * 3) return false;  // truncated
            for (uint64_t i = 0; i < nStatics; i++) {
                size_t at = base + i * 3;
                StaticFieldRecord sr;
                if (!unescapeField(f[at], sr.name) ||
                    !unescapeField(f[at + 1], sr.type) ||
                    !unescapeField(f[at + 2], sr.symbol))
                    return false;
                rec.statics.push_back(std::move(sr));
            }
            if (key.empty() || rec.canonical.empty()) return false;
            scratch.putAs(key, std::move(rec));
            continue;
        }

        if (f[0] == "vtab") {
            if (f.size() != 4) return false;
            std::string sym;
            VtableEntry e;
            if (!unescapeField(f[1], sym) ||
                !unescapeField(f[2], e.canonical) ||
                !parseU64(f[3], e.subObjectByteOffset))
                return false;
            scratch.putVtable(sym, std::move(e));
            continue;
        }

        return false;  // unknown line kind: refuse, don't guess
    }

    if (!sawAbi) return false;
    into = std::move(scratch);
    return true;
}

} // namespace cajeta::dbg

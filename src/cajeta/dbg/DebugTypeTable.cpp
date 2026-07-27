#include "cajeta/dbg/DebugTypeTable.h"

#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DerivedTypes.h>

#include <algorithm>
#include <cstdio>
#include <deque>
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

        auto elem = cajeta::CajetaType::of(elemName);
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

const TypeRecord* DebugTypeTable::find(const std::string& canonical) const {
    auto it = records_.find(canonical);
    return it == records_.end() ? nullptr : &it->second;
}

void DebugTypeTable::clear() {
    records_.clear();
    roots_.clear();
    bounded_.clear();
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
            cajeta::CajetaType::of(kStringFqn));
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

        auto ct = cajeta::CajetaType::of(name);
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

} // namespace cajeta::dbg

//
// element-ownership Unit 2 — type-system mode + monomorphization (spec §2,
// §8.3; plan 2.1). AST-level: instantiate a template with owning (`#`) vs
// borrow type arguments and assert they are DISTINCT instantiations whose
// per-argument ownership mode is queryable.
//
// These drive CajetaClass::instantiate(args, argOwning) directly — the mode is
// folded into the cache key so owning/borrow are distinct monomorphizations,
// and stored on the result for isTypeArgumentOwning(i).
//

#include "gtest/gtest.h"
#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/error/Exception.h"

#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

using cajeta::Compiler;
using cajeta::CajetaModulePtr;
using cajeta::CajetaClassPtr;
using cajeta::CajetaTypePtr;

namespace {

CajetaModulePtr compileForInspection(Compiler& compiler,
                                     const std::string& source,
                                     const std::string& fqClassName) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = std::filesystem::temp_directory_path()
              / ("cajeta_eom_" + std::to_string(rng()));
    std::filesystem::create_directories(base);
    std::filesystem::path rel;
    size_t start = 0;
    for (size_t i = 0; i <= fqClassName.size(); ++i) {
        if (i == fqClassName.size() || fqClassName[i] == '.') {
            rel /= fqClassName.substr(start, i - start);
            start = i + 1;
        }
    }
    rel += ".cajeta";
    auto full = base / rel;
    std::filesystem::create_directories(full.parent_path());
    std::ofstream out(full);
    out << source;
    out.close();
    auto archive = std::filesystem::temp_directory_path()
                 / ("cajeta_eom_arch_" + std::to_string(rng()));
    std::filesystem::create_directories(archive);
    auto m = compiler.createModule(full.string(), base.string(), archive.string());
    compiler.compile(m);
    return m;
}

const char* kSrc =
    "package test;\n"
    "public class Elem { }\n"
    "public class Box<T> { public T value; }\n"
    "public class Ut { public static int32 run() { return 0; } }\n";

} // namespace

// 2.1.1 — owning (`#T`) and borrow (`T`) instantiations are DISTINCT objects.
TEST(ElementOwnershipModeTests, owningAndBorrowAreDistinctInstantiations) {
    Compiler compiler;
    auto module = compileForInspection(compiler, kSrc, "test.Ut");
    auto box = module->getStructures()["test.Box"];
    ASSERT_NE(box, nullptr);
    ASSERT_TRUE(box->isTemplate());
    CajetaTypePtr elem = module->getStructures()["test.Elem"];
    ASSERT_NE(elem, nullptr);

    auto owning = box->instantiate({elem}, {true});
    auto borrow = box->instantiate({elem}, {false});
    ASSERT_NE(owning, nullptr);
    ASSERT_NE(borrow, nullptr);
    EXPECT_NE(owning.get(), borrow.get());  // distinct monomorphizations
}

// 2.1.2 — the per-argument ownership mode is queryable from the instantiation.
TEST(ElementOwnershipModeTests, ownershipModeIsQueryable) {
    Compiler compiler;
    auto module = compileForInspection(compiler, kSrc, "test.Ut");
    auto box = module->getStructures()["test.Box"];
    ASSERT_NE(box, nullptr);
    CajetaTypePtr elem = module->getStructures()["test.Elem"];
    ASSERT_NE(elem, nullptr);

    auto owning = box->instantiate({elem}, {true});
    auto borrow = box->instantiate({elem}, {false});
    ASSERT_NE(owning, nullptr);
    ASSERT_NE(borrow, nullptr);
    EXPECT_TRUE(owning->isTypeArgumentOwning(0));
    EXPECT_FALSE(borrow->isTypeArgumentOwning(0));
    // A plain (borrow) instantiation via the default overload stays borrow.
    auto plain = box->instantiate({elem});
    ASSERT_NE(plain, nullptr);
    EXPECT_FALSE(plain->isTypeArgumentOwning(0));
    EXPECT_EQ(plain.get(), borrow.get());  // borrow == default: same monomorph
}

// 2.1.3 — a SOURCE-LEVEL `#` type argument resolves to an owning instantiation;
// a plain one to borrow. Exercises the parse→instantiate ownership threading
// (a `Box<#Elem>` field and a `Box<Elem>` field produce two Box instantiations,
// one owning, one borrow).
TEST(ElementOwnershipModeTests, sourceLevelHashProducesOwningInstantiation) {
    auto src =
        "package test;\n"
        "public class Elem { }\n"
        "public class Box<T> { public T value; }\n"
        "public class Ut {\n"
        "  public Box<#Elem> owned;\n"
        "  public Box<Elem> borrowed;\n"
        "  public static int32 run() { return 0; }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.Ut");
    bool sawOwning = false, sawBorrow = false;
    for (auto& [name, klass] : module->getStructures()) {
        if (name.rfind("test.Box<", 0) != 0 || !klass) continue;
        ASSERT_EQ(klass->getTypeArguments().size(), 1u);
        if (klass->isTypeArgumentOwning(0)) sawOwning = true;
        else sawBorrow = true;
    }
    EXPECT_TRUE(sawOwning);   // Box<#Elem> resolved owning
    EXPECT_TRUE(sawBorrow);   // Box<Elem> resolved borrow
}

// ---------------------------------------------------------------------------
// 2.1.4 — the §8.1 value-semantics gate. `#` demands a separable ownership
// story from the argument: a primitive carries none (§8.1.1); a value type
// owns heap payload only when shared-capable (§8.1.2 — Utf8/Slice or a
// transitive embedder). Borrow instantiations of the same arguments stay fine.
// ---------------------------------------------------------------------------

// §8.1.1 — `#` on a primitive type argument is a compile error; the plain
// (borrow) instantiation of the same argument is untouched.
TEST(ElementOwnershipModeTests, hashOnPrimitiveArgIsRejected) {
    Compiler compiler;
    auto module = compileForInspection(compiler, kSrc, "test.Ut");
    auto box = module->getStructures()["test.Box"];
    ASSERT_NE(box, nullptr);
    auto& cmap = cajeta::CajetaType::getCanonicalMap();
    CajetaTypePtr i32 = cmap["int32"];
    ASSERT_NE(i32, nullptr);

    try {
        box->instantiate({i32}, {true});
        FAIL() << "expected '#' on int32 to be rejected (spec 8.1.1)";
    } catch (cajeta::Exception& e) {
        std::string msg = e.getMessage();
        EXPECT_NE(msg.find("int32"), std::string::npos) << msg;
        EXPECT_NE(msg.find("ownership"), std::string::npos) << msg;
    }
    auto borrow = box->instantiate({i32}, {false});
    ASSERT_NE(borrow, nullptr);
    EXPECT_FALSE(borrow->isTypeArgumentOwning(0));
}

// §8.1.2 — `#` on a POD value type (no heap-owning fields) is a compile
// error; the borrow instantiation stays fine.
TEST(ElementOwnershipModeTests, hashOnPodValueTypeIsRejected) {
    auto src =
        "package test;\n"
        "public record Pod { float64 x; }\n"
        "public class Box<T> { public T value; }\n"
        "public class Ut { public static int32 run() { return 0; } }\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.Ut");
    auto box = module->getStructures()["test.Box"];
    ASSERT_NE(box, nullptr);
    CajetaTypePtr pod = module->getStructures()["test.Pod"];
    ASSERT_NE(pod, nullptr);

    try {
        box->instantiate({pod}, {true});
        FAIL() << "expected '#' on a POD value type to be rejected (spec 8.1.2)";
    } catch (cajeta::Exception& e) {
        std::string msg = e.getMessage();
        EXPECT_NE(msg.find("test.Pod"), std::string::npos) << msg;
    }
    auto borrow = box->instantiate({pod}, {false});
    ASSERT_NE(borrow, nullptr);
    EXPECT_FALSE(borrow->isTypeArgumentOwning(0));
}

// §8.1.2 (accept half — the 2.1.4 headline) — `#` on a SHARED-CAPABLE value
// type (a record transitively owning heap payload via a Utf8 field) is
// accepted and instantiates owning.
TEST(ElementOwnershipModeTests, hashOnSharedCapableValueIsAccepted) {
    auto src =
        "package test;\n"
        "public record Quote { Utf8 sym; float64 px; }\n"
        "public class Box<T> { public T value; }\n"
        "public class Ut { public static int32 run() { return 0; } }\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.Ut");
    auto box = module->getStructures()["test.Box"];
    ASSERT_NE(box, nullptr);
    CajetaTypePtr quote = module->getStructures()["test.Quote"];
    ASSERT_NE(quote, nullptr);

    auto owning = box->instantiate({quote}, {true});
    ASSERT_NE(owning, nullptr);
    EXPECT_TRUE(owning->isTypeArgumentOwning(0));
}

// §8.1.1 at the SOURCE level — a `Box<#int32>` field is a compile error
// through the normal parse→instantiate threading (not just direct calls).
TEST(ElementOwnershipModeTests, sourceLevelHashOnPrimitiveIsCompileError) {
    auto src =
        "package test;\n"
        "public class Box<T> { public T value; }\n"
        "public class Ut {\n"
        "  public Box<#int32> bad;\n"
        "  public static int32 run() { return 0; }\n"
        "}\n";
    Compiler compiler;
    EXPECT_ANY_THROW(compileForInspection(compiler, src, "test.Ut"));
}

// ---------------------------------------------------------------------------
// Unit 4A — provenance + `#` dissolution at monomorphization (spec §3.1.1-2,
// §4.2, §4.1.4). An author `#K` formal is a real transfer position only under
// an owning type argument; under a plain instantiation it dissolves to a
// borrow. Both instantiations record which type parameter the formal / return
// came from, so the call-site agreement check (4B) can pair position with mode.
// ---------------------------------------------------------------------------

TEST(ElementOwnershipModeTests, authorHashFormalDissolvesUnderBorrowMode) {
    auto src =
        "package test;\n"
        "public class Elem { }\n"
        "public class Cache<K> {\n"
        "  public K store;\n"
        "  public void put(#K k) { }\n"
        "  public #K take() { return null; }\n"
        "}\n"
        "public class Ut {\n"
        "  public Cache<#Elem> owned;\n"
        "  public Cache<Elem> scratch;\n"
        "  public static int32 run() { return 0; }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.Ut");

    CajetaClassPtr owning, borrow;
    for (auto& [name, klass] : module->getStructures()) {
        if (name.rfind("test.Cache<", 0) != 0 || !klass) continue;
        if (klass->isTypeArgumentOwning(0)) owning = klass;
        else borrow = klass;
    }
    ASSERT_NE(owning, nullptr);
    ASSERT_NE(borrow, nullptr);

    auto findMethod = [](const CajetaClassPtr& cls, const std::string& mname)
            -> cajeta::MethodPtr {
        for (auto& kv : cls->getMethods()) {
            if (kv.second && kv.second->getName() == mname) return kv.second;
        }
        return nullptr;
    };
    auto formalOf = [](const cajeta::MethodPtr& m, const std::string& pname)
            -> cajeta::FormalParameterPtr {
        for (auto& fp : m->getParameterList()) {
            if (fp && fp->getName() == pname) return fp;
        }
        return nullptr;
    };

    // put(#K k): owning mode keeps the transfer; borrow mode dissolves it.
    // Both record the K provenance (index 0).
    auto owningPut = findMethod(owning, "put");
    auto borrowPut = findMethod(borrow, "put");
    ASSERT_NE(owningPut, nullptr);
    ASSERT_NE(borrowPut, nullptr);
    auto owningK = formalOf(owningPut, "k");
    auto borrowK = formalOf(borrowPut, "k");
    ASSERT_NE(owningK, nullptr);
    ASSERT_NE(borrowK, nullptr);
    EXPECT_TRUE(owningK->isTransferred());
    EXPECT_FALSE(borrowK->isTransferred());   // §4.2 dissolved
    EXPECT_EQ(owningK->getOriginTypeParamIndex(), 0);
    EXPECT_EQ(borrowK->getOriginTypeParamIndex(), 0);

    // #K take(): return provenance recorded on both; NOT dissolved (the
    // borrow-mode call is 4B's extractor error, not a silent borrow).
    auto owningTake = findMethod(owning, "take");
    auto borrowTake = findMethod(borrow, "take");
    ASSERT_NE(owningTake, nullptr);
    ASSERT_NE(borrowTake, nullptr);
    EXPECT_TRUE(owningTake->isReturnsOwnership());
    EXPECT_TRUE(borrowTake->isReturnsOwnership());
    EXPECT_EQ(owningTake->getOriginReturnTypeParamIndex(), 0);
    EXPECT_EQ(borrowTake->getOriginReturnTypeParamIndex(), 0);

    // The concrete-class control: Elem's methods carry no provenance.
    auto elem = std::dynamic_pointer_cast<cajeta::CajetaClass>(
        module->getStructures()["test.Elem"]);
    ASSERT_NE(elem, nullptr);
    for (auto& kv : elem->getMethods()) {
        if (!kv.second) continue;
        EXPECT_EQ(kv.second->getOriginReturnTypeParamIndex(), -1);
        for (auto& fp : kv.second->getParameterList()) {
            if (fp) EXPECT_EQ(fp->getOriginTypeParamIndex(), -1);
        }
    }
}

// ---------------------------------------------------------------------------
// Unit 5 — declaration-`#` (owning-required) + inheritance contagion
// (spec §4.1.5, §8.6). `class Vault<#K,V>` requires every instantiation to
// supply `#` for K; an extends edge must satisfy the requirement with a
// spelled `#` (concrete or reprojected) — passing a plain arg through is
// laundering, an error at the declaration.
// ---------------------------------------------------------------------------

// 5.1.1 — `Vault<#Elem,V>` compiles; `Vault<Elem,V>` is a compile error.
TEST(ElementOwnershipModeTests, owningRequiredParamRejectsPlainInstantiation) {
    auto src =
        "package test;\n"
        "public class Elem { }\n"
        "public class Vault<#K, V> { public K key; public V val; }\n"
        "public class Ut {\n"
        "  public Vault<#Elem, Elem> ok;\n"
        "  public static int32 run() { return 0; }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.Ut");
    auto vault = module->getStructures()["test.Vault"];
    ASSERT_NE(vault, nullptr);
    CajetaTypePtr elem = module->getStructures()["test.Elem"];
    ASSERT_NE(elem, nullptr);
    // The declared owning field resolved (satisfies the requirement)...
    bool sawOwning = false;
    for (auto& [name, klass] : module->getStructures()) {
        if (name.rfind("test.Vault<", 0) == 0 && klass
                && klass->isTypeArgumentOwning(0)) sawOwning = true;
    }
    EXPECT_TRUE(sawOwning);
    // ...and a plain instantiation of K is rejected.
    try {
        vault->instantiate({elem, elem}, {false, false});
        FAIL() << "expected owning-required rejection (spec 4.1.5)";
    } catch (cajeta::Exception& e) {
        std::string msg = e.getMessage();
        EXPECT_NE(msg.find("owning-required"), std::string::npos) << msg;
        EXPECT_NE(msg.find("K"), std::string::npos) << msg;
    }
}

// 5.1.2 — laundering: `class Leaky<K,V> extends Vault<K,V>` is a declaration
// error naming the satisfy / reproject fixes.
TEST(ElementOwnershipModeTests, launderingExtendsEdgeIsRejected) {
    auto src =
        "package test;\n"
        "public class Elem { }\n"
        "public class Vault<#K, V> { public K key; }\n"
        "public class Leaky<K, V> extends Vault<K, V> { }\n"
        "public class Ut { public static int32 run() { return 0; } }\n";
    Compiler compiler;
    try {
        compileForInspection(compiler, src, "test.Ut");
        FAIL() << "expected laundering rejection (spec 8.6)";
    } catch (cajeta::Exception& e) {
        std::string msg = e.getMessage();
        EXPECT_NE(msg.find("Leaky"), std::string::npos) << msg;
        EXPECT_NE(msg.find("#K"), std::string::npos) << msg;      // reproject fix
        EXPECT_NE(msg.find("owning-required"), std::string::npos) << msg;
    }
}

// 5.1.3 — satisfy (spelled concrete `#Elem`) and reproject (spelled `#K` on a
// `#`-declared own parameter) both compile.
TEST(ElementOwnershipModeTests, satisfyAndReprojectExtendsEdgesCompile) {
    auto src =
        "package test;\n"
        "public class Elem { }\n"
        "public class Vault<#K, V> { public K key; }\n"
        "public class SVault<V> extends Vault<#Elem, V> { }\n"
        "public class MVault<#K, V> extends Vault<#K, V> { }\n"
        "public class Ut { public static int32 run() { return 0; } }\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.Ut");
    EXPECT_NE(module->getStructures()["test.SVault"], nullptr);
    EXPECT_NE(module->getStructures()["test.MVault"], nullptr);
}

// 5.1.4 — a dual-mode base (no declaration-`#`) is unaffected: a plain
// pass-through subclass compiles and stays dual-mode.
TEST(ElementOwnershipModeTests, dualModeBaseSubclassUnaffected) {
    auto src =
        "package test;\n"
        "public class Elem { }\n"
        "public class Duo<K, V> { public K key; }\n"
        "public class Sub<K, V> extends Duo<K, V> { }\n"
        "public class Ut { public static int32 run() { return 0; } }\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.Ut");
    EXPECT_NE(module->getStructures()["test.Sub"], nullptr);
}

// 5.1.5 — a REPROJECTED requirement binds the next edge too: extending
// `MVault<#K,V>` with plain K launders and errors.
TEST(ElementOwnershipModeTests, reprojectedRequirementPropagates) {
    auto src =
        "package test;\n"
        "public class Elem { }\n"
        "public class Vault<#K, V> { public K key; }\n"
        "public class MVault<#K, V> extends Vault<#K, V> { }\n"
        "public class Deep<K, V> extends MVault<K, V> { }\n"
        "public class Ut { public static int32 run() { return 0; } }\n";
    Compiler compiler;
    EXPECT_ANY_THROW(compileForInspection(compiler, src, "test.Ut"));
}

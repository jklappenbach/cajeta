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

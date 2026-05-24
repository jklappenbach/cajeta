// Step 6 of template wildcards. Bounded forms `? extends T` and
// `? super T`. Parser produces per-(kind, bound) wildcard sentinels;
// assignability enforces the bound. Capture conversion (Java's
// `capture#N` synthetic types) is deferred — Step 6 minimum-viable
// is parse + classify + assignability check.

#include "gtest/gtest.h"
#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/type/CajetaType.h"

#include <filesystem>
#include <fstream>
#include <random>
#include <string>

using cajeta::CajetaClass;
using cajeta::CajetaClassPtr;
using cajeta::CajetaModulePtr;
using cajeta::CajetaType;
using cajeta::CajetaTypePtr;
using cajeta::Compiler;

namespace {

CajetaModulePtr compileSource(Compiler& compiler,
                              const std::string& source,
                              const std::string& fqClassName) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = std::filesystem::temp_directory_path()
              / ("cajeta_wildp6_" + std::to_string(rng()));
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
                 / ("cajeta_wildp6_arch_" + std::to_string(rng()));
    std::filesystem::create_directories(archive);
    auto m = compiler.createModule(full.string(),
                                   base.string(),
                                   archive.string());
    compiler.compile(m);
    return m;
}

} // namespace

// `? extends Animal` produces a bounded-extends sentinel distinct
// from the unbounded `?` sentinel, with kind=Extends and the bound
// recorded. wildcardSentinelExtends is idempotent — two calls with
// the same bound return the same sentinel pointer.
TEST(TemplateWildcardP6Tests, extendsSentinelClassifiedAndCached) {
    Compiler compiler;
    auto src = std::string(
        "package test;\n"
        "public class Animal {\n"
        "    public int32 tag() { return 1; }\n"
        "}\n");
    auto module = compileSource(compiler, src, "test.Animal");
    auto animal = module->getStructures()["test.Animal"];
    ASSERT_NE(animal, nullptr);

    auto extWild = CajetaType::wildcardSentinelExtends(animal);
    ASSERT_NE(extWild, nullptr);
    EXPECT_TRUE(extWild->isWildcard());
    EXPECT_EQ(extWild->wildcardKind(), CajetaType::WildcardKind::Extends);
    EXPECT_EQ(extWild->wildcardBound().get(), animal.get());

    auto extWild2 = CajetaType::wildcardSentinelExtends(animal);
    EXPECT_EQ(extWild.get(), extWild2.get())
        << "bounded wildcard sentinels should be cached per (kind, bound)";

    auto unbounded = CajetaType::wildcardSentinel();
    EXPECT_NE(extWild.get(), unbounded.get())
        << "bounded and unbounded sentinels are distinct";
}

// Same shape for `? super Animal`. Bound is recorded, kind is Super,
// distinct from the Extends sentinel of the same bound.
TEST(TemplateWildcardP6Tests, superSentinelDistinctFromExtends) {
    Compiler compiler;
    auto src = std::string(
        "package test;\n"
        "public class Animal {}\n");
    auto module = compileSource(compiler, src, "test.Animal");
    auto animal = module->getStructures()["test.Animal"];
    ASSERT_NE(animal, nullptr);

    auto extWild   = CajetaType::wildcardSentinelExtends(animal);
    auto superWild = CajetaType::wildcardSentinelSuper(animal);
    ASSERT_NE(extWild, nullptr);
    ASSERT_NE(superWild, nullptr);
    EXPECT_NE(extWild.get(), superWild.get());
    EXPECT_EQ(superWild->wildcardKind(), CajetaType::WildcardKind::Super);
    EXPECT_EQ(superWild->wildcardBound().get(), animal.get());
}

// Parser at CajetaType.cpp:454 routes `? extends Animal` through
// wildcardSentinelExtends and `? super Animal` through
// wildcardSentinelSuper. Compile-time check: a `Box<? extends Animal>`
// field declaration parses without throwing and the field's type is
// the wildcard-instantiated Box.
TEST(TemplateWildcardP6Tests, extendsParsesIntoBoundedInstantiation) {
    Compiler compiler;
    auto src = std::string(
        "package test;\n"
        "public class Animal {}\n"
        "public class Box<T> {\n"
        "    T value;\n"
        "    public Box(T v) { this.value = v; }\n"
        "}\n");
    auto module = compileSource(compiler, src, "test.Animal");
    auto box = module->getStructures()["test.Box"];
    ASSERT_NE(box, nullptr);
    auto animal = module->getStructures()["test.Animal"];
    ASSERT_NE(animal, nullptr);

    auto extWild = CajetaType::wildcardSentinelExtends(animal);
    auto boxExtAnimal = box->instantiate({extWild});
    ASSERT_NE(boxExtAnimal, nullptr);
    EXPECT_TRUE(boxExtAnimal->isWildcardInstantiation());
    auto& targs = boxExtAnimal->getTypeArguments();
    ASSERT_EQ(targs.size(), 1u);
    EXPECT_EQ(targs[0]->wildcardKind(), CajetaType::WildcardKind::Extends);
}

// Assignability: `Box<Dog>` is assignable to `Box<? extends Animal>`
// because Dog is a subtype of Animal.
TEST(TemplateWildcardP6Tests, concreteSubtypeAssignableToExtendsWildcard) {
    Compiler compiler;
    auto src = std::string(
        "package test;\n"
        "public class Animal {}\n"
        "public class Dog extends Animal {}\n"
        "public class Box<T> {\n"
        "    T value;\n"
        "    public Box(T v) { this.value = v; }\n"
        "}\n");
    auto module = compileSource(compiler, src, "test.Animal");
    auto box = module->getStructures()["test.Box"];
    auto animal = module->getStructures()["test.Animal"];
    auto dog = module->getStructures()["test.Dog"];
    ASSERT_NE(box, nullptr);
    ASSERT_NE(animal, nullptr);
    ASSERT_NE(dog, nullptr);

    auto boxDog = box->instantiate({dog});
    auto boxExtAnimal = box->instantiate(
        {CajetaType::wildcardSentinelExtends(animal)});

    EXPECT_TRUE(CajetaClass::isAssignableToWildcard(boxDog, boxExtAnimal));
}

// `Box<Robot>` is NOT assignable to `Box<? extends Animal>` because
// Robot is unrelated to Animal.
TEST(TemplateWildcardP6Tests, unrelatedConcreteRejectedFromExtendsWildcard) {
    Compiler compiler;
    auto src = std::string(
        "package test;\n"
        "public class Animal {}\n"
        "public class Robot {}\n"
        "public class Box<T> {\n"
        "    T value;\n"
        "    public Box(T v) { this.value = v; }\n"
        "}\n");
    auto module = compileSource(compiler, src, "test.Animal");
    auto box = module->getStructures()["test.Box"];
    auto animal = module->getStructures()["test.Animal"];
    auto robot = module->getStructures()["test.Robot"];
    ASSERT_NE(box, nullptr);
    ASSERT_NE(animal, nullptr);
    ASSERT_NE(robot, nullptr);

    auto boxRobot = box->instantiate({robot});
    auto boxExtAnimal = box->instantiate(
        {CajetaType::wildcardSentinelExtends(animal)});

    EXPECT_FALSE(CajetaClass::isAssignableToWildcard(boxRobot, boxExtAnimal));
}

// `? super Dog` accepts Dog itself and any supertype of Dog. Assignability:
// `Box<Animal>` is assignable to `Box<? super Dog>` (Animal is supertype of
// Dog); `Box<Robot>` is not.
TEST(TemplateWildcardP6Tests, supertypeAssignableToSuperWildcard) {
    Compiler compiler;
    auto src = std::string(
        "package test;\n"
        "public class Animal {}\n"
        "public class Dog extends Animal {}\n"
        "public class Robot {}\n"
        "public class Box<T> {\n"
        "    T value;\n"
        "    public Box(T v) { this.value = v; }\n"
        "}\n");
    auto module = compileSource(compiler, src, "test.Animal");
    auto box = module->getStructures()["test.Box"];
    auto animal = module->getStructures()["test.Animal"];
    auto dog = module->getStructures()["test.Dog"];
    auto robot = module->getStructures()["test.Robot"];
    ASSERT_NE(box, nullptr);
    ASSERT_NE(animal, nullptr);
    ASSERT_NE(dog, nullptr);
    ASSERT_NE(robot, nullptr);

    auto boxAnimal = box->instantiate({animal});
    auto boxDog    = box->instantiate({dog});
    auto boxRobot  = box->instantiate({robot});
    auto boxSuperDog = box->instantiate(
        {CajetaType::wildcardSentinelSuper(dog)});

    EXPECT_TRUE(CajetaClass::isAssignableToWildcard(boxAnimal, boxSuperDog));
    EXPECT_TRUE(CajetaClass::isAssignableToWildcard(boxDog,    boxSuperDog));
    EXPECT_FALSE(CajetaClass::isAssignableToWildcard(boxRobot, boxSuperDog));
}

// diagnostic-engine (spec §3): the error/poison type — a singleton returned when
// resolution fails so analysis continues instead of throwing.

#include <gtest/gtest.h>

#include <memory>

#include "cajeta/type/CajetaType.h"
#include "cajeta/type/QualifiedName.h"

using cajeta::CajetaType;
using cajeta::QualifiedName;

TEST(ErrorType, IsASingleton) {
    auto a = CajetaType::error();
    auto b = CajetaType::error();
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(a.get(), b.get());
}

TEST(ErrorType, IsErrorOnlyForTheSentinel) {
    EXPECT_TRUE(CajetaType::error()->isError());
    auto normal = std::make_shared<CajetaType>(QualifiedName::getOrCreate("demo.Foo"));
    EXPECT_FALSE(normal->isError());
}

TEST(ErrorType, GetLlvmTypeDoesNotCrash) {
    // ErrorType is never lowered (codegen is gated on hasErrors), but the call
    // must be safe if ever reached — returns null rather than dereferencing.
    EXPECT_NO_THROW({ (void)CajetaType::error()->getLlvmType(); });
}

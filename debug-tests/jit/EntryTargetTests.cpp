//
// Fast, pure-function tests for the JIT host's entry-name resolution. No
// compilation or JIT — these run in microseconds and pin the dotted→mangled
// mapping the host uses to locate a program's entry method.
//
#include <gtest/gtest.h>

#include "cajeta/jit/CajetaJitHost.h"

using cajeta::jit::entryTargetFromDotted;

TEST(EntryTarget, ConvertsDottedToMangledPrefix) {
    EXPECT_EQ(entryTargetFromDotted("demo.Hello.main"), "demo.Hello::main");
}

TEST(EntryTarget, MultiSegmentPackage) {
    EXPECT_EQ(entryTargetFromDotted("a.b.c.Widget.run"), "a.b.c.Widget::run");
}

TEST(EntryTarget, NoDotsReturnsEmpty) {
    EXPECT_EQ(entryTargetFromDotted("NoDots"), "");
}

TEST(EntryTarget, TrailingDotReturnsEmpty) {
    EXPECT_EQ(entryTargetFromDotted("demo.Hello."), "");
}

TEST(EntryTarget, LeadingDotReturnsEmpty) {
    EXPECT_EQ(entryTargetFromDotted(".main"), "");
}

TEST(EntryTarget, EmptyReturnsEmpty) {
    EXPECT_EQ(entryTargetFromDotted(""), "");
}

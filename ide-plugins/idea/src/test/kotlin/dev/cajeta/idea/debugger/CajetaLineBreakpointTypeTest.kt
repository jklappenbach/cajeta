package dev.cajeta.idea.debugger

import com.intellij.testFramework.fixtures.BasePlatformTestCase

/**
 * Platform-fixture test: the line breakpoint type accepts `.cajeta` files and
 * rejects others (so the gutter only offers breakpoints in Cajeta sources).
 */
class CajetaLineBreakpointTypeTest : BasePlatformTestCase() {

    private val type = CajetaLineBreakpointType()

    fun testCanPutBreakpointInCajetaFile() {
        val file = myFixture.configureByText("Calc.cajeta", "package demo;\npublic class Calc {}\n")
        assertTrue(type.canPutAt(file.virtualFile, 0, project))
    }

    fun testCannotPutBreakpointInTextFile() {
        val file = myFixture.configureByText("notes.txt", "hello\n")
        assertFalse(type.canPutAt(file.virtualFile, 0, project))
    }
}

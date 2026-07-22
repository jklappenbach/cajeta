package dev.cajeta.idea.debugger

import com.intellij.execution.process.ProcessOutputTypes
import com.intellij.execution.ui.ConsoleView
import com.intellij.execution.ui.ConsoleViewContentType
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.lang.reflect.InvocationHandler
import java.lang.reflect.Method
import java.lang.reflect.Proxy

/**
 * The debuggee's stdout reached the ProcessHandler but never rendered in the
 * Console. Cause: on CLion 2026.2 the platform's XDebugProcess.createConsole()
 * builds a console and returns it WITHOUT calling attachToProcess — verified by
 * disassembling intellij.platform.debugger.jar. Nothing else attaches under the
 * split-debugger session path, so every notifyTextAvailable went to a listener
 * list the console was not in.
 *
 * The handler therefore owns attachment, and must also replay: the launch
 * narration and a warm session's first program output are emitted during
 * sessionInitialized, before the platform ever asks for a console, and text
 * emitted before attachToProcess is otherwise dropped.
 */
class ConsoleAttachTest {

    /** Records the ConsoleView calls the handler makes. */
    private class Recorder : InvocationHandler {
        val attached = mutableListOf<Any?>()
        val printed = mutableListOf<String>()
        val printedTypes = mutableListOf<ConsoleViewContentType>()

        override fun invoke(proxy: Any, method: Method, args: Array<out Any?>?): Any? {
            when (method.name) {
                "attachToProcess" -> attached.add(args?.get(0))
                "print" -> {
                    printed.add(args?.get(0) as String)
                    printedTypes.add(args.get(1) as ConsoleViewContentType)
                }
                "hashCode" -> return System.identityHashCode(proxy)
                "equals" -> return proxy === args?.get(0)
                "toString" -> return "FakeConsoleView"
            }
            return null
        }
    }

    private fun fakeConsole(recorder: Recorder): ConsoleView =
        Proxy.newProxyInstance(
            ConsoleView::class.java.classLoader,
            arrayOf(ConsoleView::class.java),
            recorder,
        ) as ConsoleView

    @Test
    fun `attaches the console to this handler`() {
        val handler = CajetaDebugProcessHandler()
        val recorder = Recorder()

        handler.attachConsole(fakeConsole(recorder))

        assertEquals(listOf<Any?>(handler), recorder.attached)
    }

    @Test
    fun `replays output emitted before the console attached`() {
        val handler = CajetaDebugProcessHandler()
        val recorder = Recorder()

        handler.emitNarration("cajeta: using cached build\n")
        handler.emitOutput("=== Cajeta language tour ===\n")
        handler.emitError("warning: something\n")

        handler.attachConsole(fakeConsole(recorder))

        assertEquals(
            listOf(
                "cajeta: using cached build\n",
                "=== Cajeta language tour ===\n",
                "warning: something\n",
            ),
            recorder.printed,
        )
    }

    /**
     * Program stdout renders green, stderr red, launch narration plain. The
     * green key is our own: registering a color against ProcessOutputTypes
     * .STDOUT would repaint every console in the IDE.
     */
    @Test
    fun `colors stdout green, stderr red, narration plain`() {
        val handler = CajetaDebugProcessHandler()
        val recorder = Recorder()

        handler.emitOutput("out\n")
        handler.emitError("err\n")
        handler.emitNarration("cajeta: compile finished\n")
        handler.attachConsole(fakeConsole(recorder))

        assertEquals(
            listOf(
                CajetaDebugProcessHandler.STDOUT_CONTENT,
                ConsoleViewContentType.ERROR_OUTPUT,
                ConsoleViewContentType.SYSTEM_OUTPUT,
            ),
            recorder.printedTypes,
        )
        assertNotEquals(
            "stdout must not reuse the platform stdout key",
            ProcessOutputTypes.STDOUT,
            CajetaDebugProcessHandler.CAJETA_STDOUT,
        )
        val green = CajetaDebugProcessHandler.STDOUT_CONTENT.attributes.foregroundColor
        assertNotNull("stdout needs an explicit foreground", green)
        assertTrue(
            "stdout foreground should be green-dominant, was $green",
            green.green > green.red && green.green > green.blue,
        )
    }

    @Test
    fun `does not replay output emitted after the console attached`() {
        val handler = CajetaDebugProcessHandler()
        val recorder = Recorder()

        handler.attachConsole(fakeConsole(recorder))
        handler.emitOutput("after\n")

        // Post-attach text travels the normal listener path (the console is a
        // listener now); replaying it here would double it.
        assertTrue(recorder.printed.isEmpty())
    }

    @Test
    fun `replays only once across repeated attachment`() {
        val handler = CajetaDebugProcessHandler()
        handler.emitOutput("once\n")

        val first = Recorder()
        handler.attachConsole(fakeConsole(first))
        val second = Recorder()
        handler.attachConsole(fakeConsole(second))

        assertEquals(listOf("once\n"), first.printed)
        assertTrue(second.printed.isEmpty())
    }

    @Test
    fun `bounds the replay buffer when no console ever attaches`() {
        val handler = CajetaDebugProcessHandler()
        repeat(CajetaDebugProcessHandler.MAX_REPLAY_LINES * 3) { handler.emitOutput("line $it\n") }

        val recorder = Recorder()
        handler.attachConsole(fakeConsole(recorder))

        assertTrue(
            "replay must stay bounded, was ${recorder.printed.size}",
            recorder.printed.size <= CajetaDebugProcessHandler.MAX_REPLAY_LINES,
        )
        assertEquals("line 0\n", recorder.printed.first())
    }
}

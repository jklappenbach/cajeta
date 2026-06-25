package dev.cajeta.idea.markdown

import com.intellij.openapi.application.ApplicationManager
import com.intellij.openapi.diagnostic.Logger
import com.intellij.openapi.util.Disposer
import com.intellij.ui.jcef.JBCefApp
import com.intellij.ui.jcef.JBCefBrowserBuilder
import com.intellij.ui.jcef.JBCefJSQuery
import com.intellij.util.concurrency.AppExecutorUtil
import com.intellij.util.ui.ImageUtil
import org.cef.browser.CefBrowser
import org.cef.handler.CefLoadHandlerAdapter
import java.awt.Window
import java.awt.image.BufferedImage
import java.util.concurrent.TimeUnit
import javax.swing.JWindow

/**
 * Renders a themed markdown HTML document to a [BufferedImage] using JCEF
 * (Chromium) off-screen, then disposes the browser — so no live browser is
 * retained per fold region (nothing to leak). Experimental and best-effort: any
 * failure or timeout simply never calls back, and the caller keeps its Swing
 * fallback. EDT-only (JCEF + Swing component access).
 *
 * Mechanism: an OSR [com.intellij.ui.jcef.JBCefBrowser] is hosted in an
 * off-screen [JWindow] (CEF only paints frames for a realized, visible surface);
 * once the page finishes loading we read `document.body.scrollHeight` back
 * through a [JBCefJSQuery], paint the OSR component into an image at that height,
 * and tear everything down.
 *
 * NOTE (experimental): off-screen OSR capture timing and HiDPI scaling are the
 * parts most likely to need tuning on real hardware — see the user-verify
 * checklist. Gated behind the JCEF surface setting; Swing is the default.
 */
object JcefHtmlImageRenderer {

    private val log = Logger.getInstance(JcefHtmlImageRenderer::class.java)
    private const val MAX_HEIGHT = 6000
    private const val TIMEOUT_MS = 4000L

    fun render(htmlDoc: String, width: Int, onImage: (BufferedImage) -> Unit) {
        if (width <= 0 || !JBCefApp.isSupported()) return
        ApplicationManager.getApplication().invokeLater {
            try {
                start(htmlDoc, width, onImage)
            } catch (t: Throwable) {
                log.warn("JCEF markdown render failed to start", t)
            }
        }
    }

    private fun start(htmlDoc: String, width: Int, onImage: (BufferedImage) -> Unit) {
        val browser = JBCefBrowserBuilder().setOffScreenRendering(true).build()
        val component = browser.component
        // CEF only renders frames for a realized, visible surface, so host the
        // OSR component in a focus-less window parked far off-screen.
        val window = JWindow().apply {
            focusableWindowState = false
            type = Window.Type.UTILITY
            setSize(width, MAX_HEIGHT)
            setLocation(-32000, -32000)
            contentPane.add(component)
            isVisible = true
        }
        val query = JBCefJSQuery.create(browser)

        var done = false
        fun finish(image: BufferedImage?) {
            if (done) return
            done = true
            try {
                if (image != null) onImage(image)
            } finally {
                window.isVisible = false
                window.dispose()
                Disposer.dispose(query)
                Disposer.dispose(browser)
            }
        }

        query.addHandler { heightStr ->
            val h = heightStr.trim().toDoubleOrNull()?.toInt()?.coerceIn(1, MAX_HEIGHT)
            ApplicationManager.getApplication().invokeLater {
                val img = runCatching { if (h != null) capture(component, width, h) else null }
                    .onFailure { log.warn("JCEF markdown capture failed", it) }
                    .getOrNull()
                finish(img)
            }
            null
        }

        browser.jbCefClient.addLoadHandler(
            object : CefLoadHandlerAdapter() {
                override fun onLoadingStateChange(
                    b: CefBrowser?,
                    isLoading: Boolean,
                    canGoBack: Boolean,
                    canGoForward: Boolean,
                ) {
                    if (!isLoading) {
                        val js = query.inject("String(document.body.scrollHeight)")
                        browser.cefBrowser.executeJavaScript(js, browser.cefBrowser.url, 0)
                    }
                }
            },
            browser.cefBrowser,
        )

        browser.loadHTML(htmlDoc)

        // Safety net: if no height callback arrives, give up (keep Swing fallback).
        AppExecutorUtil.getAppScheduledExecutorService().schedule(
            { ApplicationManager.getApplication().invokeLater { finish(null) } },
            TIMEOUT_MS, TimeUnit.MILLISECONDS,
        )
    }

    private fun capture(component: javax.swing.JComponent, width: Int, height: Int): BufferedImage {
        component.setBounds(0, 0, width, height)
        component.doLayout()
        val img = ImageUtil.createImage(width, height, BufferedImage.TYPE_INT_ARGB)
        val g = img.createGraphics()
        try {
            component.paint(g)
        } finally {
            g.dispose()
        }
        return img
    }
}

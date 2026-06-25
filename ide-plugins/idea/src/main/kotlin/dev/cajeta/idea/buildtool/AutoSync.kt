package dev.cajeta.idea.buildtool

import dev.cajeta.idea.settings.CajetaSettings

/**
 * The pure auto-sync decision (spec §13): a watched-manifest change maps, per the
 * auto-reload setting, to ignore / reload / prompt; and a failed re-discovery
 * keeps the prior tree usable (§13.2.3). No `com.intellij.*`.
 */
object AutoSync {

    enum class Action { IGNORE, RELOAD, PROMPT }

    fun decide(autoReload: String): Action = when (autoReload) {
        CajetaSettings.AUTO_RELOAD_ALWAYS -> Action.RELOAD
        CajetaSettings.AUTO_RELOAD_NEVER -> Action.IGNORE
        else -> Action.PROMPT   // default (prompt), like Gradle's "reload" notification
    }

    /** Models to display after a re-discovery: the new set if anything was
     *  discovered, else the prior set (a bad manifest never wipes the tree). */
    fun <K, V> reconcile(prior: Map<K, V>, discovered: Map<K, V>): Map<K, V> =
        if (discovered.isEmpty()) prior else discovered
}

/**
 * A leading-edge-reset debounce (spec §13.1): each change pushes the due time out
 * by [windowMs], so a burst of edits coalesces into one reload once quiet.
 * Time is injected (no real clock) so it is unit-tested deterministically.
 */
class Debounce(private val windowMs: Long) {

    private var dueAt: Long = 0L

    fun onChange(nowMs: Long) { dueAt = nowMs + windowMs }

    fun isDue(nowMs: Long): Boolean = dueAt > 0L && nowMs >= dueAt

    fun consume() { dueAt = 0L }
}

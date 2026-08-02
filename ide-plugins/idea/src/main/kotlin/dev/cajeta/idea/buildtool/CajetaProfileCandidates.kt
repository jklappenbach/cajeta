package dev.cajeta.idea.buildtool

import dev.cajeta.idea.debugger.Json

/**
 * The DI profiles a project declares, for the Profile field to offer instead
 * of making the developer remember them.
 *
 * The list comes from the COMPILER (`--lint <root> --list-profiles`), which is
 * what makes it trustworthy: it already parses the three `@Profile` spellings
 * for DI selection, and it sees profiles declared inside dependency archives
 * that a plugin-side text scan never would.
 *
 * Discovery is a SUGGESTION, never a constraint. The field stays editable, a
 * typed value is persisted whatever the list held, and a failed query degrades
 * to exactly today's behaviour — a text field. That is the same discipline
 * `EntryMethodCandidates` uses, and for the same reason: a closed list that
 * omits a valid value blocks the developer, which is worse than no list.
 */
object CajetaProfileCandidates {

    /** The AOT default when the field is left empty — offered explicitly,
     *  because a blank entry that silently means "prod" is a thing people
     *  misread. */
    const val DEFAULT_PROFILE = "prod"

    /**
     * What a discovery attempt produced. An EMPTY list and a FAILED query are
     * different facts: the first says this project declares no profiles, the
     * second says nobody could look. A UI that renders them alike tells the
     * developer something untrue.
     */
    data class Result(
        val profiles: List<String>,
        val queried: Boolean,
        val error: String? = null,
    ) {
        /** Null when there is nothing to explain. */
        fun emptyMessage(): String? = when {
            profiles.isNotEmpty() -> null
            !queried -> error ?: "Could not read profiles — type one, or build the project."
            else -> "This project declares no @Profile annotations."
        }

        /** What the dropdown shows: the default first, then the declared ones,
         *  never duplicated. */
        fun offered(): List<String> =
            (listOf(DEFAULT_PROFILE) + profiles).distinct()
    }

    /** Parse `{"profiles":[...]}`. Any other shape is a failed query, not an
     *  empty project — see [Result]. */
    fun parse(stdout: String): Result {
        val text = stdout.trim()
        if (text.isEmpty()) return Result(emptyList(), queried = false, error = "no output")
        // The query prints one JSON object; tolerate a build banner ahead of it.
        val start = text.indexOf('{')
        if (start < 0) return Result(emptyList(), queried = false, error = "no JSON in output")
        val root = try {
            Json.parse(text.substring(start)) as? Json.Obj
        } catch (e: Exception) {
            null
        } ?: return Result(emptyList(), queried = false, error = "unreadable output")
        val arr = root.entries["profiles"] as? Json.Arr
            ?: return Result(emptyList(), queried = false, error = "no profiles field")
        val names = arr.items.mapNotNull { (it as? Json.Str)?.value?.ifBlank { null } }
        return Result(names.distinct().sorted(), queried = true)
    }

    /** The argv for the query, so the caller does not spell it twice. */
    fun argvFor(compilerPath: String, sourceRoot: String): List<String> =
        listOf(compilerPath, "--lint", sourceRoot, "--list-profiles")
}

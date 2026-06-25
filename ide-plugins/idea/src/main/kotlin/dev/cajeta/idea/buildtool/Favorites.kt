package dev.cajeta.idea.buildtool

/** A favorited task: its name plus the root manifest it belongs to (spec §11). */
data class FavoriteRef(val manifestPath: String, val task: String)

/**
 * An immutable, deduped, ordered set of favorite tasks (spec §11.2.3) pinned at
 * the top of the tool window. [toggle] adds (to the end) or removes. Pure.
 */
data class Favorites(val refs: List<FavoriteRef>) {

    fun contains(ref: FavoriteRef): Boolean = ref in refs

    fun toggle(ref: FavoriteRef): Favorites =
        if (contains(ref)) Favorites(refs.filterNot { it == ref }) else Favorites(refs + ref)
}

/**
 * Binds a task to a saved run configuration (spec §11.1/§11.2.1): the saved
 * config runs the *same* command as the bound task — task + manifest + the
 * active profile/flavor, params seeded from their defaults — composed through the
 * pure [RunArgs] model so "saved" and "run-from-tree" never diverge. Pure.
 */
object SavedConfig {

    fun specFor(task: CajetaTask, manifestPath: String?, profile: String?, flavor: String?): TaskRunSpec =
        RunArgs.buildSpec(
            task = task,
            manifestPath = manifestPath,
            profile = profile,
            flavor = flavor,
            properties = emptyMap(),
            paramValues = RunArgs.initialValues(task),
        )

    fun configName(task: CajetaTask): String = "cajeta ${task.name}"
}

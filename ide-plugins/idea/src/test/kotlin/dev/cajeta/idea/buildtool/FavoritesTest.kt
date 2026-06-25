package dev.cajeta.idea.buildtool

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * W-buildtool unit 14.1.1: the pure favorites set + the saved-config binding
 * (spec §11). A "Save as Run Configuration" produces a config that runs the same
 * command as the bound task, and a favorite toggles into a deduped, ordered set
 * pinned at the top of the tool window. No `com.intellij.*`.
 */
class FavoritesTest {

    private val build = CajetaTask(
        name = "build",
        params = listOf(TaskParam(name = "flavor", default = "debug")),
    )

    @Test
    fun savedConfigRunsSameCommandAsBoundTask() {
        val spec = SavedConfig.specFor(build, "/p/cajeta.json", profile = "ci", flavor = "release")
        // bound to the task: same task + manifest + active profile/flavor, params
        // seeded from defaults.
        assertEquals("build", spec.task)
        assertEquals("/p/cajeta.json", spec.manifestPath)
        assertEquals("ci", spec.profile)
        assertEquals("release", spec.flavor)
        assertEquals(
            listOf("build", "--manifest=/p/cajeta.json", "--profile=ci", "--flavor=release", "-p", "flavor=debug"),
            CajetaCommandLine.runArgv(spec),
        )
        assertEquals("cajeta build", SavedConfig.configName(build))
    }

    @Test
    fun favoriteToggleAddsThenRemoves() {
        val ref = FavoriteRef("/p/cajeta.json", "build")
        var favs = Favorites(emptyList())
        favs = favs.toggle(ref)
        assertTrue(favs.contains(ref))
        favs = favs.toggle(ref)
        assertFalse(favs.contains(ref))
    }

    @Test
    fun favoritesAreDedupedAndOrderPreserved() {
        val a = FavoriteRef("/p/cajeta.json", "build")
        val b = FavoriteRef("/p/cajeta.json", "test")
        val favs = Favorites(emptyList()).toggle(a).toggle(b).toggle(a).toggle(a)  // a re-added after removal
        assertEquals(listOf(b, a), favs.refs)   // b stayed, a moved to end on re-add
    }
}

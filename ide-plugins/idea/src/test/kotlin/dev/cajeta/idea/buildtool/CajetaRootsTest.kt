package dev.cajeta.idea.buildtool

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File

/**
 * W-buildtool unit 13.1.1: the pure root-registry core (spec §10). Manages the
 * set of linked `cajeta.json` roots (link/unlink, deduped + stable order) and
 * expands a workspace manifest into its member child roots. No `com.intellij.*`.
 */
class CajetaRootsTest {

    @Test
    fun linkAddsDedupedAndStablySorted() {
        var roots = LinkedRoots(emptyList())
        roots = roots.link("/b/cajeta.json").link("/a/cajeta.json").link("/b/cajeta.json")
        assertEquals(listOf("/a/cajeta.json", "/b/cajeta.json"), roots.paths)
        assertTrue(roots.contains("/a/cajeta.json"))
    }

    @Test
    fun unlinkRemovesRoot() {
        val roots = LinkedRoots(listOf("/a/cajeta.json", "/b/cajeta.json")).unlink("/a/cajeta.json")
        assertEquals(listOf("/b/cajeta.json"), roots.paths)
    }

    @Test
    fun workspaceMembersResolveToMemberManifests() {
        val ws = """
            { "details": { "name": "p" },
              "workspace": { "members": ["shared/core", "apps/cli"] } }
        """.trimIndent()
        val members = CajetaRoots.workspaceMembers(ws, "/ws")
        assertEquals(
            listOf(File("/ws/shared/core", "cajeta.json").path, File("/ws/apps/cli", "cajeta.json").path),
            members,
        )
    }

    @Test
    fun nonWorkspaceManifestHasNoMembers() {
        val plain = """{ "details": { "name": "p" }, "tasks": {} }"""
        assertTrue(CajetaRoots.workspaceMembers(plain, "/p").isEmpty())
        // malformed input is tolerated (no throw), yields no members
        assertTrue(CajetaRoots.workspaceMembers("{ not json", "/p").isEmpty())
    }
}

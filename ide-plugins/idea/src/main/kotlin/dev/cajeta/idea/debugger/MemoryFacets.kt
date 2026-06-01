package dev.cajeta.idea.debugger

/**
 * Plugin-side mirror of the compiler's memory-facet vocabulary (CP7) and the
 * pure mapping from facets to a presentation descriptor for the Variables view
 * (CP7-2, FR-4 and FR-5).
 *
 * Everything here is platform-free so it unit-tests without an IntelliJ fixture
 * (FR-8.4): the enums parse from the DAP `cajeta` sub-object tags, and
 * [present] turns facets into a [FacetPresentation] of plain text + style flags.
 * [CajetaValue] does the thin translation of that descriptor into icons and
 * text attributes — the only part that needs the platform.
 */
enum class AllocClass {
    UNKNOWN, STACK, HEAP, SHARED;

    companion object {
        fun fromTag(tag: String?): AllocClass = when (tag) {
            "stack" -> STACK
            "heap" -> HEAP
            "shared" -> SHARED
            else -> UNKNOWN
        }
    }
}

enum class OwnershipRole {
    UNKNOWN, OWNER, BORROW, MOVED;

    companion object {
        fun fromTag(tag: String?): OwnershipRole = when (tag) {
            "owner" -> OWNER
            "borrow" -> BORROW
            "moved" -> MOVED
            else -> UNKNOWN
        }
    }
}

enum class LifetimeState {
    UNKNOWN, LIVE, MOVED_OUT, ABOUT_TO_DROP;

    companion object {
        fun fromTag(tag: String?): LifetimeState = when (tag) {
            "live" -> LIVE
            "moved-out" -> MOVED_OUT
            "about-to-drop" -> ABOUT_TO_DROP
            else -> UNKNOWN
        }
    }
}

/** The three orthogonal facets for one variable, decoded from the wire. */
data class MemoryFacets(
    val alloc: AllocClass = AllocClass.UNKNOWN,
    val ownership: OwnershipRole = OwnershipRole.UNKNOWN,
    val lifetime: LifetimeState = LifetimeState.UNKNOWN,
) {
    /** True when at least one facet is concretely known (drives whether we decorate). */
    val isKnown: Boolean
        get() = alloc != AllocClass.UNKNOWN ||
            ownership != OwnershipRole.UNKNOWN ||
            lifetime != LifetimeState.UNKNOWN

    companion object {
        val UNKNOWN = MemoryFacets()

        /** Decode the namespaced `cajeta` object from a `variables` entry. */
        fun parse(cajeta: Json?): MemoryFacets {
            if (cajeta == null) return UNKNOWN
            return MemoryFacets(
                alloc = AllocClass.fromTag(cajeta.opt("alloc")?.asString()),
                ownership = OwnershipRole.fromTag(cajeta.opt("ownership")?.asString()),
                lifetime = LifetimeState.fromTag(cajeta.opt("lifetime")?.asString()),
            )
        }
    }
}

/**
 * A platform-free description of how a variable should look: a compact appended
 * [tag] (the color-independent textual affordance, FR-5.3), a fuller [tooltip],
 * and style flags the platform layer maps to text attributes. [error] marks a
 * moved-out binding whose value must not read as trustworthy (FR-4.3).
 */
data class FacetPresentation(
    val tag: String,
    val tooltip: String,
    val bold: Boolean,
    val strikeout: Boolean,
    val grayed: Boolean,
    val error: Boolean,
)

/**
 * Map facets to their presentation. The compact tag lists only the noteworthy
 * pieces — ownership (owner/borrow/moved), allocation class, and a lifetime
 * state worth flagging (moved-out / about-to-drop) — skipping `unknown` and the
 * unremarkable `live` so the common case stays uncluttered. The tooltip spells
 * out every known facet. Owners are emphasised (bold) unless moved out; a
 * moved-out binding is struck, grayed, and flagged as an error to read.
 */
fun MemoryFacets.present(): FacetPresentation {
    val movedOut = lifetime == LifetimeState.MOVED_OUT

    val tagParts = mutableListOf<String>()
    ownershipTag()?.let { tagParts.add(it) }
    allocTag()?.let { tagParts.add(it) }
    lifetimeTag()?.let { tagParts.add(it) }
    val tag = tagParts.joinToString(" · ")

    val tipParts = mutableListOf<String>()
    ownershipPhrase()?.let { tipParts.add(it) }
    allocPhrase()?.let { tipParts.add(it) }
    lifetimePhrase()?.let { tipParts.add(it) }
    val tooltip = if (tipParts.isEmpty()) "no ownership metadata"
    else tipParts.joinToString(", ")

    return FacetPresentation(
        tag = tag,
        tooltip = tooltip,
        bold = ownership == OwnershipRole.OWNER && !movedOut,
        strikeout = movedOut,
        grayed = movedOut,
        error = movedOut,
    )
}

private fun MemoryFacets.ownershipTag(): String? = when (ownership) {
    OwnershipRole.OWNER -> "owner"
    OwnershipRole.BORROW -> "borrow"
    OwnershipRole.MOVED -> "moved"
    OwnershipRole.UNKNOWN -> null
}

private fun MemoryFacets.allocTag(): String? = when (alloc) {
    AllocClass.STACK -> "stack"
    AllocClass.HEAP -> "heap"
    AllocClass.SHARED -> "shared"
    AllocClass.UNKNOWN -> null
}

private fun MemoryFacets.lifetimeTag(): String? = when (lifetime) {
    LifetimeState.MOVED_OUT -> "moved-out"
    LifetimeState.ABOUT_TO_DROP -> "about-to-drop"
    // `live` and `unknown` are the unremarkable cases; omitted from the tag.
    LifetimeState.LIVE, LifetimeState.UNKNOWN -> null
}

private fun MemoryFacets.ownershipPhrase(): String? = when (ownership) {
    OwnershipRole.OWNER -> "owned"
    OwnershipRole.BORROW -> "borrowed"
    OwnershipRole.MOVED -> "moved out"
    OwnershipRole.UNKNOWN -> null
}

private fun MemoryFacets.allocPhrase(): String? = when (alloc) {
    AllocClass.STACK -> "stack-allocated"
    AllocClass.HEAP -> "heap-allocated"
    AllocClass.SHARED -> "shared (kernel-lifetime)"
    AllocClass.UNKNOWN -> null
}

private fun MemoryFacets.lifetimePhrase(): String? = when (lifetime) {
    LifetimeState.LIVE -> "live"
    LifetimeState.MOVED_OUT -> "moved out — reading is an error"
    LifetimeState.ABOUT_TO_DROP -> "about to drop at scope exit"
    LifetimeState.UNKNOWN -> null
}

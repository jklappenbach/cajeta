# ide-features — refactoring, hierarchy, call graph, and navigation

Status: draft (2026-07-12)
Depends on: `ide-symbol-index-spec.md` (all four features consume it)

## 1. Definition

### 1.1 Purpose
Four IDE features that turn the symbol layer into things a developer uses:
refactoring, an inheritance hierarchy view, a static call graph, and gutter
navigation between parents and children.

### 1.2 Scope
This spec assumes `ide-symbol-index` is delivered: a compiler-authoritative
cross-reference export, named PSI elements, reference adapters over that export,
and a persistent index of its five relations — covering project, dependency, and
stdlib source. Nothing here re-derives symbol data, and nothing here resolves
Cajeta semantics in Kotlin. Each feature below is a *presentation* of relations the
compiler already computed.

### 1.3 Why these four are one spec
They are four presentations of the same three relations:

| Feature | Relation it presents |
|---|---|
| Refactoring | references (find + rewrite) |
| Inheritance hierarchy | inheritance edges, both directions |
| Call graph | call edges, both directions |
| Gutter navigation | inheritance + override edges, in the gutter |

Sequenced together they share one foundation. Sequenced apart, each would grow its
own half-index.

### 1.4 Constraints
- 1.4.1 Prefer IntelliJ's own extension points over bespoke UI. A custom menu item
  is a fallback, not a starting point.
- 1.4.2 Library and stdlib code is read-only. Refactoring must refuse to rewrite it,
  and must not silently skip usages there either — it reports them.
- 1.4.3 Nothing on the typing path may block on the compiler binary.

### 1.5 Non-goals
- 1.5.1 A dynamic call graph. Runtime call information is already visible in the
  debugger's stack view at a breakpoint; a second, weaker rendering of it is not
  worth building. Decided 2026-07-12.
- 1.5.2 A whole-program static call graph rendering. See §4.0.1 — open question.
- 1.5.3 Semantic completion and inspections.

## 2. Refactoring

IntelliJ's refactoring machinery binds to `PsiNamedElement` (for the target) and
`PsiReference` (for the usages). The symbol spec supplies both — named elements at
§3, reference adapters over the compiler's export at §4 — so the built-in
refactorings become available by registering extension points rather than by
writing dialogs and text edits.

The usages a rename rewrites are the ones the *compiler* resolved, not ones the IDE
guessed at. That is what makes rename safe across overloads and inherited members.

**Requirements**

- 2.0.1 **Rename** (`RenamePsiElementProcessor`) for types, methods, fields,
  parameters, locals, and type parameters. Updates the declaration and every
  resolved reference.
- 2.0.2 Rename of a public type renames its file when the file is named for it.
- 2.0.3 **Safe delete** (`SafeDeleteProcessor`): refuses when usages remain, and
  lists them.
- 2.0.4 **Find usages** (`FindUsagesProvider`) with correct descriptive names and
  usage-type grouping (read / write / call / inherit / import).
- 2.0.5 Refactoring across the project source is in scope. Usages found in library
  or stdlib source are **reported but not rewritten** (1.4.2), and the operation
  says so plainly rather than appearing to succeed.
- 2.0.6 Every refactoring is undoable as one action.
- 2.0.7 **Refactoring refuses to run on an incomplete index.** A rename driven by a
  partial reference set silently corrupts code — it rewrites the declaration and the
  usages it knows about, leaving the rest dangling. If the xref index is stale, mid-
  refresh, or unavailable (symbol spec §7), rename and safe-delete are disabled with
  a stated reason. This is the one place where degrading gracefully is not an option:
  the operation must be correct or refused.
- 2.0.8 A custom Cajeta refactoring menu item is added **only** for operations the
  platform has no EP for. Extract Method and Change Signature are candidates for a
  later pass; neither is in this spec.

**Use cases**

- 2.1 As a developer, when I rename a class, every reference to it in my project
  updates, and its file is renamed with it.
- 2.2 As a developer, when I rename a method that overrides a parent's, I am asked
  whether to rename the whole override chain.
- 2.3 As a developer, when I rename a local, only that local's scope is touched.
- 2.4 As a developer, when I safe-delete a class that is still used, I get the usage
  list and the delete is refused.
- 2.5 As a developer, when I rename something a dependency also uses, I am told the
  library usages exist and cannot be rewritten — not left to discover a broken build.
- 2.6 As a developer, when a rename goes wrong, one undo puts it all back.
- 2.7 As a developer, when the index is not current, Rename is greyed out with a
  reason — rather than half-renaming my code.

## 3. Inheritance hierarchy

**Requirements**

- 3.0.1 Implement IntelliJ's **Type Hierarchy** EP (`HierarchyProvider`), so the
  standard hierarchy tool window and its Ctrl+H shortcut work, rather than a custom
  panel.
- 3.0.2 Three directions, as the platform expects: supertypes, subtypes, and the
  combined view.
- 3.0.3 The tree spans project, dependency, and stdlib types.
- 3.0.4 Interfaces and multiple inheritance render correctly — Cajeta has both
  (`CajetaClass` carries `superClasses` *and* `interfaces`, with vbase layout).
- 3.0.5 A **Method Hierarchy** view showing where a method is declared and which
  subtypes override it.
- 3.0.6 Nodes are navigable: selecting one opens its declaration.

**Use cases**

- 3.1 As a developer, when I press Ctrl+H on a class, I see its ancestors and
  descendants in the IDE's own hierarchy window.
- 3.2 As a developer, when I view the hierarchy of a stdlib type, my project's
  subclasses of it appear.
- 3.3 As a developer, when a class implements several interfaces, all of them show.
- 3.4 As a developer, when I ask for a method's hierarchy, I see the declaring type
  and every override.

## 4. Call graph

**Requirements**

- 4.0.1 Implement IntelliJ's **Call Hierarchy** EP (`HierarchyProvider`, call
  variant): callers-of and callees-of a method, expandable, navigable — the same
  surface Java gets. *Open question: whether a whole-program rendered graph is also
  wanted. It needs a different UI (a graph surface, not a tree) and degrades past a
  few hundred nodes. Recommendation: ship the tree, then decide.*
- 4.0.2 Callers-of and callees-of both read the compiler's resolved call edges
  (symbol spec §2.0.1, indexed at §5.0.1). The callee of a call site is whatever the
  compiler bound it to — the IDE does not perform overload resolution.
- 4.0.3 Overloads are distinguished, not merged, via the overload key carried end to
  end (symbol spec §5.0.3).
- 4.0.4 A call through a virtual method shows the declared target, and — from the
  inverted inheritance edges — the possible overriding targets, marked as possible
  rather than certain.
- 4.0.5 Calls originating in library or stdlib source appear, since that source is
  indexed.
- 4.0.6 The tree is computed lazily per expansion; opening the view does not build a
  whole-program graph.

**Use cases**

- 4.1 As a developer, when I ask who calls a method, I get its call sites and can
  walk up the chain.
- 4.2 As a developer, when I ask what a method calls, I can walk down.
- 4.3 As a developer, when a method is virtual, I can see which overrides might
  actually run, distinguished from the static target.
- 4.4 As a developer, when I ask for callers of one overload, I do not get the other
  overload's callers.
- 4.5 As a developer, when I open a call hierarchy on a large project, it appears
  immediately and expands on demand.

## 5. Gutter navigation

**Requirements**

- 5.0.1 A `RelatedItemLineMarkerProvider` (the plugin registers **no**
  `LineMarkerProvider` today) placing gutter icons on type and method declarations.
- 5.0.2 Distinct, conventional icons: *overriding* / *implementing* (points up) and
  *overridden* / *implemented* (points down) — matching what a JetBrains user
  already reads without a legend.
- 5.0.3 On a class declaration: an up-icon to its supertypes; a down-icon to its
  subtypes.
- 5.0.4 On a method declaration: an up-icon to the method it overrides; a down-icon
  to overriding methods.
- 5.0.5 Click navigates. Where there are several targets, a chooser popup lists them.
- 5.0.6 Markers are computed off the EDT (`collectSlowLineMarkers`) — the subclass
  index is queried in the background, so a file with many types does not stall the
  editor on open.
- 5.0.7 Coexists with the debugger's existing facet gutter icons
  (`FacetGutterManager`), which are session-scoped and use the markup model
  directly. The two must not fight over the gutter.

**Use cases**

- 5.1 As a developer, when I open a class that extends another, an icon in the gutter
  takes me to the parent.
- 5.2 As a developer, when a class has subclasses, an icon takes me to them; with
  several, I pick from a list.
- 5.3 As a developer, when a method overrides another, the gutter says so and jumps
  to the parent method.
- 5.4 As a developer, when I am debugging and stopped on a line, the ownership facet
  icons and the navigation icons coexist without one erasing the other.
- 5.5 As a developer, when I open a large file, gutter icons appear shortly after the
  text, and never freeze the editor.

## 6. Deliverable

Rename / safe-delete / find-usages via the platform's own refactoring EPs; Type and
Method Hierarchy; Call Hierarchy; and parent/child gutter navigation — all reading
the `ide-symbol-index` layer, all working across project, dependency, and stdlib
source.

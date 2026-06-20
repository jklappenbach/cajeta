---
name: td-project-workflow
description: Standard project layout (docs/, agents/, spec/plan files, two-level work stack) and the spec→plan→develop workflow governed by the design and implement skills
metadata:
  type: feedback
---

# Project initialization & the spec → plan → develop workflow

## Project structure
Initialize every project with this layout:
- **This memory file** at the project root.
- **`docs/`** off the root, with a **`specs/`** subdirectory.
- **`agents/`** off the root.
- **Specs** live at `docs/specs/[name]-spec.md`.
- **Plans** live at `agents/[name]-plan.md`.
- **Work stacks** live in `agents/` too (maintained by the **implement** skill):
  one **per-plan task stack** `agents/[name]-focus.md` (**shared** across clones —
  single-writer per plan), under a **per-clone** cross-plan **focus stack**
  `agents/state/[clone-id]/focus.md` (so clones sharing one `agents/` don't bleed
  attention; `clone-id` = hostname + slugified working-copy path). Completion status
  lives in the plan's checkboxes — there is **no** separate completed-log.

## What a spec is (a.k.a. SRD / SRS)
A spec focuses on the **requirements and use cases** — the **why** and the **what**.
Structure:
- A **definition section** that defines the application, feature, or capability.
- **Feature subsections** that decompose aspects of the design or featureset, each
  complete with **enumerated use cases**.

## What a plan is
A plan is the specification converted into an **actionable work request**. It includes:
- A basic description of the work.
- The APIs, SDKs, or systems to be used.
- What should be **deliverable** once the work is complete.

Then a subsection for **each unit of work** (usually corresponding to one commit),
each containing three subsections of line items:
1. **TDD** — the tests written *before* coding starts. Passing all of these tests is
   part of the implicit acceptance criteria for the section.
2. **Coding** — the actual implementation items.
3. **Acceptance** — any explicit requirements beyond tests passing.

The entire document is **numbered in outline format** so every section and line item
has a unique identifier. The plan's checkboxes (`- [ ]` / `- [x]` / `- [~]`) are the
**source of truth for what's done** — the stacks never record completion.

## Work state (the two-level stack)
While implementing, work state is a **call stack**: the frame stack
`agents/state/[clone-id]/focus.md` holds which plan/context you're in; each
`agents/[name]-focus.md` is that plan's own task LIFO. A tangent *inside* the current
plan pushes on that plan's stack; a jump to *another* plan (or free exploration) pushes a
frame on `focus.md` — so popping always returns you to where you were, within a plan
**and** across plans. The focus stack is **per-clone** (working copies sharing one
`agents/` keep separate attention); the per-plan task stacks are **shared**. The
**implement** skill governs this.

## Workflow
1. **Scoping new work** → first create a new spec and write it *with the developer*.
   Load the **design** skill — it governs the authoring of *both* the spec and
   the plan.
2. **Once the spec and plan are complete and approved** → load the **implement**
   skill. It drives the two-level work stack so the documents accurately store state,
   and state stays visible to the developer.

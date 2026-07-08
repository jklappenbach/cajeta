# docs hygiene classification — review before moves (docs-refactor 6.2.1)

Verdicts: **guide** → seeds `docs/guide/` · **specification** →
`docs/specification/<module>/` · **relocate** → non-doc content, target named ·
**retire** → superseded, successor named.

## Loose docs at docs/ top level
| path | verdict | target |
|---|---|---|
| LanguageGuide.md | guide | docs/guide/ (the canonical seed) |
| ArchiveManagement.md | specification | specification/buildtool/ |
| BuildTool.md | specification | specification/buildtool/ |
| Compilation.md | specification | specification/buildtool/ |
| CompilerModes.md | specification | specification/buildtool/ |
| Documentation.md | specification | specification/buildtool/ |
| IncrementalCompilation.md | specification | specification/buildtool/ |
| CajetaMath.md | specification | specification/cajeta-math/ |
| CajetaCaramelo.md | moved out | relocated to the cajeta-caramelo repo (docs/specification/CajetaCaramelo.md) — Caramelo is a consumer, not a foundation spec |
| Debugging.md | specification | specification/debugging/ (new) |
| DI-override-hook.md | specification | specification/lang/ |
| Embedded.md | specification | specification/embedded/ (new) |
| HarnessDesign.md | specification | specification/concurrent/ (see questions) |
| LintRules.md | specification | specification/lang/ |
| OperatorOverloading.md | specification | specification/lang/ (no existing counterpart — clean move) |
| TemplateWildcard.md | specification | specification/lang/templates/ |
| olla-ci-publish.md | specification | specification/buildtool/ (CI runbook; windows-ci/ precedent) |
| CajetaTorch.md | retire | superseded by specification/nucleo/torch-facade-spec.md (see questions) |
| CaptureConversion.md | retire | superseded by specification/lang/templates/reified-capture-spec.md (see questions) |
| Net.md | retire | self-declared tombstone; specification/io/net/Networking.md |
| SkillDiscovery.md | retire | superseded by skill-discovery spec (archived); concise framing may seed a guide chapter (see questions) |

## Stray directories
| path | verdict | target |
|---|---|---|
| history/ImplementationStatus.md | retire | status tracker, marked complete |
| history/StructsViewsStatus.md | retire | status tracker, complete (832/832) |
| gpu/CajetaGPU.md + gpu/*.md (10 capability docs) | specification | specification/gpu/ (new module) |
| gpu/xpu/*.md | specification | specification/xpu/ (new module) |
| gpu/gfx/CajetaGFX.md, CajetaRender.md | retire | cajeta-gfx-spec.md explicitly supersedes ("both drifted") |
| cajeta/gpu/ifx/ (README + .cajeta code) | relocate | samples/ per plan — README says target is runtime/src/cajeta/ifx/ (see questions) |
| buildtool/LibraryProjectType.md | specification | specification/buildtool/ (already slated) |

## Open questions
1. CajetaTorch.md: diff against torch-facade-spec for unique API-mapping content before retiring.
2. CaptureConversion.md: confirm reified-capture-spec subsumes the capture#N staging model.
3. SkillDiscovery.md: retire outright, or absorb its three-operations framing into a guide chapter?
4. cajeta/gpu/ifx/: teaching sample (samples/) or the stdlib facade awaiting wire-in (runtime/src/cajeta/ifx/)? runtime/src/cajeta/ifx/ already exists and shipped — likely stale duplicate; verify then retire or samples/.
5. HarnessDesign.md: concurrent/ or a dedicated specification/harness/ module?
6. Retired files: delete outright (git history preserves) or move to docs/attic/?

# --lto=thin fails to link (defect spec)

Status: draft, 2026-10-19. Found by tensor-element-access's measurement (7.5).

## 1. Definition

`cajeta --emit=exe --lto=thin` fails at the link on v0.43.1 (proton, Linux):

```
ld.lld: error: Invalid summary version 13. Version should be in the range [1-12].
cajeta: link failed — 'cc' exited 1
```

The compiler writes a ThinLTO summary in a newer format than the `ld.lld`
that links it reads. `--lto=full` links and runs. The help text says thin
"uses the fork lld", so the link is likely reaching a system lld instead.

## 2. Use cases

- **2.1** When an executable is built with `--lto=thin`, it links with the
  lld that matches the compiler's LLVM, and runs.
- **2.2** When no matching lld is available, the build says which lld it
  found and which it needs, before the link.

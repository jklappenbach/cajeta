# Classpath-failure diagnostic duplication — spec (defect, cosmetic)

Found by the tour-quality http review (findings/http.md, Defects).

## 1. Definition

- 1.1 On a `--classpath` archive read failure the driver prints the error
  twice, the second time with an empty subject:
  `cajeta: : CajetaArchive: cannot open: ...`.

## 2. Use cases

- 2.1 As a developer, when an archive on `--classpath` cannot be opened, then
  exactly one diagnostic is emitted, with the archive path as subject.

## 3. Acceptance

- 3.1 Test pinning single emission + non-empty subject for a missing and a
  corrupt archive.

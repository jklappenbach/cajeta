# Vendored conformance fixture

Copied verbatim from **cajeta-coco** `fixtures/conformance/`, which is the source
of truth — coco owns the format, so it owns the fixture.

This is a copy rather than a reference for one practical reason: Gradle cannot
resolve a path in a sibling repository that may not be checked out, least of all
on CI. `CocoReaderTest.fixtureMatchesCocoSourceOfTruth` closes the drift that
copying invites: when `COCO_REPO` points at a coco checkout it asserts these
files are byte-identical to the originals, and skips when it does not.

`EXPECTED.md` in coco states what a correct reader derives from these files and
which mutations it must reject. The assertions in `CocoReaderTest` are that
document, executed.

Do not edit these files here. Regenerate in coco and re-copy.

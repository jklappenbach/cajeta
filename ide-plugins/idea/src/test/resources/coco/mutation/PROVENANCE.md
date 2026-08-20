# `mutation.tsv` — from a real `coco mutate` run

Not hand-made. Produced by coco's mutation driver over a purpose-built project
(`probe.Guard`) whose `main` deliberately under-asserts one boundary and
properly asserts another.

    coco run    --src <p>/src --entry probe.Guard.main --out <p>/out
    coco mutate --src <p>/src --entry probe.Guard.main --out <p>/out

    coco: mutation score: 3/6 killed (0 skipped as uncovered)

## Why this file is the right fixture

Line 13 is `if (n >= limit)`, and the coverage run reports it at **2 line hits
with BOTH branch arms taken** — 100% line and 100% branch coverage. The
`sge->sgt` mutant on it still **SURVIVED**.

That is the entire argument for the Mutants tab in one row: a line that every
coverage metric calls fully tested, whose behaviour nobody actually pinned. The
caller never passes `n == limit`, so `>=` and `>` are indistinguishable to the
suite.

`isPositive` is the control. Its caller probes 1, 0 and -1, so `sgt->sge` is
killed — same shape of comparison, properly asserted.

## Regenerating

Rebuild `.build/coco-engine` first; a stale engine captures the behaviour of old
code. Then re-run the two commands above and copy `<out>/mutation.tsv`.

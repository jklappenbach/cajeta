# Notebook tour

A runnable tour of the Cajeta Jupyter kernel: the accumulating session,
installing a library mid-session, what verification refuses, and what a
failure costs you.

Everything runs **offline**. `setup.sh` stages a local filesystem
repository with four small libraries, generates two ed25519 keypairs, and
signs with them — one key the tour trusts, one it deliberately does not.

## Run it

```
./setup.sh
CAJETA_TRUST_KEYS_DIR="$PWD/trust" jupyter lab notebooks/tour.ipynb
```

If you have not installed the kernelspec yet:

```
cajeta init --kernel
```

Start Jupyter from **this directory** so the kernel adopts this project —
that is what makes `cajeta.json`'s repository the one `Packages.install`
resolves against.

`CAJETA_TRUST_KEYS_DIR` points the trust store at the keypair `setup.sh`
generated. Without it the signed installs are refused, which is correct
behaviour and something the tour asks you to try.

## What the tour covers

| Part | |
|---|---|
| 1 | The session accumulates — bindings and classes persist across cells |
| 2 | `Packages.install`, the phase narration, and why the import goes in the next cell |
| 3 | Version conflict, unknown library, the same-cell rule — and that the session survives each |
| 4 | Checksums, signatures, and a valid signature from an untrusted key |
| 5 | `installAndSave` writing `cajeta.json` with your comments intact |
| 6 | An install refusing to shadow a class your cell declared |

**Six cells are meant to fail.** Each is labelled. They are the tour: after
every one, the kernel keeps serving and the session keeps its bindings.

## Things to try

- Add `"require-signatures": true` to `cajeta.json` and re-run Part 3's
  `plain` install — the unsigned archive is now refused.
- Restart without `CAJETA_TRUST_KEYS_DIR` and re-run Part 2 — a signed
  archive with no trusted keys is refused rather than waved through.
- Restart after Part 5 — `import demo.Stats` works in the *first* cell,
  with no install call, because the dependency is in the manifest now.

## Layout

```
cajeta.json          the governing project — repositories, dependencies
setup.sh             stages repo/, trust/ and the signing keys
notebooks/tour.ipynb the tour
repo/                generated: the filesystem repository
trust/               generated: the public key the kernel trusts
.keys/  .work/       generated: private keys and build scratch
```

`./setup.sh --clean` removes everything generated. Part 5 edits
`cajeta.json`; `git checkout cajeta.json` puts it back.

## It is tested

`NotebookTourSampleTests` runs this notebook cell by cell against a real
session, reading the cell sources out of the `.ipynb` itself. The prose
above and the notebook's own claims are assertions, not decoration — if a
cell labelled expected-to-fail ever starts passing, that test fails too.

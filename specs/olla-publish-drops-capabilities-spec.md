# olla-publish-drops-capabilities — declared capabilities do not survive to the registry

## 1. Definition

Found 2026-08-08 while publishing `dev.cajeta.gossip`. A library's
manifest declares its capabilities; the registry reports an empty list
for every one of them.

```
cajeta-gossip/cajeta.json   "capabilities": ["network"]
GET /v2/resolve?name=dev.cajeta.gossip&version=0.1.1
    -> {"capabilities":[], ...}

cajeta-docs/cajeta.json     "capabilities": ["filesystem"]
GET /v2/resolve?name=dev.cajeta.docs&version=0.1.0
    -> {"capabilities":[], ...}
```

Two independent libraries, two different capabilities, both flattened —
so this is the publish or resolve path, not a one-off bad upload. The
archives themselves are fine; it is the metadata that arrives empty.

## 2. Why it matters more than it looks

An empty capability list is not a neutral default — it is a POSITIVE
CLAIM that the library touches nothing. A consumer resolving
`dev.cajeta.docs` is told it needs no filesystem access, when reading
files is most of what it does. The failure mode is a capability check
that passes when it should have failed, which is the direction that
does harm.

It also silently weakens `cajeta-cloud`'s §14.11 guarantee. That spec
turns on the interface library declaring `capabilities: []` and MEANING
it — but if every published library reports `[]` regardless, the
distinction between "declares nothing" and "declared something that was
dropped" is invisible at the registry, and the guarantee cannot be
checked by a consumer.

## 3. Requirements

- **3.1** `/v2/resolve` reports exactly the capability list the
  published manifest declared.
- **3.2** A library declaring no capabilities and one whose
  capabilities were lost are DISTINGUISHABLE — if the field cannot be
  populated it must be absent or null, never an empty list that reads
  as a promise.
- **3.3** A pin publishes a library with a non-empty capability list
  and asserts the resolve response carries it.

## 4. Scope note

Not a compiler defect: the publish pipeline / registry owns this
(alongside `buildtool-*`). Nothing in the language or stdlib is
implicated — the manifests are correct as written.

## 5. Reproduction

The two `curl` calls in §1 against the current registry, compared with
each repo's committed `cajeta.json`.

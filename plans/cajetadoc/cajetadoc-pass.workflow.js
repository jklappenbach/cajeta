export const meta = {
  name: 'cajetadoc-runtime-pass',
  description: 'Add/improve cajetadoc + markdown comments across runtime stdlib classes (non-xpu)',
  phases: [
    { title: 'Document', detail: 'one agent per source file — add cajetadoc + markdown comments' },
    { title: 'Verify', detail: 'grade docs, confirm only comments changed' },
  ],
}

// File list arrives via `args` (JSON array of repo-relative paths, or a JSON
// string of one). The script cannot run find, so the launcher passes it in.
let FILES = []
if (Array.isArray(args)) {
  FILES = args
} else if (typeof args === 'string') {
  try {
    const parsed = JSON.parse(args)
    if (Array.isArray(parsed)) FILES = parsed
    else if (typeof parsed === 'string') FILES = [parsed]
  } catch (e) {
    FILES = args.split(/\s+/).filter(Boolean)
  }
} else if (args && Array.isArray(args.files)) {
  FILES = args.files
}
if (!FILES.length) {
  log('No files passed in args — nothing to do.')
  return { error: 'empty file list', argsType: typeof args }
}
log(`Documenting ${FILES.length} cajeta source files.`)

const CONVENTIONS = `
Cajeta doc-comment conventions — MATCH THE EXISTING HOUSE STYLE (see cajeta.time/* and
cajeta.lang.Optional for the gold standard):
- Doc comments are JavaDoc-style \`/** ... */\` blocks placed IMMEDIATELY above the
  class / interface / enum / method they document.
- cajetadoc renders the body as Markdown. Use it well but sparingly:
    * \`##\` subheadings only when a comment has real sections.
    * \`inline code\` for identifiers, types, literals.
    * Fenced code blocks tagged \`\`\`cajeta for usage examples.
    * Wiki-links to sibling stdlib types: [[LocalDate]], [[Comparable]], [[ZonedDateTime]].
      Use a wiki-link EVERY time you name another stdlib class so cajetadoc cross-references it.
- Cajeta idioms to use in examples (infer the real ones from THIS file):
    * Construction is usually via static factories: \`Type.ofX(...)\`, \`Type.from(...)\`, \`Type.parse(...)\`, etc.
    * Storage classes: \`stack\` (value, copy semantics), \`heap\`/\`#\` (heap, ownership transfer).
`

const RULES = `
What to document (user's rules — follow exactly):
1. CLASS / INTERFACE / ENUM level: a \`/** */\` block that says what the type IS and HOW it's
   used. Where it makes sense, include a short \`\`\`cajeta usage example. Show how it interacts
   with related stdlib types and [[wiki-link]] them. A developer who has never seen this class
   should come away knowing how to use it.
2. PROCESS-STARTERS get a usage snippet: any method a caller invokes to START something —
   constructors, static factories (of*/from*/parse/open/create/new*/build*), and "do the thing"
   entry points (e.g. run/start/encode/format/resolve) — get a \`/** */\` containing a concise
   \`\`\`cajeta example that uses the REAL signature from this file.
3. GETTERS / SETTERS: NO comment. Skip trivial field accessors entirely (a one-line return/assign
   with no logic). Do not add noise to them.
4. ENUMS and named CONSTANTS: at least one sentence on purpose. Give each enum constant a short
   note when its meaning isn't obvious from the name. Do not annotate self-explanatory constants
   redundantly, but the enum/constant-group as a whole must have a purpose sentence.
5. OTHER methods (not getters/setters, not process-starters): one concise \`/** */\` line on what
   it does and any non-obvious behavior (throws, wrapping, clamping, etc.).

Style: concise, user-friendly, comprehensive. DEMONSTRATE THROUGH CODE rather than prose wherever
you can. Keep existing GOOD comments — only add where missing or tighten where genuinely weak.
Do not churn already-well-documented members.

HARD CONSTRAINTS:
- COMMENTS ONLY. Never change executable code, signatures, field declarations, imports, the
  package line, or the whitespace/content of any code line. If you would have to touch code, don't.
- Every example must reference methods/members that ACTUALLY EXIST in this file. Read the file
  first; do not invent API. Examples are illustrative (not compiled) but must be idiomatic and
  use real names.
`

const DOC_SCHEMA = {
  type: 'object',
  additionalProperties: false,
  required: ['file', 'changed', 'classDocUpdated', 'methodsDocumented',
             'enumsOrConstantsDocumented', 'usageExamplesAdded', 'crossLinksAdded',
             'gettersSettersSkipped', 'notes'],
  properties: {
    file: { type: 'string' },
    changed: { type: 'boolean', description: 'true if any edit was made' },
    classDocUpdated: { type: 'boolean' },
    methodsDocumented: { type: 'integer' },
    enumsOrConstantsDocumented: { type: 'integer' },
    usageExamplesAdded: { type: 'integer' },
    crossLinksAdded: { type: 'integer' },
    gettersSettersSkipped: { type: 'integer' },
    notes: { type: 'string', description: 'one or two sentences on what was done / anything notable' },
  },
}

const VERIFY_SCHEMA = {
  type: 'object',
  additionalProperties: false,
  required: ['file', 'ok', 'grade', 'issues', 'summary'],
  properties: {
    file: { type: 'string' },
    ok: { type: 'boolean', description: 'true if ONLY comments changed and docs meet the rules' },
    grade: { type: 'string', enum: ['A', 'B', 'C', 'needs-work'] },
    issues: { type: 'array', items: { type: 'string' },
              description: 'concrete problems: code changed, fabricated API in an example, missing class doc, undocumented enum, getter over-annotated, etc.' },
    summary: { type: 'string' },
  },
}

const results = await pipeline(
  FILES,
  // Stage 1 — document the file.
  (file) => agent(
    `You are improving ONLY the documentation comments in one Cajeta stdlib source file:\n` +
    `  ${file}\n\n` +
    `Read the whole file, then apply the rules below with Edit. Work in place.\n` +
    CONVENTIONS + `\n` + RULES + `\n` +
    `When done, report what you did.`,
    { label: `doc:${file.replace('runtime/src/cajeta/', '')}`, phase: 'Document', schema: DOC_SCHEMA }
  ),
  // Stage 2 — grade and confirm comments-only.
  (doc, file) => agent(
    `Review the documentation quality of this Cajeta stdlib file AFTER a doc pass:\n` +
    `  ${file}\n\n` +
    `Read the file. Judge it against these rules:\n` + RULES + `\n` + CONVENTIONS + `\n` +
    `Check specifically:\n` +
    `- Did the pass touch ONLY comments? (Code, signatures, imports must be untouched. Flag any code change.)\n` +
    `- Do all \`\`\`cajeta examples reference REAL members of this file (no fabricated API)?\n` +
    `- Class/interface/enum has a useful doc with an example where it makes sense?\n` +
    `- Constructors / factories / process-starters have usage snippets?\n` +
    `- Enums and constants have a purpose sentence? Getters/setters left clean (not over-annotated)?\n` +
    `- Cross-referenced sibling types use [[wiki-links]]?\n` +
    `Grade A (excellent) / B (good) / C (thin but acceptable) / needs-work (rule violation or fabricated API).`,
    { label: `verify:${file.replace('runtime/src/cajeta/', '')}`, phase: 'Verify', schema: VERIFY_SCHEMA, model: 'sonnet' }
  ).then(verify => ({ doc, verify }))
)

const rows = results.filter(Boolean)
const changed = rows.filter(r => r.verify?.ok !== false)
const flagged = rows.filter(r => r.verify && (r.verify.ok === false || r.verify.grade === 'needs-work'))

return {
  totalFiles: FILES.length,
  processed: rows.length,
  documented: rows.filter(r => r.doc?.changed).length,
  flagged: flagged.map(r => ({ file: r.doc?.file || r.verify?.file, grade: r.verify?.grade, issues: r.verify?.issues })),
  grades: rows.map(r => ({ file: r.verify?.file, grade: r.verify?.grade })),
}

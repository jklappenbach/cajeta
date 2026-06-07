export const meta = {
  name: 'cajetadoc-fix-flagged',
  description: 'Fix flagged cajetadoc issues (fabricated API, getter over-annotation, missing wiki-links) on the files the verify pass marked',
  phases: [
    { title: 'Fix', detail: 'one agent per flagged file — address its specific issues' },
    { title: 'Recheck', detail: 're-grade, confirm fabricated API and rule violations gone' },
  ],
}

// args = [{ file, grade, issues:[...] }, ...] from the first pass's verify stage.
let ITEMS = []
if (Array.isArray(args)) ITEMS = args
else if (typeof args === 'string') { try { ITEMS = JSON.parse(args) } catch (e) { ITEMS = [] } }
if (!ITEMS.length) { log('No flagged items in args.'); return { error: 'empty' } }
log(`Fixing ${ITEMS.length} flagged files.`)

const HOUSE = `
House style (match cajeta.time/* and cajeta.lang.Optional):
- Doc comments are \`/** ... */\` blocks directly above the class/interface/enum/method.
- Body is Markdown: \`##\` subheads, \`inline code\`, fenced \`\`\`cajeta examples, and
  [[WikiLinks]] to sibling stdlib types EVERY time another stdlib class is named (replace
  bare backtick type names and any \`@See FQN\` tags with [[Type]] inline links).
- COMMENTS ONLY. Never change executable code, signatures, fields, imports, or the package line.
`

const CORRECTNESS = `
CRITICAL — no fabricated API:
- Every identifier used in a \`\`\`cajeta example (method names, factory names, enum constants,
  storage class, argument arity, return type) MUST exist with that exact signature. Before you
  write or keep any example, OPEN the file under doc AND the sibling files it references and
  CONFIRM each call. Use Grep/Read across runtime/src to verify. If an API does not exist, do not
  reference it — rewrite the example to use real API, or drop the example.
- Match real signatures exactly: storage class (\`stack\` vs \`#\` vs \`heap\`), enum constant casing
  (e.g. OpenMode.READ not OpenMode.Read), argument count, and the actual return type (e.g. an
  Optional<...> return must be used with isPresent()/get(), not as a raw value).
- Rule 3: trivial one-line getters/setters (a bare return/assign with no logic) get NO doc comment.
  Remove any doc block the previous pass added to such accessors.
`

const FIX_SCHEMA = {
  type: 'object', additionalProperties: false,
  required: ['file', 'fixed', 'remaining', 'notes'],
  properties: {
    file: { type: 'string' },
    fixed: { type: 'array', items: { type: 'string' }, description: 'each issue addressed, briefly' },
    remaining: { type: 'array', items: { type: 'string' }, description: 'issues deliberately not addressed and why' },
    notes: { type: 'string' },
  },
}

const RECHECK_SCHEMA = {
  type: 'object', additionalProperties: false,
  required: ['file', 'ok', 'grade', 'issues'],
  properties: {
    file: { type: 'string' },
    ok: { type: 'boolean', description: 'true if no fabricated API, no getter over-annotation, and only comments changed' },
    grade: { type: 'string', enum: ['A', 'B', 'C', 'needs-work'] },
    issues: { type: 'array', items: { type: 'string' } },
  },
}

const results = await pipeline(
  ITEMS,
  (it) => agent(
    `Fix the documentation issues in this Cajeta stdlib file:\n  ${it.file}\n\n` +
    `The verification pass flagged these specific issues — address EACH one:\n` +
    it.issues.map((s, i) => `  ${i + 1}. ${s}`).join('\n') + `\n\n` +
    HOUSE + CORRECTNESS + `\n` +
    `Read the file, verify real API in this file and siblings under runtime/src/cajeta/, then Edit ` +
    `to resolve every issue. Comments only. Report what you fixed.`,
    { label: `fix:${it.file.replace('runtime/src/cajeta/', '')}`, phase: 'Fix', schema: FIX_SCHEMA }
  ),
  (fix, it) => agent(
    `Re-grade the documentation of this file after a fix pass:\n  ${it.file}\n\n` +
    `Original issues were:\n` + it.issues.map((s, i) => `  ${i + 1}. ${s}`).join('\n') + `\n\n` +
    `Read the file (and any sibling it references) and check:\n` +
    `- Are ALL \`\`\`cajeta example calls backed by REAL API with correct signatures/casing/arity/storage class? (Grep to confirm.)\n` +
    `- Were trivial getters/setters left WITHOUT doc comments (Rule 3)?\n` +
    `- Sibling stdlib types use [[wiki-links]]?\n` +
    `- Only comments changed (no code touched)?\n` +
    HOUSE +
    `Set ok=false and list any issue that REMAINS. Grade A/B/C/needs-work.`,
    { label: `recheck:${it.file.replace('runtime/src/cajeta/', '')}`, phase: 'Recheck', schema: RECHECK_SCHEMA, model: 'sonnet' }
  ).then(recheck => ({ fix, recheck }))
)

const rows = results.filter(Boolean)
const stillBad = rows.filter(r => r.recheck && (r.recheck.ok === false || r.recheck.grade === 'needs-work'))
return {
  flaggedInput: ITEMS.length,
  processed: rows.length,
  cleared: rows.length - stillBad.length,
  stillBad: stillBad.map(r => ({ file: r.recheck?.file, grade: r.recheck?.grade, issues: r.recheck?.issues })),
  grades: rows.map(r => ({ file: r.recheck?.file, grade: r.recheck?.grade })),
}

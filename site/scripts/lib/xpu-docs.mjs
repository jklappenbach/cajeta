// The XPU tab's pages and a markdown link reader, shared by the XPU tests.
export const XPU_PAGES = [
  'README.md',
  'walkthrough.md',
  'scheduler.md',
  'optimization.md',
  'math/README.md',
  'nucleo/README.md',
  'ml/README.md',
];

/** Markdown link targets, `[text](target)`, without titles. */
export function linkTargets(md) {
  return [...md.matchAll(/\]\(\s*<?([^)\s>]+)>?(?:\s+"[^"]*")?\s*\)/g)].map((m) => m[1]);
}

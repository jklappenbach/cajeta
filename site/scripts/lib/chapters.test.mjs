import { test } from 'node:test';
import assert from 'node:assert/strict';
import { splitChapters } from '../../src/lib/chapters.mjs';

const BASE = '/guides/sample/';

function html() {
  return [
    '<h1 id="the-guide">The Guide</h1>',
    '<p class="lede">Welcome to the guide, read it in order.</p>',
    '<h2 id="1-first-steps">1. First steps</h2>',
    '<p>Start here. See <a href="#3-advanced">advanced</a> later.</p>',
    '<h3 id="setup">Setup</h3>',
    '<p>Jump to <a href="#setup">setup</a> anytime.</p>',
    '<h2 id="2-middle-bits">2. Middle bits</h2>',
    '<p>Middle.</p>',
    '<h2 id="not-a-chapter">Aside</h2>',
    '<p>Still chapter two territory.</p>',
    '<h2 id="3-advanced">3. Advanced</h2>',
    '<p>Back to <a href="#1-first-steps">first</a>.</p>',
  ].join('\n');
}

const headings = [
  { depth: 1, slug: 'the-guide', text: 'The Guide' },
  { depth: 2, slug: '1-first-steps', text: '1. First steps' },
  { depth: 3, slug: 'setup', text: 'Setup' },
  { depth: 2, slug: '2-middle-bits', text: '2. Middle bits' },
  { depth: 2, slug: 'not-a-chapter', text: 'Aside' },
  { depth: 2, slug: '3-advanced', text: '3. Advanced' },
];

// 1.1.1 basic split
test('splitChapters yields intro plus one chunk per numbered h2', () => {
  const ch = splitChapters(html(), headings, BASE);
  assert.equal(ch.length, 4);
  assert.equal(ch[0].slug, '');
  assert.equal(ch[0].title, 'Introduction');
  assert.ok(ch[0].html.includes('Welcome to the guide'));
  assert.deepEqual(
    ch.slice(1).map((c) => c.slug),
    ['1-first-steps', '2-middle-bits', '3-advanced']
  );
  assert.equal(ch[1].title, '1. First steps');
  assert.equal(ch[1].url, '/guides/sample/1-first-steps/');
  assert.equal(ch[0].url, '/guides/sample/');
  assert.ok(ch[1].html.startsWith('<h2 id="1-first-steps"'));
  assert.ok(ch[1].html.includes('Start here'));
  assert.ok(!ch[1].html.includes('Middle.'));
});

// 1.1.2 threshold
test('splitChapters returns null under 3 numbered chapters', () => {
  const two = '<h2 id="1-a">1. A</h2><p>x</p><h2 id="2-b">2. B</h2><p>y</p>';
  const hs = [
    { depth: 2, slug: '1-a', text: '1. A' },
    { depth: 2, slug: '2-b', text: '2. B' },
  ];
  assert.equal(splitChapters(two, hs, BASE), null);
});

// 1.1.3 unnumbered h2 stays in the running chapter
test('non-numbered h2 does not start a new chapter', () => {
  const ch = splitChapters(html(), headings, BASE);
  const middle = ch.find((c) => c.slug === '2-middle-bits');
  assert.ok(middle.html.includes('Aside'));
  assert.ok(middle.html.includes('Still chapter two territory'));
});

// 1.1.4 anchor rewriting across chapters
test('cross-chapter anchors are rewritten, same-chunk anchors stay bare', () => {
  const ch = splitChapters(html(), headings, BASE);
  const first = ch.find((c) => c.slug === '1-first-steps');
  const last = ch.find((c) => c.slug === '3-advanced');
  // link from ch1 to ch3 gets the chapter URL
  assert.ok(first.html.includes('href="/guides/sample/3-advanced/#3-advanced"'));
  // link to an h3 in the same chunk stays a bare anchor
  assert.ok(first.html.includes('href="#setup"'));
  // link from ch3 back to ch1
  assert.ok(last.html.includes('href="/guides/sample/1-first-steps/#1-first-steps"'));
});

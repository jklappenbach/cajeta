// Editorial touches applied to every rendered doc:
//  - the first real prose paragraph gets class "lede" (drop capital),
//  - tables get wrapped in an overflow-x scroll container, both markdown
//    tables and raw HTML ones (a generated catalog writes its tables as HTML;
//    rehype-raw parses those only after this plugin, so here they are still a
//    `raw` node whose text opens with <table).

function textOf(node) {
  if (node.type === 'text') return node.value;
  return (node.children ?? []).map(textOf).join('');
}

export default function rehypeEditorial() {
  return (tree) => {
    let ledeDone = false;
    const children = tree.children ?? [];
    for (let i = 0; i < children.length; i++) {
      const node = children[i];
      if (node.type === 'raw' && /^\s*<table[\s>]/i.test(node.value)) {
        children[i] = {
          type: 'element',
          tagName: 'div',
          properties: { className: ['table-scroll'] },
          children: [node],
        };
        continue;
      }
      if (node.type !== 'element') continue;

      if (!ledeDone && node.tagName === 'p' && textOf(node).trim().length >= 60) {
        node.properties ??= {};
        node.properties.className = [
          ...(Array.isArray(node.properties.className) ? node.properties.className : []),
          'lede',
        ];
        ledeDone = true;
      }

      if (node.tagName === 'table') {
        children[i] = {
          type: 'element',
          tagName: 'div',
          properties: { className: ['table-scroll'] },
          children: [node],
        };
      }
    }
  };
}

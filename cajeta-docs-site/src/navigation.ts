import { getPermalink } from './utils/permalinks';

// Doc links use the lowercase, path-based slugs the `docs` content collection
// produces (e.g. docs/specification/lang/MemoryModel.md -> /docs/specification/memorymodel).

export const headerData = {
  links: [
    {
      text: 'Docs',
      href: getPermalink('/docs'),
      links: [
        { text: 'All documents', href: getPermalink('/docs') },
        { text: 'Core language & toolchain', href: getPermalink('/docs#core') },
        { text: 'Standard library', href: getPermalink('/docs#stdlib') },
        { text: 'GPU / compute', href: getPermalink('/docs#gpu') },
        { text: 'Tooling specs', href: getPermalink('/docs#specs') },
        { text: 'Implementation status', href: getPermalink('/docs#status') },
      ],
    },
    {
      text: 'Language',
      links: [
        { text: 'Language Guide', href: getPermalink('/docs/languageguide') },
        { text: 'Memory Model', href: getPermalink('/docs/specification/memorymodel') },
        { text: 'Ownership Transfer', href: getPermalink('/docs/specification/ownershiptransfer') },
        { text: 'Error Model', href: getPermalink('/docs/specification/errormodel') },
        { text: 'Concurrency', href: getPermalink('/docs/specification/concurrency') },
        { text: 'Views', href: getPermalink('/docs/specification/views') },
        { text: 'Lambdas', href: getPermalink('/docs/specification/lambdas') },
        { text: 'Templates & Wildcards', href: getPermalink('/docs/templatewildcard') },
        { text: 'Operator Overloading', href: getPermalink('/docs/operatoroverloading') },
      ],
    },
    {
      text: 'Stdlib',
      links: [
        { text: 'Lang', href: getPermalink('/docs/specification/lang') },
        { text: 'Collections', href: getPermalink('/docs/specification/collections') },
        { text: 'Streams', href: getPermalink('/docs/specification/streams') },
        { text: 'String', href: getPermalink('/docs/specification/lang/string') },
        { text: 'Annotations', href: getPermalink('/docs/specification/annotations') },
        { text: 'Reflection', href: getPermalink('/docs/specification/reflection') },
        { text: 'Networking', href: getPermalink('/docs/net') },
      ],
    },
    {
      text: 'GPU',
      links: [
        { text: 'GPU overview', href: getPermalink('/docs/gpu/cajetagpu') },
        { text: 'XPU kernels', href: getPermalink('/docs/gpu/xpu/cajetaxpu') },
        { text: 'XPU capability matrix', href: getPermalink('/docs/gpu/xpu/cajetaxpu-matrix') },
      ],
    },
    {
      text: 'Tooling',
      links: [
        { text: 'Build Tool', href: getPermalink('/docs/buildtool') },
        { text: 'Compilation', href: getPermalink('/docs/compilation') },
        { text: 'Compiler Modes', href: getPermalink('/docs/compilermodes') },
        { text: 'Debugging', href: getPermalink('/docs/debugging') },
      ],
    },
  ],
  actions: [{ text: 'GitHub', href: 'https://github.com/jklappenbach/cajeta', target: '_blank' }],
};

export const footerData = {
  links: [
    {
      title: 'Language',
      links: [
        { text: 'Language Guide', href: getPermalink('/docs/languageguide') },
        { text: 'Memory Model', href: getPermalink('/docs/specification/memorymodel') },
        { text: 'Ownership Transfer', href: getPermalink('/docs/specification/ownershiptransfer') },
        { text: 'Error Model', href: getPermalink('/docs/specification/errormodel') },
        { text: 'Concurrency', href: getPermalink('/docs/specification/concurrency') },
        { text: 'Views', href: getPermalink('/docs/specification/views') },
        { text: 'Lambdas', href: getPermalink('/docs/specification/lambdas') },
      ],
    },
    {
      title: 'Standard library',
      links: [
        { text: 'Lang', href: getPermalink('/docs/specification/lang') },
        { text: 'Collections', href: getPermalink('/docs/specification/collections') },
        { text: 'Streams', href: getPermalink('/docs/specification/streams') },
        { text: 'Annotations', href: getPermalink('/docs/specification/annotations') },
        { text: 'Reflection', href: getPermalink('/docs/specification/reflection') },
        { text: 'Networking', href: getPermalink('/docs/net') },
        { text: 'JSON codec', href: getPermalink('/docs/specification/codec/json') },
      ],
    },
    {
      title: 'GPU / compute',
      links: [
        { text: 'GPU overview', href: getPermalink('/docs/gpu/cajetagpu') },
        { text: 'XPU kernels', href: getPermalink('/docs/gpu/xpu/cajetaxpu') },
        { text: 'XPU capability matrix', href: getPermalink('/docs/gpu/xpu/cajetaxpu-matrix') },
        { text: 'Ray query', href: getPermalink('/docs/gpu/rayquery') },
      ],
    },
    {
      title: 'Tooling & process',
      links: [
        { text: 'Build Tool', href: getPermalink('/docs/buildtool') },
        { text: 'Compilation', href: getPermalink('/docs/compilation') },
        { text: 'Debugging', href: getPermalink('/docs/debugging') },
        { text: 'Lint Rules', href: getPermalink('/docs/lintrules') },
        { text: 'Implementation Status', href: getPermalink('/docs/history/implementationstatus') },
      ],
    },
  ],
  secondaryLinks: [],
  socialLinks: [
    { ariaLabel: 'Github', icon: 'tabler:brand-github', href: 'https://github.com/jklappenbach/cajeta' },
  ],
  footNote: `
    Cajeta · documentation rendered from the repository's <code>docs/</code> tree with <a class="text-blue-600 underline dark:text-muted" href="https://github.com/arthelokyo/astrowind">AstroWind</a>.
  `,
};

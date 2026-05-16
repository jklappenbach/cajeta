import { getPermalink, getAsset } from './utils/permalinks';

export const headerData = {
  links: [
    {
      text: 'Home',
      href: getPermalink('/'),
    },
    {
      text: 'Docs',
      href: getPermalink('/docs'),
      links: [
        { text: 'All documents', href: getPermalink('/docs') },
        { text: 'Language', href: getPermalink('/docs#language') },
        { text: 'Standard library', href: getPermalink('/docs#stdlib') },
        { text: 'Tooling', href: getPermalink('/docs#tooling') },
        { text: 'Process', href: getPermalink('/docs#process') },
        { text: 'Implementation status', href: getPermalink('/docs#status') },
      ],
    },
    {
      text: 'Language',
      links: [
        { text: 'Memory Model', href: getPermalink('/docs/MemoryModel') },
        { text: 'Error Model', href: getPermalink('/docs/ErrorModel') },
        { text: 'Threading Model', href: getPermalink('/docs/ThreadModel') },
        { text: 'Structs', href: getPermalink('/docs/Structs') },
        { text: 'Views', href: getPermalink('/docs/Views') },
        { text: 'Lambdas', href: getPermalink('/docs/Lambdas') },
        { text: 'Aspect Model', href: getPermalink('/docs/AspectModel') },
        { text: 'Floating Point Model', href: getPermalink('/docs/FLoatingPointModel') },
      ],
    },
    {
      text: 'Stdlib',
      links: [
        { text: 'Standard Library', href: getPermalink('/docs/StandardLibrary') },
        { text: 'cajeta.io.net.http', href: getPermalink('/docs/CajetaHttp') },
        { text: 'cajeta.ml', href: getPermalink('/docs/CajetaML') },
        { text: 'cajeta.prism', href: getPermalink('/docs/CajetaPrism') },
        { text: 'cajeta.reflect', href: getPermalink('/docs/CajetaReflect') },
        { text: 'cajeta.torch', href: getPermalink('/docs/CajetaTorch') },
      ],
    },
    {
      text: 'Tooling',
      links: [
        { text: 'Build Tool', href: getPermalink('/docs/BuildTool') },
        { text: 'Compilation', href: getPermalink('/docs/Compilation') },
        { text: 'Debugging', href: getPermalink('/docs/Debugging') },
        { text: 'Documentation', href: getPermalink('/docs/Documentation') },
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
        { text: 'Memory Model', href: getPermalink('/docs/MemoryModel') },
        { text: 'Error Model', href: getPermalink('/docs/ErrorModel') },
        { text: 'Threading Model', href: getPermalink('/docs/ThreadModel') },
        { text: 'Structs', href: getPermalink('/docs/Structs') },
        { text: 'Views', href: getPermalink('/docs/Views') },
        { text: 'Lambdas', href: getPermalink('/docs/Lambdas') },
        { text: 'Aspect Model', href: getPermalink('/docs/AspectModel') },
        { text: 'Floating Point Model', href: getPermalink('/docs/FLoatingPointModel') },
      ],
    },
    {
      title: 'Standard library',
      links: [
        { text: 'Standard Library', href: getPermalink('/docs/StandardLibrary') },
        { text: 'cajeta.io.net.http', href: getPermalink('/docs/CajetaHttp') },
        { text: 'cajeta.ml', href: getPermalink('/docs/CajetaML') },
        { text: 'cajeta.prism', href: getPermalink('/docs/CajetaPrism') },
        { text: 'cajeta.reflect', href: getPermalink('/docs/CajetaReflect') },
        { text: 'cajeta.torch', href: getPermalink('/docs/CajetaTorch') },
      ],
    },
    {
      title: 'Tooling',
      links: [
        { text: 'Build Tool', href: getPermalink('/docs/BuildTool') },
        { text: 'Compilation', href: getPermalink('/docs/Compilation') },
        { text: 'Debugging', href: getPermalink('/docs/Debugging') },
        { text: 'Documentation', href: getPermalink('/docs/Documentation') },
      ],
    },
    {
      title: 'Process',
      links: [
        { text: 'Harness Design', href: getPermalink('/docs/HarnessDesign') },
        { text: 'Lint Rules', href: getPermalink('/docs/LintRules') },
        { text: 'Test Plan', href: getPermalink('/docs/TestPlan') },
        { text: 'Implementation Status', href: getPermalink('/docs/ImplementationStatus') },
        { text: 'Async Status', href: getPermalink('/docs/AsyncStatus') },
        { text: 'Structs + Views Status', href: getPermalink('/docs/StructsViewsStatus') },
      ],
    },
  ],
  secondaryLinks: [],
  socialLinks: [
    { ariaLabel: 'RSS', icon: 'tabler:rss', href: getAsset('/rss.xml') },
    { ariaLabel: 'Github', icon: 'tabler:brand-github', href: 'https://github.com/jklappenbach/cajeta' },
  ],
  footNote: `
    Cajeta · documentation generated with <a class="text-blue-600 underline dark:text-muted" href="https://github.com/arthelokyo/astrowind">AstroWind</a>.
  `,
};

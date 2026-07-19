// @ts-check
import { defineConfig } from 'astro/config';
import react from '@astrojs/react';
import rehypeDocsLinks from './src/lib/rehype-docs-links.mjs';
import rehypeEditorial from './src/lib/rehype-editorial.mjs';
import { docsRoot } from './scripts/lib/docs-root.mjs';
import cajetaGrammar from './src/grammars/cajeta.tmLanguage.json' with { type: 'json' };

export default defineConfig({
  site: 'https://cajeta.dev',
  trailingSlash: 'ignore',
  integrations: [react()],
  markdown: {
    shikiConfig: {
      themes: { light: 'vitesse-light', dark: 'vitesse-dark' },
      langs: [cajetaGrammar],
      // fence languages used in docs/ that shiki does not bundle
      langAlias: { antlr: 'text', eiffel: 'text', pseudo: 'text' },
      wrap: false,
    },
    rehypePlugins: [rehypeDocsLinks, rehypeEditorial],
  },
  vite: {
    server: {
      // the content collection reads the docs tree, outside the vite root
      fs: { allow: ['..', docsRoot()] },
    },
  },
});

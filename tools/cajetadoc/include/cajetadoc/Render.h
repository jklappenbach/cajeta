// cajetadoc — HTML page generation + themeable, React-adoptable CSS
// (plan §10 / §11, focused first pass).
//
// Emits a package-hierarchical static site: directory tree mirrors the package
// tree (JavaDoc model). Markup is scoped under a single `.cajetadoc` root using
// zero-specificity `:where()` selectors, cascade layers, and design-token
// custom properties with host fallthrough so the output adopts a parent React
// site's theme when embedded, and falls back to a built-in theme standalone.
#ifndef CAJETADOC_RENDER_H
#define CAJETADOC_RENDER_H

#include <string>

#include "cajetadoc/Model.h"

namespace cajetadoc {

// Render a single type page (fragment-and-shell) to a complete HTML document.
// `cssHref` is the relative path from this page to the stylesheet.
std::string renderTypePage(const Type& type, const std::string& cssHref);

// Render a package index page listing the package's types.
std::string renderPackageIndex(const Package& pkg, const std::string& cssHref);

// Render the project overview index listing all packages.
std::string renderOverview(const Model& model, const std::string& cssHref);

// The built-in themeable stylesheet (cascade layers + :where() + tokens).
std::string defaultStylesheet();

// Generate the whole site to `outDir`. Returns the number of pages written;
// fills `error` on failure.
int generateSite(const Model& model, const std::string& outDir, std::string& error);

} // namespace cajetadoc

#endif // CAJETADOC_RENDER_H

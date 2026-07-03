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

class SymbolIndex;

// Project-level chrome shown in the fixed top header bar on every page. The
// icon is a placeholder for now (a built-in inline SVG is used when `iconHref`
// is empty). Empty string fields are simply omitted from the header.
struct SiteMeta {
    std::string title = "Cajeta"; // project title (left, next to the icon)
    std::string iconHref;         // project icon URL; empty => built-in placeholder
    std::string version;          // e.g. "0.5.1" (rendered as "v0.5.1")
    std::string datePublished;    // e.g. "2026-06-05"
    std::string license;          // e.g. "Apache-2.0"
};

// Render a single type page (fragment-and-shell) to a complete HTML document.
// `cssHref` is the relative path from this page to the stylesheet. `index` (if
// non-null) supplies cross-reference links; `pkg` (if non-null) supplies the
// sibling-type nav chrome. `meta` fills the fixed top header bar.
std::string renderTypePage(const Type& type, const std::string& cssHref,
                           const SymbolIndex* index = nullptr, const Package* pkg = nullptr,
                           const SiteMeta& meta = {});

// Render a package index page listing the package's types.
std::string renderPackageIndex(const Package& pkg, const std::string& cssHref,
                               const SymbolIndex* index = nullptr, const SiteMeta& meta = {});

// Render the project overview index listing all packages.
std::string renderOverview(const Model& model, const std::string& cssHref,
                           const SymbolIndex* index = nullptr, const SiteMeta& meta = {});

// Render the site-wide index of @EntryPoint-tagged methods, grouped by package.
std::string renderEntryPointsIndex(const Model& model, const std::string& cssHref,
                                   const SymbolIndex* index = nullptr,
                                   const SiteMeta& meta = {});

// The built-in themeable stylesheet (cascade layers + :where() + tokens).
std::string defaultStylesheet();

// Generate the whole site to `outDir`. Returns the number of pages written;
// fills `error` on failure. `meta` fills the fixed top header bar.
int generateSite(const Model& model, const std::string& outDir, std::string& error,
                 const SiteMeta& meta = {});

} // namespace cajetadoc

#endif // CAJETADOC_RENDER_H

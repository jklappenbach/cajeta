// FilesystemRepository — directory-tree backed repository
// driver. Layout per BuildTool.md "Repositories" section:
//
//   <root>/<name>/<version>/<name>-<version>.cja
//   <root>/<name>/versions.json          (optional, listing)
//   <root>/index.json                    (optional, package list)
//
// Used for dev-overrides, vendoring, and CI scenarios that
// pre-stage artifacts.

#pragma once

#include "cajeta/buildtool/Repository.h"

#include <string>

namespace cajeta::buildtool {

    class FilesystemRepository : public Repository {
    public:
        FilesystemRepository(std::string name, std::string root);

        std::string name() const override { return name_; }

        llvm::Expected<std::vector<std::string>> listVersions(
            const std::string& packageName) const override;

        llvm::Expected<std::string> fetch(
            const std::string& packageName,
            const std::string& version) const override;

    private:
        std::string name_;
        std::string root_;
    };

} // namespace cajeta::buildtool

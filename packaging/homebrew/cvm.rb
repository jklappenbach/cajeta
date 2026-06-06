# Homebrew formula for cvm — the Cajeta Version Manager (plan §2 Tier 1 / D12).
#
# The package ecosystems (brew/apt/dnf/AUR/winget) carry cvm, NOT cajeta: cvm is
# version-independent (a 1.0 cvm installs an 8.0 cajeta), so a single evergreen
# package covers "get latest" forever, sidestepping distro lag. cajeta itself is
# a direct download (Tier-0 release installers).
#
# Installs the PREBUILT cvm binary — not build-from-source: cvm is written in
# Cajeta and compiled by the cajeta compiler, so building it in a formula would
# need cajeta already present (chicken-and-egg). The release workflow builds cvm
# per target; this formula just installs that artifact.
#
# RELEASE AUTOMATION bumps `version` + the per-arch `sha256` for each cvm release
# and pushes to the tap `jklappenbach/homebrew-tap`. Values below are placeholders.

class Cvm < Formula
  desc "Cajeta Version Manager — installs and switches Cajeta toolchains"
  homepage "https://github.com/jklappenbach/cajeta"
  version "0.0.0"
  license "MIT"

  on_macos do
    on_arm do
      url "https://github.com/jklappenbach/cajeta/releases/download/cvm-v#{version}/cvm-v#{version}-aarch64-apple-darwin.tar.gz"
      sha256 "0000000000000000000000000000000000000000000000000000000000000000"
    end
  end

  on_linux do
    on_arm do
      url "https://github.com/jklappenbach/cajeta/releases/download/cvm-v#{version}/cvm-v#{version}-aarch64-linux-gnu.tar.gz"
      sha256 "0000000000000000000000000000000000000000000000000000000000000000"
    end
    on_intel do
      url "https://github.com/jklappenbach/cajeta/releases/download/cvm-v#{version}/cvm-v#{version}-x86_64-linux-gnu.tar.gz"
      sha256 "0000000000000000000000000000000000000000000000000000000000000000"
    end
  end

  def install
    bin.install "cvm"
  end

  # cvm self-update is disabled when it's package-managed (it detects its own
  # exe under the brew prefix); update with `brew upgrade cvm` instead. cvm still
  # manages cajeta toolchains under ~/.cajeta regardless.
  def caveats
    <<~EOS
      Run `cvm` to finish setup (creates ~/.cajeta, installs the latest Cajeta
      toolchain, and adds it to your PATH). Update cvm itself with:
        brew upgrade cvm
    EOS
  end

  test do
    assert_match version.to_s, shell_output("#{bin}/cvm --version")
  end
end

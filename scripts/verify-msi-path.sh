#!/usr/bin/env bash
# Does this MSI put its bin/ on PATH?  usage: verify-msi-path.sh <file.msi>...
#
# WiX installs under Program Files, which is not on PATH, so both the compiler
# and cvm attach a WiX <Environment> element through CPACK_WIX_PATCH_FILE. That
# patch binds to a component id CPack DERIVES from the component name and the
# install path, so a rename upstream silently drops the PATH entry and the MSI
# still builds. Checking that a .msi exists does not catch it. This reads the
# Environment table and fails when no PATH row is there.
#
# WindowsInstaller.Installer is present on any Windows host, so this needs no
# extra tooling. Off Windows it skips, because there is nothing to read with.
set -uo pipefail

if [ "$#" -eq 0 ]; then
    echo "usage: verify-msi-path.sh <file.msi>..." >&2
    exit 2
fi
if ! command -v powershell >/dev/null 2>&1; then
    echo ">> verify-msi-path: powershell absent — skipped (not a Windows host)"
    exit 0
fi

fails=0
for m in "$@"; do
    [ -f "$m" ] || { echo "::error::no such MSI: $m"; fails=$((fails + 1)); continue; }
    win="$(cygpath -w "$m" 2>/dev/null || printf '%s' "$m")"
    rows=$(MSYS2_ARG_CONV_EXCL='*' powershell -NoProfile -Command "
      \$i  = New-Object -ComObject WindowsInstaller.Installer
      \$db = \$i.GetType().InvokeMember('OpenDatabase','InvokeMethod',\$null,\$i,@('${win}',0))
      \$v  = \$db.GetType().InvokeMember('OpenView','InvokeMethod',\$null,\$db,@('SELECT \`Name\`,\`Value\` FROM Environment'))
      \$v.GetType().InvokeMember('Execute','InvokeMethod',\$null,\$v,\$null)
      while (\$r = \$v.GetType().InvokeMember('Fetch','InvokeMethod',\$null,\$v,\$null)) {
        \$n = \$r.GetType().InvokeMember('StringData','GetProperty',\$null,\$r,1)
        \$d = \$r.GetType().InvokeMember('StringData','GetProperty',\$null,\$r,2)
        Write-Output \"\$n=\$d\"
      }" 2>&1 | tr -d '\r')
    printf '  %-52s Environment: %s\n' "$(basename "$m")" "${rows:-<empty>}"
    case "$rows" in
        *PATH*) ;;
        *)  echo "::error::$(basename "$m") has no PATH row in its Environment table, so installing it puts nothing on PATH. The WiX patch's component id no longer matches what CPack generated."
            fails=$((fails + 1)) ;;
    esac
done

[ "$fails" -eq 0 ] || exit 1
echo "verify-msi-path: OK"

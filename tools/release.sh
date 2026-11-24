#!/usr/bin/env bash
# tools/release.sh X.Y.Z NOTES.md - publishes version X.Y.Z of the port from this repository.
#
# NOTES.md (kept outside this repository) holds the release description: only what is new in this version. The
# script puts X.Y.Z in tools/VERSION and commits it as "Release X.Y.Z", brings in what is on GitHub (rebase), pushes
# main and the annotated tag vX.Y.Z whose message is NOTES.md. The tag starts .github/workflows/build-windows.yml,
# which builds the package and publishes the release with the tag message as its notes; launchers of earlier versions
# then offer the update.
# While a remote named `legacy` exists (the old fidalgojoao/UCMR, which launchers before 0.10.156 ask for updates),
# main and the tag go there too, so its build publishes the same release.
set -euo pipefail

version=${1:-}
notes=${2:-}
if [[ ! $version =~ ^[0-9]+\.[0-9]+\.[0-9]+$ || -z $notes ]]; then
    echo "usage: tools/release.sh X.Y.Z NOTES.md" >&2
    exit 2
fi
if [[ ! -s $notes ]]; then
    echo "missing or empty release notes: $notes" >&2
    exit 1
fi
notes=$(cd -- "$(dirname -- "$notes")" && pwd)/$(basename -- "$notes")
cd -- "$(dirname -- "$0")/.."
tag=v$version
git fetch -q --tags origin
if git rev-parse -q --verify "refs/tags/$tag" > /dev/null; then
    echo "$tag is taken already" >&2
    exit 1
fi
if [[ $(git rev-parse --abbrev-ref HEAD) != main ]]; then
    echo "releases are made from main" >&2
    exit 1
fi

echo "$version" > tools/VERSION
git add tools/VERSION
git commit -q -m "Release $version"
git pull -q --rebase --autostash origin main
git push -q origin main
git tag -a --cleanup=verbatim -F "$notes" "$tag"
git push -q origin "$tag"
if git remote get-url legacy > /dev/null 2>&1; then
    git push -q legacy main || { echo "the old repository has commits main lacks: bring them over first" >&2; exit 1; }
    git push -q legacy "$tag"
fi
echo "$tag pushed: the build publishes the release when it is done."

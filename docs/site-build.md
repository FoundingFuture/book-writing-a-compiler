# Site build check

A chapter is pushed after `check-web-content` passes and a throwaway Hugo build of the site passes. `check-web-content` does not catch a broken `relref` or a front-matter key the theme rejects. The build does.

## Procedure

1. Export the site repository `FoundingFuture/website` at its `origin/main` into a scratch directory.
2. Run the site's `tools/get-theme.sh` in the copy. It fetches the newest ff-1 release, as `./c` does.
3. Replace the `contentDir` setting with two module mounts: the site's `site/content`, and this checkout's `writing-a-compiler/` at `content/programming/writing-a-compiler`.
4. Run `hugo --panicOnWarning --printPathWarnings` in the copy. A finished chapter has `draft: false`, because the build skips drafts.
5. Delete the copy.

`site/content/programming/_index.md` already exists in the site repository.

## Script

Set `WEBSITE` to a local clone of `FoundingFuture/website` and `SCRATCH` to an empty scratch directory.

```bash
#!/usr/bin/env bash
set -euo pipefail
WEBSITE="${WEBSITE:?clone of FoundingFuture/website}"
SCRATCH="${SCRATCH:?scratch directory}"
BOOK="$(git rev-parse --show-toplevel)/writing-a-compiler"
COPY="$SCRATCH/site-copy"

rm -rf "$COPY"
mkdir -p "$COPY"
git -C "$WEBSITE" fetch -q origin
git -C "$WEBSITE" archive origin/main | tar -x -C "$COPY"
(cd "$COPY" && tools/get-theme.sh)

sed -i.bak '/^contentDir = /d' "$COPY/hugo.toml"
cat >> "$COPY/hugo.toml" <<TOML

[[module.mounts]]
source = "site/content"
target = "content"

[[module.mounts]]
source = "$BOOK"
target = "content/programming/writing-a-compiler"
TOML

status=0
(cd "$COPY" && hugo --panicOnWarning --printPathWarnings) || status=$?
rm -rf "$COPY"
exit "$status"
```

The site pins Hugo 0.165.0 in `tools/bootstrap-hugo.sh`. Use the same version.

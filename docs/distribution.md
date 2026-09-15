# Anti distribution

Design of how Anti libraries are published and how licence obligations travel with
programs. Settled on 2026-09-14 and aligned with `docs/decisions.md` on 2026-09-15.
`docs/decisions.md` is the authority, and the details below agree with it, with
`docs/tooling.md` and with `docs/tooling-addendum.md`.

Contents:

- [HTTPS](#https)
- [CA bundle](#ca-bundle)
- [Publication targets](#publication-targets)
- [Transports](#transports)
- [Publish sequence](#publish-sequence)
- [GitHub templates](#github-templates)
- [Licence fields](#licence-fields)
- [Embedded licence text](#embedded-licence-text)
- [License command](#license-command)
- [Obligations of a shipped program](#obligations-of-a-shipped-program)
- [Licence choices for Anti itself](#licence-choices-for-anti-itself)
- [Chapter changes](#chapter-changes)

## HTTPS

Every repository `url` is `https://` or `file://`. `http://` is refused, except for
`127.0.0.1` and `localhost`, so the resolver tests can run against a local server
without certificates.

`anti` links Mbed TLS from the runtime archive for its HTTPS client. The index fetch,
the ETag check, the package download and the post-publish verification all use it. The
`anti.net` module in the standard library uses the same library later.

## CA bundle

The trust store is Mozilla's root store, taken as curl's extract `cacert.pem` from
https://curl.se/docs/caextract.html. Licence: MPL 2.0.

- The runtime archive's CMake build downloads the file for a pinned date, verifies the
  SHA-256 curl publishes next to it, and installs it as `lib/cacert.pem`.
- The pin is bumped with every runtime archive release.
- `anti` and the `anti.net` module load the file at runtime from the archive. It is never
  embedded in a user's executable.
- On Linux, `--system-certs` reads `/etc/ssl/certs` instead. macOS Keychain and Windows
  CryptoAPI are not read. Mbed TLS cannot open them without a platform export. That
  export is out of scope.

The runtime archive's licence directory lists Mozilla and the MPL 2.0 for the bundle.

## Publication targets

`anti publish` writes to a publication target. Targets live in the user's
`~/.anti/config.toml`, never in a project, because they name hosts and keys.

```toml
[publish.ff]
url = "https://anti.foundingfuture.com/repo"
transport = "ssh://primus/var/www/anti/repo"

[publish.gh]
url = "https://raw.githubusercontent.com/eddie/anti-packages/main"
transport = "git+ssh://git@github.com/eddie/anti-packages.git"

[publish.local]
url = "file:///Users/eddie/antl-repo"
transport = "file:///Users/eddie/antl-repo"
```

`url` is what users list under `[repositories]`. `transport` is how `anti` writes.
`anti publish --to ff` selects a target. `publish = "ff"` under `[package]` in
`anti.toml` sets the project default.

## Transports

The transport is a URL scheme. `anti` runs system binaries for authentication and never
reads a key itself.

| Scheme | Mechanism |
|---|---|
| `file://` | Writes directly to the path |
| `ssh://host/path` | Runs `ssh` and `rsync`. `~/.ssh/config` supplies user, port, key and jump hosts. Reads the remote index with `ssh host cat` |
| `git+ssh://` and `git+https://` | Clones or fetches into `~/.anti/publish/<sha256-of-url>/`, copies the staged files, commits, runs `git pull --rebase`, pushes. Retries once on a rejected push |
| `cmd:<program>` | Runs the program with the staging directory and the target name as arguments. For S3, FTP or any custom API |

## Publish sequence

`anti publish` runs these steps in order:

1. Refuse when `[package]` lacks `license` or `license_text`, or when the package has
   dependencies and no `anti.lock`.
2. Build the `.antl` files in release mode and compute the `sha256` of each.
3. Read the current `<name>/index.toml` through the transport, not from the cache.
4. Write `build/publish/<name>/<version>/<module>.antl` for every module, `sha256`, and
   the updated `<name>/index.toml` with the modules of the version and `revision`
   increased by one.
5. Upload the version directory, then `sha256`, then `index.toml`. A reader never sees
   an index entry whose files are missing.
6. Fetch `<url>/<name>/index.toml` over HTTPS and confirm the new entry is present. On
   failure, print the URL read and the entry expected. The usual cause is a wrong `url`.

`--dry-run` runs steps 1 to 4 and prints the transport commands without running them.

## GitHub templates

Two template repositories on github.com. Both automate `anti publish`. Neither replaces
it.

`anti-repository`, one per author or organisation:

- Holds `<name>/index.toml` and `<name>/<version>/` directories on `main`.
- Served by `raw.githubusercontent.com`. The raw URL of `main` is the repository users
  list.
- Has no workflow. Publishing is a commit made by a library's workflow.

`anti-library`, one per library:

- The default layout, a starter `anti.toml`, `.gitignore` for `build/` and `dist/`.
- `check.yml` on demand, through a manual trigger: `anti check`, then `anti test` on
  GitHub's runners for Linux x86_64 and ARM64, macOS ARM64 and x86_64, Windows x86_64
  and ARM64. It does not run on every push, because hosted build minutes are limited.
- `release.yml` on a tag `v<version>`: verifies the tag equals `version` in `anti.toml`,
  runs `check.yml`, then `anti publish --to gh`, then attaches the `.antl` and `sha256`
  to the GitHub release.
- Pins the `anti` version it installs.

Setup the template cannot do, stated in its `README.md`:

1. Create a repository from `anti-repository`.
2. Generate a deploy key with write access to it.
3. Store the private half as the secret `ANTI_REPO_KEY` in each library repository.
   The workflow adds it to the runner's ssh agent before `anti publish`.

The two templates are created only after `anti` exists and has published one release by
hand. A template that has never run carries untested steps.

Two libraries releasing at the same moment never touch the same file, because the index
is per package. The `git pull --rebase` in the transport handles the push contention.

## Licence fields

`[package]` in `anti.toml` gains three fields:

```toml
[package]
name = "com.foundingfuture.anti.tree"
version = "1.2.4"
license = "MIT"
license_text = "LICENSE"
attribution = ["Copyright 2026 Eddie", "Contains code from Example Corp, BSD-3-Clause"]
```

- `license` is an SPDX identifier.
- `license_text` is a path to the full text, or an `https://` URL.
- `attribution` is zero or more lines copied verbatim.

The `.antl` header carries all three. A path in `license_text` is replaced by the file's
content at build time. The header never references the source tree. The runtime
archive's `.antl` files carry the same fields, written by the CMake build from the pinned
sources of each bundled library.

## Embedded licence text

The driver knows every module in an executable or a shared library at link time. It
emits one read-only data object, symbol `anti_licenses`, holding:

- For every linked package, in dependency order: name, version, SPDX identifier and
  attribution lines.
- Every distinct licence text once, followed by the names of the packages it covers.

The object starts and ends with a fixed marker string so a tool can find it in any Anti
binary. Size is a few kilobytes. The embedding is unconditional. The notices travel with
the binary, which is what Apache 2.0 and BSD 3-clause require.

A static archive carries no `anti_licenses`, because it has no link step and two archives
in one C program would define the symbol twice. It carries a copy of the package header
of its `.antl`, licence fields included.

The program's command line is not touched. The standard library exposes the text as
`license.text() -> str` in the module `anti.license`. The `main` that `anti new` writes
handles `--licenses`:

```anti
import anti.io;
import anti.license;
import anti.text;

fn main(args: []str) -> int
{
    if args.len > 1 && text.equal(args[1], "--licenses") {
        io.print(license.text());
        return 0;
    }
    return 0;
}
```

The author may delete those lines. Nothing in the language depends on them.

## License command

`anti license` has five forms:

| Form | Prints |
|---|---|
| `anti license` | The licence of `antic` and `anti`, then every runtime archive component with name, version, identifier and full text |
| `anti license --project` | The packages the current project links, from `anti.lock` and the imported bundled modules, with identifiers, attributions and texts |
| `anti license --project --notice` | Writes the same content as `dist/<os>-<cpu>/<mode>/NOTICE.txt` |
| `anti license --from <executable>` | Reads `anti_licenses` out of the binary by its marker and prints it |
| `anti license --from-archive lib<name>.a` | Reads the licence fields from the copy of the `.antl` package header in a static archive, so a C project can produce its notice |

The runtime archive holds a `licenses/` directory with one file per component, written
by the CMake build. `anti license` reads it. Nothing is typed twice.

## Obligations of a shipped program

Code emitted by `antic` is the user's code translated. The LLVM licence exception says
the same for llvm-mc output. The user's obligations toward Anti are whatever the Anti
licence states about output, and it states none.

Static linking is where obligations enter. What the runtime archive links into an
executable carries its licence with it:

| Component | Licence | Binary obligation |
|---|---|---|
| Thread-pool runtime and standard library | 0BSD | None |
| raylib | zlib | None. Notice only in source distributions |
| miniaudio | MIT-0 | None |
| Mbed TLS | Apache 2.0 | Ship the licence and notice with the binary |
| PCRE2 | BSD 3-clause | Reproduce the copyright notice with the binary |
| CA bundle | MPL 2.0 | None. The file is loaded from the archive, not shipped |

A program that imports `anti.raylib` and `anti.miniaudio` owes nobody anything. A
program that imports `anti.net` or `anti.regex` owes an attribution, and `anti_licenses`
plus `NOTICE.txt` supply it. This section is a statement of how the licences read, not legal advice.

## Licence choices for Anti itself

- The thread-pool runtime and the standard library: 0BSD, with a `LICENSE` file in
  `rt/` and in `std/`. Use without attribution, so the common case embeds one short
  notice and owes nothing.
- `antic` and `anti`: MIT. The compiler's licence never reaches its output.
- The book text: decided separately on the site.

## Chapter changes

- Chapter 9: the three licence fields in the `.antl` header.
- Chapter 16: the `anti_licenses` data object and its markers in the emitter.
- Chapter 23: the CA bundle, the `licenses/` directory, the obligations table, the
  licence choices for the runtime and standard library.
- The build tool chapter: HTTPS, publication targets, transports, the publish
  sequence, the two GitHub templates, `anti license`, described without the code of
  `anti`.
- The standard library chapters: the `anti.license` module and the `--system-certs`
  option of `anti.net`.

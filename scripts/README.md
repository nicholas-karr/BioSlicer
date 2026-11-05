# BioSlicer Build & Deploy

## Flatpak (Linux distribution)

BioSlicer is distributed as a Flatpak. There is a single installer,
[`flatpak/install.sh`](../flatpak/install.sh), committed in the repo, and it has two sources:

| Mode | Source | Used for | Update cost |
|---|---|---|---|
| release (default) | `BioSlicer-<arch>.flatpak` + `.sha256` attached to the latest [GitHub Release](https://github.com/nicholas-karr/BioSlicer/releases) | Public builds | Whole bundle (~90 MB), skipped if already installed |
| repo | An **OSTree repository** at `<url>/repo-<arch>`, chosen with `BIOSLICER_URL=<url>` or a `deploy.sh`-generated `install.sh` | Self-hosted / fast test loop | Only the files that changed (~40 MB for a code change, "Nothing to do" if none) |

Why two: GitHub Releases can only host flat asset files, so public builds are bundles, as
with PrusaSlicer. An OSTree repo is a directory tree of thousands of hashed files that needs
a real web server, but it updates incrementally and skips the slow bundling step, which is
what a build-test loop needs. OSTree stores every file once, named by its hash, so a client
only downloads files it doesn't already have.

End users run the installer straight from GitHub (see the
[main README](../README.md#install-linux--wsl2)). Installer environment variables:
`BIOSLICER_URL` (use a self-hosted repo), `BIOSLICER_SUFFIX` (its channel suffix),
`BIOSLICER_VERSION` (release tag instead of latest), `BIOSLICER_RELEASE_URL` (override the
release download base URL, e.g. to test against a local server).

### Building and deploying

```bash
flatpak/deploy.sh --web-root <dir> [--url <public url of dir>] [--bundle] [--arch x86_64]
```

This builds the Flatpak, exports it into a persistent local OSTree repo
(`.flatpak-repo-<arch>`), and mirrors it into `<dir>`:

- `repo-<arch>/` (`repo-x86_64`, `repo-aarch64`), always
- `install<suffix>.sh`: a copy of the installer set to repo mode for `--url`, when `--url` is given
- `BioSlicer-<arch>.flatpak` and `.sha256`, with `--bundle` (the GitHub Release assets)

Options: `--arch x86_64|aarch64|all` (default: host, plus aarch64 when a cross toolchain is
available), `--bundle` (also make release bundles: ~90 s slower, single-threaded),
`--deltas` (generate static deltas for the repo: ~15 s slower to publish, roughly halves the
download), `--suffix S` (publish `install<S>.sh`, `repo-<arch><S>` and `BioSlicer-<arch><S>.flatpak`
as a separate channel), `--no-build` (redeploy `install.sh` only). Run `flatpak/deploy.sh` with
no arguments for the option list.

The last build's inputs are hashed, so re-running with no source changes skips the build (~2 s).
The repo is kept between runs, so export only writes changed objects. Progress is logged, with
a heartbeat every 10 s during the quiet export/bundle steps and a timing line per phase.

**Typical times** (12 cores, warm ccache): no changes ~2 s; one-file change ~45 s, of which
flatpak-builder is ~35 s; adding `--bundle` adds ~90 s.

### Publishing a release

1. `flatpak/deploy.sh --bundle --web-root dist` on each architecture you ship (or `--arch all`).
2. Attach `dist/BioSlicer-<arch>.flatpak` and `.flatpak.sha256` for every arch to a GitHub
   Release, keeping those exact file names:
   `gh release create v0.1.0 dist/BioSlicer-*.flatpak dist/BioSlicer-*.flatpak.sha256`.
3. `curl -fsSL https://raw.githubusercontent.com/nicholas-karr/BioSlicer/master/flatpak/install.sh | bash`
   now installs it.

**Planned:** a GitHub Actions workflow that does this on version tags. A cold CI build compiles
all dependencies, so it needs the GNOME 49 SDK plus a cache for the flatpak-builder state and
ccache to stay fast. Not set up yet.

### Installing what you built

- **Same machine:** `flatpak/install-local.sh [arch]` installs from the local repo; no web
  server or release needed.
- **Another machine, self-hosted:** `curl -fsSL <url>/install.sh | bash` (or the repo's
  installer with `BIOSLICER_URL=<url>`). The first run installs; later runs update only what
  changed. To test without a real server: `cd <web-root> && python3 -m http.server 8123`, then
  deploy with `--url http://<host>:8123`.

The self-hosted repo is unsigned (built locally), so the installer adds it with
`--no-gpg-verify`. An update can also fetch a newer GNOME runtime from Flathub if one exists.

### Cache behaviour

The build has three layers of caching:

| Layer | What's cached | Invalidated when |
|---|---|---|
| deploy.sh input hash | The whole published build | Any tracked/untracked source file changes |
| flatpak module cache (ostree) | Built deps (Boost, OCCT, wxWidgets, …) | Anything in `deps/` or `cmake/` changes |
| ccache | Individual `.o` files for BioSlicer | Source file content changes |

`flatpak/bioslicer-deps-src.tar.gz` is an archive of `deps/` and `cmake/` that the manifest's
`BioSlicerDeps` module builds from. It is not committed: `deploy.sh` regenerates it on every
build from the files git knows about (so local build trees such as `deps/build-default` never
get in), with fixed order, timestamps, owners and modes, so the same sources give the same
bytes, and the same sha256, on every machine.

flatpak-builder keys the deps cache on the `sha256:` recorded for that archive in
`flatpak/com.bioslicer.BioSlicer.yml`, not on the file's content. **If you change anything in
`deps/` or `cmake/`**, `deploy.sh` notices that the archive no longer matches the recorded
checksum, builds from a generated copy of the manifest carrying the real one (which rebuilds
and re-caches the deps, a long build), and prints the value to put into the manifest when
you commit the change.

### What the installer does to your system

The installer can also change packages, D-Bus state, shell configuration, older
Flatpak installations, and WSL sudoers configuration. Its complete system-effect
inventory is maintained in
[`flatpak/INSTALLER_EFFECTS.md`](../flatpak/INSTALLER_EFFECTS.md).

## Typical SLA Flow

1. Configure SLA video fields in slicer custom G-code settings.
2. Export G-code.
3. Inspect or test the generated frame sequence:

   ```bash
   python scripts/extract_sla_frames_from_gcode.py build/generated/job.gcode
   ```

The extractor retains only the G-code parsing and ffmpeg frame-decoding support
needed to test slicer output. Printer display services and controller macros are
deliberately outside this repository.

## Hybrid Multi-Material Generators

Each script below generates both a `.3mf` and sliced `.gcode` in `build/generated/`.
They are designed for up to three FFF materials plus one SLA material, with geometry bounded to at most `50x50x50 mm`.

### 1) Tricolor Waffle Tower + SLA Stitch Nodes

```bash
python scripts/gen_hybrid_tricolor_waffle_tower.py
```

### 2) Braided Tri-Color Column + SLA Spine

```bash
python scripts/gen_hybrid_braided_column.py
```

### 3) Voxel Moire Block + SLA Marker Lattice

```bash
python scripts/gen_hybrid_voxel_moire_block.py
```

All scripts support:

```bash
--prusa-slicer <path>
--output-dir <dir>
--name <base_name>
--machine-settings-ini <ini>
--sla-synth-width <px>
--sla-synth-height <px>
--sla-synth-fps <fps>
--sla-synth-lossless
```

## Channel Setup Overrides (Scalable Logical Extruders)

For high-channel printers (for example 100 logical channels), generate one override INI per physical setup:

```bash
python scripts/gen_channel_setup_override.py \
	--output build/generated/setup_A_10ch.ini \
	--channels 100 \
	--default-nozzle 0.4 \
	--nozzle-map 1=0.4,2=0.6,3=0.25,4=0.8 \
	--sla-channels 2,17 \
	--embed-sla-video \
	--synthesize-sla-video
```

This writes vector options such as `nozzle_diameter`, `sla_material_extruder`, and `toolchange_gcode` so one printer profile can be reused across many hardware setups.

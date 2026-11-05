![BioSlicer logo](/resources/icons/PrusaSlicer.png?raw=true)

# BioSlicer

BioSlicer is a fork of [PrusaSlicer](https://www.prusa3d.com/prusaslicer/) with
hybrid FFF + SLA (video) multi-material support. It is distributed for Linux as a
Flatpak.

## Install (Linux / WSL2)

This one command is the canonical way to install, update and launch BioSlicer. It runs the
installer from this repository ([flatpak/install.sh](flatpak/install.sh)), which downloads the
Flatpak attached to the [latest GitHub Release](https://github.com/nicholas-karr/BioSlicer/releases/latest):

```bash
curl -fsSL https://raw.githubusercontent.com/nicholas-karr/BioSlicer/master/flatpak/install.sh | bash
```

- **First run:** installs Flatpak if needed, installs BioSlicer for your user, creates a
  `bioslicer` shortcut in `~/.local/bin`, and launches the app.
- **Later runs:** the same command updates BioSlicer to the newest release (it skips the
  download if you already have it) and launches it.
- After installing you can also start it with `bioslicer` or
  `flatpak run com.bioslicer.BioSlicer`.
- Works on `x86_64` and `aarch64`; the installer picks the right one.
- To install a specific release, set `BIOSLICER_VERSION=<tag>` (for example
  `curl -fsSL https://raw.githubusercontent.com/nicholas-karr/BioSlicer/master/flatpak/install.sh | BIOSLICER_VERSION=v0.1.0 bash`).

> **Status:** the release workflow is planned but not set up yet, so there are no
> release builds and the installer will report "No BioSlicer release build" until the first
> one is published. Until then, build and host it yourself (see [Build](#build)) and run the
> same installer against your host: `curl -fsSL https://raw.githubusercontent.com/nicholas-karr/BioSlicer/master/flatpak/install.sh | BIOSLICER_URL=https://your-host/BioSlicer bash`.

The installer can also install packages, start D-Bus and (on WSL2) add sudoers rules.
See [flatpak/INSTALLER_EFFECTS.md](flatpak/INSTALLER_EFFECTS.md) for everything it changes.

## Build

### Flatpak (what the installer above installs)

Requires `flatpak`, `flatpak-builder`, `rsync` and `ccache`; `ostree` is optional but makes
export faster. Run from the repository root. There are two kinds of output:

**Test / self-hosted build** — an OSTree repo that clients update incrementally (only changed
files are downloaded). Fast: about 45 s for a one-file change with a warm ccache, ~2 s if
nothing changed.

```bash
flatpak/deploy.sh --arch x86_64 --url https://your-host/BioSlicer --web-root <dir served at that URL>
```

This writes `repo-<arch>/` and a copy of `install.sh` pre-configured with `--url` to
`--web-root`, so `curl -fsSL https://your-host/BioSlicer/install.sh | bash` installs and
updates from it. `--arch x86_64` builds the host architecture only (the default also builds
aarch64 when a cross toolchain is available); `--deltas` makes updates download about half as
much; `--suffix -dev` publishes a parallel channel.

**Release build** — single-file bundles, the assets attached to a GitHub Release (the same way
PrusaSlicer ships). Slower, because bundling compresses on one thread (~90 s extra):

```bash
flatpak/deploy.sh --bundle --web-root dist
```

This writes `dist/BioSlicer-<arch>.flatpak` and `dist/BioSlicer-<arch>.flatpak.sha256`
(and the OSTree repo, which you can ignore). Attach both files for every architecture to a
GitHub Release, keeping those exact names — the installer looks for them at
`https://github.com/nicholas-karr/BioSlicer/releases/latest/download/`:

```bash
gh release create v0.1.0 --title "BioSlicer 0.1.0" dist/BioSlicer-*.flatpak dist/BioSlicer-*.flatpak.sha256
```

**Planned:** a GitHub Actions workflow that runs the release build on version tags and attaches
the assets automatically. Not set up yet.

To install what you just built on the same machine, without a web server or release:

```bash
flatpak/install-local.sh
```

See [scripts/README.md](scripts/README.md) for build details, caching and the deploy workflow.

### Native build

If you want to compile the source directly instead of via Flatpak, follow one of these:
* [Linux](doc/How%20to%20build%20-%20Linux%20et%20al.md)
* [macOS](doc/How%20to%20build%20-%20Mac%20OS.md)
* [Windows](doc/How%20to%20build%20-%20Windows.md)

## About

BioSlicer (like PrusaSlicer) takes 3D models (STL, OBJ, AMF) and converts them into G-code
instructions for FFF printers or PNG layers for mSLA 3D printers. It's
compatible with any modern printer based on the RepRap toolchain, including all
those based on the Marlin, Prusa, Sprinter and Repetier firmware. It also works
with Mach3, LinuxCNC and Machinekit controllers.

It is based on [PrusaSlicer](https://github.com/prusa3d/PrusaSlicer), which is based on [Slic3r](https://github.com/Slic3r/Slic3r) by Alessandro Ranellucci and the RepRap community.

See the [upstream PrusaSlicer homepage](https://www.prusa3d.com/slic3r-prusa-edition/) and
the [documentation directory](doc/) for more information.

### What language is it written in?

All user facing code is written in C++.
The slicing core is the `libslic3r` library, which can be built and used in a standalone way.
The command line interface is a thin wrapper over `libslic3r`.

### What are PrusaSlicer's main features?

Key features are:

* **multi-platform** (Linux/Mac/Win) and packaged as standalone-app with no dependencies required
* complete **command-line interface** to use it with no GUI
* multi-material **(multiple extruders)** object printing
* multiple G-code flavors supported (RepRap, Makerbot, Mach3, Machinekit etc.)
* ability to plate **multiple objects having distinct print settings**
* **multithread** processing
* **STL auto-repair** (tolerance for broken models)
* wide automated unit testing

Other major features are:

* combine infill every 'n' perimeters layer to speed up printing
* **3D preview** (including multi-material files)
* **multiple layer heights** in a single print
* **spiral vase** mode for bumpless vases
* fine-grained configuration of speed, acceleration, extrusion width
* several infill patterns including honeycomb, spirals, Hilbert curves
* support material, raft, brim, skirt
* **standby temperature** and automatic wiping for multi-extruder printing
* [customizable **G-code macros**](https://github.com/prusa3d/PrusaSlicer/wiki/PrusaSlicer-Macro-Language) and output filename with variable placeholders
* support for **post-processing scripts**
* **cooling logic** controlling fan speed and dynamic print speed

### Can I help?

Sure! You can do the following to find things that are available to help with:
* Add an [issue](https://github.com/prusa3d/PrusaSlicer/issues) to the github tracker if it isn't already present.
* Look at [issues labeled "volunteer needed"](https://github.com/prusa3d/PrusaSlicer/issues?utf8=%E2%9C%93&q=is%3Aopen+is%3Aissue+label%3A%22volunteer+needed%22)

### What's PrusaSlicer license?

PrusaSlicer is licensed under the _GNU Affero General Public License, version 3_.
The PrusaSlicer is originally based on Slic3r by Alessandro Ranellucci.

### How can I use PrusaSlicer from the command line?

Please refer to the [Command Line Interface](https://github.com/prusa3d/PrusaSlicer/wiki/Command-Line-Interface) wiki page.

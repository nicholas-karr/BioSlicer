Source copies of two dependencies that the dependency build normally downloads from github.com, for environments
without access to those repositories. They are the same versions the build pins, not modified:

- libbgcode: prusa3d/libbgcode commit 5041c093b33e2748e76d6b326f2251310823f3df (build with
  `-DLibBGCode_SOURCE_DIR=<repo>/deps/vendored/libbgcode` in the deps build, or build and install it by hand).
- heatshrink: atomicobject/heatshrink v0.4.1.

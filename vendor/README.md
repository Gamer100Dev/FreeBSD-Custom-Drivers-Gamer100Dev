# Vendored source snapshots

The source code for this project is stored here:

- `freebsd-stable-15/` — primary FreeBSD RTW88 implementation, module Makefile, and LinuxKPI USB compatibility implementation.
- `linux-master/` — focused Linux RTW88 reference implementation.

See [the import manifest](../upstream/IMPORT-MANIFEST.md) for exact upstream commit IDs and imported paths.

Make project changes in a separate patch/overlay rather than silently editing vendored upstream files. Keep original headers intact and record every local change.

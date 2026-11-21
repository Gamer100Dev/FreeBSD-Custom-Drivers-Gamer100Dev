# CMake build frontend for native RTL8822BU module

CMake is a build frontend/orchestrator; the module itself is compiled by the
FreeBSD kernel-module make system (bsd.kmod.mk), not by a generic C compiler.
Run this on FreeBSD with a matching source tree and CMake installed.

```sh
cmake -S . -B build -DFREEBSD_SRCROOT=/usr/src
cmake --build build
```

The final module is copied to `build/if_rtw88_native.ko`. The source-tree
module output is also retained under
`$FREEBSD_SRCROOT/sys/modules/rtw88_native/if_rtw88_native.ko`.

To install through CMake's install target:

```sh
cmake --install build --prefix /usr/local
```

That installs the module file into `/usr/local/lib`. Installation and loading
are intentionally not performed automatically by the build. Only load the
module on a test system, and note that this driver is still experimental.

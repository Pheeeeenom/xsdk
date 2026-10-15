# Third-party notices

xsdk is under the MIT licence (LICENSE). A few of its files carry code from other projects under their own licences,
and those files keep their notices.

## In every title built with xsdk

`libxsdk.a` includes code from libxenon and from Xenia, both under the 3-clause BSD licence, which asks that a program
distributed as a binary carry the notice in its documentation. A game or homebrew title built with xsdk meets that by
shipping this section with it: in its documentation, or in a notices file beside the `.xex`.

### libxenon

In `src/gpu_draw.c`, `src/gpu_resolve.c`, `src/gpu_setup.c`, `src/shader.c` and `src/texture.c`, from libxenon's
`drivers/xenos/xe.c`.

```
Copyright (c) 2009, free60.org
USB Stack Copyright (c) 2000,2001,2002,2003,2005, Broadcom Corporation

All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of the copyright holder nor the names of its
      contributors may be used to endorse or promote products derived
      from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY free60.org ''AS IS'' AND ANY
EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL free60.org BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE
```

### Xenia

In `src/texture_layout.c` and `src/tiled.c`, from Xenia's `src/xenia/gpu/texture_util.cc`, `texture_util.h` and
`texture_address.h`.
xost's shader encoder (`src/xsc_*.c`) also follows the instruction layouts in Xenia's microcode definitions.

```
Copyright (c) 2015, Ben Vanik.
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of the project nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL BEN VANIK BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

## In the repository only

Not linked into titles, so nothing for a game to ship:

- `tests/tiled_index.h` and `tests/probe_display.c`: `tiled_index` from libxenon's `drivers/console/console.c`,
  copyright 2009 Georg Lukas, under the 3-clause BSD licence; the files carry the notice.
- `xenos-toolchain/xdis/xenos_ucode.py`: transcribed from Xenia's microcode definitions, under Xenia's licence above;
  the file carries the notice.

## The toolchain

`tools/toolchain/` builds GCC, binutils and newlib, and its patches change those projects, so the patches are under
their licences: GCC's and binutils' GPL version 3 or later, and newlib's own licences (its `COPYING.NEWLIB`). Building
a title with the toolchain doesn't put the title under the GPL: GCC's runtime library exception covers the libgcc and
libstdc++ code linked into it. newlib's C library, also linked into every title, is under the permissive licences
listed in its `COPYING.NEWLIB`.

# Third-party notices

citrusf2 is under the MIT licence (`LICENSE`). The libraries it builds on are under the MIT licence too.

| Component | Licence | Use in citrusf2 |
|---|---|---|
| seqlib, Copyright (c) 2026 the seqlib authors | MIT (`externals/seqlib/LICENSE`) | In `externals/seqlib`, a submodule, compiled into citrusf2: the MIDI, SoundFont 2 and SFZ files and their samples. |
| xsflib, Copyright (c) 2026 the xsflib authors | MIT (`externals/xsflib/LICENSE`) | In `externals/xsflib`, a submodule, compiled into citrusf2: byte order and bounds-checked reads, reads and writes of whole files with UTF-8 paths, the printable form of names, numbers from options, and the console of a command-line tool. Its CMake helpers build citrusf2 and the macOS app. |
| ntdlib, Copyright (c) 2026 the ntdlib authors | MIT (`externals/ntdlib/LICENSE`) | In `externals/ntdlib`, a submodule, compiled into citrusf2: the sound archive and wave readers, the sequence player, the conversion of instruments and filters, and the command line's frame. |

Binary distributions must include the MIT notices of citrusf2, seqlib, xsflib and ntdlib. Shipping `LICENSE` and this file
does that. The licence texts follow.

## Console data

Nothing in citrusf2 licenses game data. The sound archives that citrusf2 reads belong to their copyright holders.

ntdlib's `include/ntdlib/nwsnd/tables.h` holds the numeric tables of NintendoWare's sound libraries: powers of two,
decibel levels, envelope attack multipliers, pan curves, low-pass cutoffs, biquad filter coefficients and sines. The
models use the exact values to reproduce the consoles' output. ntdlib's `src/sequence_export/filter_tables_*.cpp` hold
each console's filters fitted to SoundFont 2 and SFZ settings.

## Licence texts

### seqlib

```
MIT License

Copyright (c) 2026 the seqlib authors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

### xsflib

```
MIT License

Copyright (c) 2026 the xsflib authors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

### ntdlib

```
MIT License

Copyright (c) 2026 the ntdlib authors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

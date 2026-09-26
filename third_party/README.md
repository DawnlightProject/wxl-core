# third_party

Vendored libraries used by wxl-host and the host client. Each folder keeps its upstream licence.
WarcraftXL is GPL-3.0-or-later; every library here is under a permissive licence compatible with it.

| Library | Upstream | Commit | Licence | Used by |
|---|---|---|---|---|
| TLSF 3.1 | github.com/mattconte/tlsf | deff9ab5 (2020-03-29) | BSD-3-Clause (`tlsf/LICENSE`) | host store, client transfer window |
| LZ4 | github.com/lz4/lz4 (`lib/` only) | 0774d055 (2026-06-01) | BSD-2-Clause (`lz4/LICENSE`) | host backing copies |
| Zstandard | github.com/facebook/zstd (`lib/` common, compress, decompress) | 01b7154f (2026-09-18) | BSD-3-Clause, taken under the BSD option (`zstd/LICENSE`) | host cold store |
| enkiTS | github.com/dougbinks/enkiTS (`src/`) | 4cba61a0 (2026-09-02) | zlib (`enkiTS/License.txt`) | host job system |
| Tracy (client only) | github.com/wolfpld/tracy (`public/`) | 0913edf7 (2026-09-24) | BSD-3-Clause (`tracy/LICENSE`) | optional profiling, `-DWXL_TRACY=ON` |

StormLib (MIT) stays under `deps/stormlib`, where it already lived.

Only the files listed above were copied; nothing was modified. To update a library, replace its
folder with the same subset from a newer upstream commit and update this table.

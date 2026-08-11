# Third-party notices

R SDR's original source code is licensed under the MIT License. Bundled
third-party components are not covered by the project's MIT License and retain
their respective copyright and license terms.

## ER-BSAC decoder / Libav sources

`third_party/bsac/` contains the AAC/ER-BSAC decoder and supporting Libav
sources from [yhlee/bsac](https://github.com/yhlee/bsac), commit `f67b817`.
These sources are distributed under the GNU Lesser General Public License
version 2.1 or later (LGPL-2.1-or-later). Some individual upstream files carry
MIT/X11/BSD-style or libjpeg notices; the combined library is distributed
under LGPL-2.1-or-later.

See [`third_party/bsac/LICENSE`](third_party/bsac/LICENSE) and
[`third_party/bsac/COPYING.LGPLv2.1`](third_party/bsac/COPYING.LGPLv2.1) for the
applicable notices and complete license text.

The small `third_party/bsac/bsac_api.c` and `bsac_api.h` integration wrapper is
original R SDR code and is covered by the project's MIT License.

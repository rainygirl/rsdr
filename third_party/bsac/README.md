# ER-BSAC decoder

This directory contains the AAC/ER-BSAC decoder and the minimum supporting
Libav sources from [yhlee/bsac](https://github.com/yhlee/bsac), commit
`f67b817`. They are LGPL 2.1 or later; see `LICENSE` and `COPYING.LGPLv2.1`.

The small `bsac_api` wrapper is original R SDR code licensed under the MIT
License; it keeps the legacy Libav API out of the C++ application. Only the AAC
decoder is enabled by `config.h`.

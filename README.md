<div align="center">

```
 ___  ___  ___  ___
/ __|/ _ \| _ \| _ \     Its So Cool Format
\__ \ (_) |  _/|  _/     .isc — свой формат архивов
|___/\___/|_|  |_|       сжатие без компромиссов
```

# isc-archive

**Формат архивов `.isc` (ISCF v1)** — контейнер, компрессор и утилита в одном флаконе.
Свои алгоритмы, свои формулы, свои понятия. Цель — сильное сжатие **и** быстрая распаковка.

![license](https://img.shields.io/badge/license-MIT-green)
![language](https://img.shields.io/badge/language-C99-blue)
![version](https://img.shields.io/badge/version-1.0.0-orange)
![deps](https://img.shields.io/badge/dependencies-0-success)

</div>

---

## What is this

`.isc` is a from-scratch archive format: its own container (ISCF), its own LZ+Huffman
compressor (**LZI**), its own checksum (**isum**), its own block router (**рецепты** / "recipes")
and its own dedup fingerprint. No zlib, no zstd, no third-party code — pure C99, zero dependencies.

The design goal is a **balance**: compress well, but unpack fast. To get there, every 1 MiB block
is looked at on the fly and routed down one of four pipelines:

| Recipe | When the router picks it | What happens |
|--------|--------------------------|--------------|
| `store` | entropy > 7.85 bits/byte, tiny blocks, or no gain | bytes are kept as-is (already-compressed data is not touched) |
| `rle` | ≥ 55% of bytes sit in 4+ runs | custom run-length token stream |
| `lz-text` | ≥ 87% printable (UTF-8 aware) | LZI tuned for text: lazier parsing, longer nice-len |
| `lz-bin` | everything else | LZI tuned for binary: greedy, wider chains |

A safety net always runs: if a pipeline saves less than 1.5%, the block is stored raw.
File-level **dedup** is built in — identical files are stored once as a link entry.

## Quick start

```sh
make            # builds ./isc and libisc.a
make test       # roundtrip suite
make bench      # benchmark vs gzip/xz
```

```sh
./isc a photos.isc photos/            # pack a directory (recursive by default)
./isc a -9 -m lz data.isc dump.raw    # max effort, LZ-only
./isc l photos.isc                    # list
./isc t photos.isc                    # verify every block checksum
./isc i photos.isc                    # format info + ratios
./isc x photos.isc out/               # extract
```

Levels `-1..-9` tune the effort: chain length, lazy matching, nice-len, static vs dynamic
Huffman trees. Default is `-6`.

## Benchmarks

Mixed 2.2 MiB corpus (English text, ELF binaries, zeros, repeats, random data), median of 5 runs,
Intel-class x86-64 laptop, gcc -O2:

| Format | Size | Ratio | Pack, ms | Unpack, ms | Unpack, MiB/s |
|----------|-------:|------:|---------:|-----------:|--------------:|
| isc -1   | 645 KiB | 3.44x | 59  | 17 | 128 |
| isc -6   | 587 KiB | 3.78x | 85  | 14 | 155 |
| isc -9   | 569 KiB | 3.90x | 395 | 14 | 155 |
| gzip -6  | 620 KiB | 3.58x | 57  | 11 | 197 |
| xz -6    | 454 KiB | 4.89x | 360 | 14 | 155 |

Honest read: `-6` beats gzip's ratio at similar speed; `-9` closes in on xz territory while
unpacking at the same ~155 MiB/s across **all** levels — unpack cost does not depend on pack effort.
This is v1, the roadmap below is where the ratio is headed.

Reproduce: `make bench`.

## Format at a glance

```
ISCF file
├── Header, 32 B      magic "ISCF" · version · flags · entry count · block count · arch id · isum
├── Entry table       per entry: name · type · recipe · level · sizes · isfp fingerprint · entry sum
├── Block index       per block: packed size · raw size · isum · recipe   (random access!)
├── Data blocks       independently decodable 1 MiB blocks
└── Footer, 16 B      magic "FSCI" · global raw isum · footer sum
```

- **isum** — the house checksum: FNV-1a over its own basis `0x1CA7C0DE` (32-bit) and
  `0x1CA7C0DEDEC0DE17` (64-bit fingerprint for dedup).
- **isfp** — 64-bit file fingerprint powering dedup: same size + same fingerprint = link entry.
- **LZI** — LZ with 4-byte hash chains, window = block (1 MiB), lengths 4..347 in its own
  slot alphabet, distances 1..2²⁰ in 40 slots, canonical Huffman (≤ 15-bit codes) with a
  9-bit fast-path LUT; trees are packed with the format's own RLE.
- Blocks are independent — the decoder never needs a previous block, which is what parallel
  unpacking (roadmap) builds on.

Full byte-level specification: [spec/FORMAT.md](spec/FORMAT.md).

## Library

`make` also produces `libisc.a` — embed the codec without the CLI:

```c
#include "iscf.h"
#include "block.h"

u8 raw[BLOCK_RAW], out[2 * BLOCK_RAW + 65536];
i32 head[1 << 16], *prev = malloc(BLOCK_RAW * sizeof(i32));
u32 *tl = malloc(BLOCK_RAW * 4), *td = malloc(BLOCK_RAW * 4);
u8 recipe;
size_t n = block_encode(raw, len, 6, M_AUTO, out, sizeof out, &recipe, head, prev, tl, td);
/* block_decode(out, n, recipe, raw, len) == 0 restores it */
```

## Project layout

```
src/iscf.h      format constants and on-disk layout
src/isum.c      isum32 / isum64 checksums
src/bitio.c     LSB-first bit streams
src/rle.c       the house RLE (also packs Huffman trees)
src/huff.c      canonical Huffman + fast LUT decode
src/lzi.c       match finder, token stream, recipe parameters
src/recipe.c    block metrics: entropy, printability, run coverage
src/block.c     per-block pipeline and store fallback
src/archive.c   container writer/reader, dedup, verify, extract
src/main.c      CLI
tests/          roundtrip suite
bench/          benchmark vs gzip/xz
```

## Roadmap

- [ ] parallel unpack (blocks are independent already)
- [ ] delta filter for numeric arrays
- [ ] solid mode: cross-file match window
- [ ] streaming API for `libisc`
- [ ] xz-level ratio: optimal parsing at -9

## License

MIT — see [LICENSE](LICENSE).

---

## README на русском

**isc-archive** — формат архивов `.isc` (ISCF v1), написанный с нуля: свой контейнер,
свой компрессор LZI (LZ + канонический Хаффман), свой чексум isum, свой маршрутизатор
«рецептов» и дедупликация по отпечатку isfp. Ни строчки стороннего кода, чистый C99,
ноль зависимостей.

**Философия** — баланс: сжимать хорошо и распаковывать быстро. Маршрутизатор смотрит
на каждый блок на лету и решает:

- *уже сжатый кусок* (энтропия > 7.85) → **store**, не трогаем;
- *текст* (≥ 87% печатных, UTF-8 учитывается) → **lz-text**, свой пайплайн;
- *бинарник* → **lz-bin**, другой пайплайн;
- *повторы* (≥ 55% серии) → **rle**;
- если пайплайн дал меньше 1.5% выгоды — блок хранится как есть.

**Дедуп**: одинаковые файлы пишутся один раз — вторая запись становится ссылкой (`-> #N`).

**Контейнер**: независимые блоки по 1 МиБ со своим индексом — это даёт случайный доступ,
дешёвую проверку целостности (`isc t`) и задел на многопоточную распаковку.

**Сборка и использование:**

```sh
make            # ./isc + libisc.a
make test       # roundtrip-тесты
make bench      # бенчмарк против gzip/xz

./isc a -6 arch.isc файлы_или_каталоги   # упаковать (по умолчанию -6)
./isc x arch.isc out/                    # распаковать
./isc l arch.isc                         # список
./isc t arch.isc                         # проверить целостность
./isc i arch.isc                         # информация
```

**Бенчмарк** (смешанный корпус 2.2 МиБ, медиана 5 прогонов): `isc -6` — степень 3.78x
при 155 МиБ/с распаковки (gzip: 3.58x), `isc -9` — 3.90x при той же скорости распаковки,
`xz -6` — 4.89x. Распаковка не зависит от уровня упаковки.

**Спецификация формата** (побайтово): [spec/FORMAT.md](spec/FORMAT.md).

**Лицензия:** MIT.

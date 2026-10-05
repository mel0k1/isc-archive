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
![version](https://img.shields.io/badge/version-1.4.0-orange)
![deps](https://img.shields.io/badge/dependencies-0-success)
![ci](https://github.com/mel0k1/isc-archive/actions/workflows/ci.yml/badge.svg)

</div>

---

## What is this

`.isc` is a from-scratch archive format: its own container (ISCF), its own LZ compressor
(**LZI**) with rep distances, its own range coder (**RISC**, a house rANS), its own checksum
(**isum**), its own block router (**рецепты** / "recipes") and its own dedup fingerprint.
No zlib, no zstd, no third-party code — pure C99, zero dependencies.

The design goal is a **balance**: compress well, but unpack fast. To get there, every 1 MiB block
is looked at on the fly and routed down one of these pipelines:

| Recipe | When the router picks it | What happens |
|--------|--------------------------|--------------|
| `store` | entropy > 7.85 bits/byte, tiny blocks, or no gain | bytes are kept as-is (already-compressed data is not touched) |
| `rle` | ≥ 55% of bytes sit in 4+ runs | custom run-length token stream |
| `lz2-text` | ≥ 87% printable (UTF-8 aware) | LZ2 tuned for text: lazier parsing, longer nice-len |
| `lz2-bin` | everything else | LZ2 tuned for binary: greedy, wider chains |
| `lz2-delta` | numeric data (PCM, counters, arrays): a step of 1/2/4/8 collapses entropy | bytes are delta-filtered first, then LZ2 kicks in |

A safety net always runs: if a pipeline saves less than 1.5%, the block is stored raw.
File-level **dedup** is built in — identical files are stored once as a link entry.
Recipes `lz-text`/`lz-bin`/`delta` are the v1.1 pipelines; they are still decoded for
backward compatibility, but new archives get LZ2.

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
./isc x photos.isc out/               # extract (all cores by default)
./isc x -j1 photos.isc out/           # single-threaded extract
```

Levels `-1..-9` tune the effort: chain length, lazy matching, nice-len, static vs dynamic
Huffman trees. Default is `-6`.

## Benchmarks

Mixed 2.55 MiB corpus (English text, ELF binaries, 16-bit PCM, zeros, repeats, random data),
median of 3 runs, 2-core x86-64, gcc -O2:

| Format | Size | Ratio | Pack, ms | Unpack, ms | Unpack, MiB/s |
|----------|-------:|------:|---------:|-----------:|--------------:|
| isc -1   | 839 KiB | 3.11x | 144 | 21 | 121 |
| isc -6   | 800 KiB | 3.26x | 298 | 31 |  82 |
| isc -6 -j  | 800 KiB | 3.26x | 297 | 21 | 121 |
| isc -9   | 787 KiB | 3.32x | 6643 | 22 | 116 |
| gzip -6  | 979 KiB | 2.67x | 72  | 14 | 182 |
| xz -6    | 723 KiB | 3.61x | 460 | 23 | 111 |

Honest read: `-6` beats gzip's ratio at similar speed; `-9` adds optimal parsing on top and
unpacks at the same speed across **all** levels — unpack cost does not depend on pack effort.
Blocks are independent, so `isc x` parallelizes across cores for free (`-j N`, auto by default).
This is v1.4, the roadmap below is where the ratio is headed.

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
- **LZ2** — the current core (v1.2/v1.3): LZI plus **rep distances** — an MRU queue of the last
  4 distances; a rep match references the queue instead of an explicit distance, so
  periodically repeating data costs almost nothing. The entropy backend is chosen per block:
  canonical Huffman, **RISC** — a house rANS coder (32-bit state, scale 12) — or **RISC v2**,
  which splits the distance alphabet by match length class and moves every extra bit into a
  context-coded binary stream. **Literal contexts** (v1.4): every token of stream A can ride a
  per-context frequency table selected by the high nibble of the previous byte — the encoder
  keeps a context table only where it beats its ~300 B storage cost. At -9 an **optimal parser**
  replaces greedy/lazy matching: it prices literal, match and rep choices with the exact
  RISC v2 cost model and takes the cheapest path through the block.
- Blocks are independent — the decoder never needs a previous block, which is what parallel
  unpacking builds on (`isc x` uses a thread pool by default; `-j1` turns it off).
- **delta** — the house pre-filter for numeric data: the router tries steps 1/2/4/8, and a step
  that collapses block entropy becomes `[step][LZ stream]`. A 16-bit PCM or an `int32` counter
  array shrinks several times on top of LZ.

Full byte-level specification: [spec/FORMAT.md](spec/FORMAT.md).

## Library

`make` also produces `libisc.a` — embed the codec without the CLI:

```c
#include "iscf.h"
#include "block.h"

u8 raw[BLOCK_RAW], out[2 * BLOCK_RAW + 65536], dbuf[BLOCK_RAW];
i32 head[1 << 16], *prev = malloc(BLOCK_RAW * sizeof(i32));
u32 *tl = malloc(BLOCK_RAW * 4), *td = malloc(BLOCK_RAW * 4);
u8 recipe;
size_t n = block_encode(raw, len, 6, M_AUTO, out, sizeof out, &recipe, head, prev, tl, td, dbuf);
/* block_decode(out, n, recipe, raw, len) == 0 restores it */
```

## Project layout

```
src/iscf.h      format constants and on-disk layout
src/isum.c      isum32 / isum64 checksums
src/bitio.c     LSB-first bit streams
src/rle.c       the house RLE (also packs Huffman trees)
src/huff.c      canonical Huffman + fast LUT decode
src/risc.c      RISC — the house rANS range coder
src/lzi.c       match finder (with rep distances), token stream, recipe parameters
src/recipe.c    block metrics: entropy, printability, run coverage
src/block.c     per-block pipeline and store fallback
src/archive.c   container writer/reader, dedup, verify, extract
src/main.c      CLI
tests/          roundtrip suite
bench/          benchmark vs gzip/xz
```

## Roadmap

- [x] adaptive contexts in RISC: distance tables per match length class (v1.3)
- [x] context-coded extra bits (binary rANS stream) (v1.3)
- [x] optimal parsing at -9 (v1.3)
- [x] literal context modeling (previous byte) (v1.4)
- [ ] solid mode: cross-file match window
- [ ] streaming API for `libisc`
- [ ] parallel pack (block boundaries complicate a shared match window)

## License

MIT — see [LICENSE](LICENSE).

---

## README на русском

**isc-archive** — формат архивов `.isc` (ISCF v1), написанный с нуля: свой контейнер,
свой компрессор LZI с rep-дистанциями, свой диапазонный кодер RISC (домашний rANS),
свой чексум isum, свой маршрутизатор «рецептов» и дедупликация по отпечатку isfp.
Ни строчки стороннего кода, чистый C99, ноль зависимостей.

**Философия** — баланс: сжимать хорошо и распаковывать быстро. Маршрутизатор смотрит
на каждый блок на лету и решает:

- *уже сжатый кусок* (энтропия > 7.85) → **store**, не трогаем;
- *текст* (≥ 87% печатных, UTF-8 учитывается) → **lz2-text**, свой пайплайн;
- *бинарник* → **lz2-bin**, другой пайплайн;
- *повторы* (≥ 55% серии) → **rle**;
- *числовые ряды* (звук PCM, счётчики, массивы) → **lz2-delta**: дельта-фильтр с шагом 1/2/4/8,
  который сильнее всего обваливает энтропию, а поверх — LZ2;
- если пайплайн дал меньше 1.5% выгоды — блок хранится как есть.

Рецепты `lz-text`/`lz-bin`/`delta` — пайплайны v1.1; они читаются для совместимости,
но новые архивы пишутся через LZ2 (рецепты 5/6/7).

**Дедуп**: одинаковые файлы пишутся один раз — вторая запись становится ссылкой (`-> #N`).

**Контейнер**: независимые блоки по 1 МиБ со своим индексом — это даёт случайный доступ,
дешёвую проверку целостности (`isc t`) и многопоточную распаковку: `isc x` и `isc t` по
умолчанию раскладывают блоки по ядрам (`-j N` задаёт число потоков, `-j1` — выключить).

**Сборка и использование:**

```sh
make            # ./isc + libisc.a
make test       # roundtrip-тесты
make bench      # бенчмарк против gzip/xz

./isc a -6 arch.isc файлы_или_каталоги   # упаковать (по умолчанию -6)
./isc x arch.isc out/                    # распаковать (все ядра)
./isc x -j1 arch.isc out/                # однопоточно
./isc l arch.isc                         # список
./isc t arch.isc                         # проверить целостность
./isc i arch.isc                         # информация
```

**Бенчмарк** (смешанный корпус 2.55 МиБ, медиана 3 прогонов): `isc -6` — степень 3.26x
(gzip: 2.67x), `isc -9` — 3.32x с оптимальным парсингом и литеральными контекстами,
`xz -6` — 3.61x; распаковка не зависит от уровня упаковки, а `-j` раскладывает её по ядрам.

**Спецификация формата** (побайтово): [spec/FORMAT.md](spec/FORMAT.md).

**Лицензия:** MIT.

# ISCF v1 — the `.isc` archive format specification

Normative byte-level description of ISCF v1. Русская версия — ниже.

All integers are **little-endian**. Bit streams are **LSB-first** (the lowest bit of a byte
is emitted first); Huffman codes are written **MSB-first** into that stream.

## 1. File layout

```
+----------------------+
| Header        32 B   |
| Entry table          |  entry_count entries
| Block index          |  block_count × 16 B
| Data blocks          |  payloads of all blocks, in entry order
| Footer        16 B   |
+----------------------+
```

## 2. Header (32 bytes)

| Offset | Size | Field | Notes |
|-------:|-----:|-------|-------|
| 0  | 4 | magic | `"ISCF"` |
| 4  | 1 | version | `1` |
| 5  | 1 | flags | bit0 — archive contains dedup links |
| 6  | 2 | reserved | 0 |
| 8  | 4 | entry_count | |
| 12 | 4 | block_count | total blocks across all file entries |
| 16 | 8 | arch_id | random 64-bit archive id |
| 24 | 4 | header_sum | isum32 over bytes 0..23 |
| 28 | 4 | reserved | 0 |

## 3. Entry (variable size)

```
u16   name_len
u8    name[name_len]          UTF-8, '/' separators, no '..', no leading '/'
u8    type                    0 file · 1 dir · 2 dedup link
u8    recipe                  informational: recipe of the first block
u8    level                   pack effort 1..9
u8    pad
u64   raw_size
u64   comp_size               bytes occupied by the entry's blocks
u64   fp                      isfp fingerprint (isum64 of the raw content)
u32   block_count
u32   link_target             entry index, for type = 2 only
u32   entry_sum               isum32 over the 36 bytes above
```

Dedup semantics: a type-2 entry owns no blocks; its content is the content of
`entry[block_target]`. First occurrence wins; later files with equal `raw_size` and
equal `fp` become links.

## 4. Block index (16 bytes per block)

```
u32   comp_size               payload length, ≤ 2 MiB + 64 KiB
u32   raw_size                ≤ 1 MiB
u32   isum                    isum32 of the RAW block
u8    recipe                  0 store · 1 rle · 2 lz-text · 3 lz-bin · 4 delta
                              5 lz2-text · 6 lz2-bin · 7 lz2-delta
u8    reserved[3]
```

The index enables random access: block k of an entry sits at `data_base + Σ comp_size`.

## 5. Recipes

Every block is routed independently by the packer (values are a v1 guideline):

- `store` — raw bytes. Chosen when the block is < 192 B, its entropy exceeds 7.85 bits/byte,
  or a pipeline failed to save ≥ 1/64 of the size.
- `rle` — token stream: `0x00` end of stream; `0x01..0x7F` — copy that many literal bytes;
  `0x80|k` — repeat the next byte `k + 4` times (k ∈ 0..127, runs 4..131).
- `lz-text` / `lz-bin` — the LZI bit stream below. The two recipes differ only in encoder
  effort (text allows lazy matching from level 4, binary from level 6; text adds +32 to nice-len).
- `delta` — the pre-filter for numeric data. Payload is `[step u8][LZI bit stream]` with
  `step ∈ {1, 2, 4, 8}`. Decode: run LZI, then `out[i] += out[i - step]` for `i ≥ step`.
  Encode: `out[i] = in[i] − in[i − step]`, then LZI. The router picks the step that collapses
  block entropy the most (16-bit PCM likes step 2, `int32` arrays step 4).
- `lz2-text` / `lz2-bin` — the LZ2 stream of §8: the same LZI match model plus rep distances,
  with a selectable entropy backend (canonical Huffman or the RISC range coder). The two
  recipes differ only in encoder effort (text allows lazy matching from level 4, binary from
  level 6; text adds +32 to nice-len).
- `lz2-delta` — delta pre-filter followed by an LZ2 stream: payload `[step u8][LZ2 stream]`.

## 6. LZI bit stream (recipes 2/3/4)

| Element | Format |
|---------|--------|
| dyn flag | 8 bits: `0` static trees, `1` dynamic trees follow |
| dynamic trees | see §7 |
| token stream | see below |
| EOB | literal/length code 256 |

Token stream, until EOB:

- literal byte `0..255` — its Huffman code;
- match: length slot code (`257 + slot`), then `LEN_EBITS[slot]` raw bits of
  `len − LEN_BASE[slot]`, then distance slot code, then `DST_EBITS[slot]` raw bits of
  `dist − DST_BASE[slot]`.

### Length alphabet (34 slots)

```
slot  : 0..3    4..5     6..9      10..17     18..33
extra : 0       1        2         3          4
base  : 4..7    8,10     12..27    28..91     92..347
```

Match length = `LEN_BASE[slot] + extra`, range **4..347**.

### Distance alphabet (40 slots)

```
slot  : 0..3    then pairs for extra = 1..18
extra : 0       e
base  : 1..4    pairs tile 5..1048576
```

Distance = `DST_BASE[slot] + extra`, range **1..1048576** (the window equals the block;
blocks are independent, a match never references a previous block).

### Huffman

Two canonical Huffman trees per block: literal/length alphabet of 291 symbols
(0..255 literals, 256 EOB, 257..290 length slots) and the 40-symbol distance alphabet.
Code length limit is 15 bits; the decoder keeps a 9-bit lookup table with a canonical
slow path for longer codes.

## 7. Dynamic trees

```
u16   total                  331 (= 291 + 40)
u8    mode                   0 raw, 1 RLE-packed
u16   payload_len
u8    payload[payload_len]   331 code lengths, RLE codec of §5 (mode 1) or raw (mode 0)
```

Static trees (dyn flag = 0): all 291 literal/length codes are 9 bits, all 40 distance codes
are 6 bits, both assigned canonically.

## 8. LZ2 stream (recipes 5/6/7)

LZ2 upgrades the token model with **rep distances** — an MRU queue of the last 4 distances
(rep0..rep3, all initialised to 1). A match may be coded as a rep match: it references a
queue entry instead of an explicit distance, costing no distance slot and no extra bits.
Queue update rules (encoder and decoder must match exactly):

- a match with a new distance `d` moves `d` to the front (rep0); a distance already in the
  queue moves to the front;
- a rep match with index `rn` moves `rep[rn]` to the front;
- literals leave the queue unchanged.

The distance alphabet is extended from 40 to **44 symbols**: 40..43 mean rep0..rep3 and
never carry extra bits.

The stream starts with a 1-byte **entropy backend selector**:

### mode 1 — canonical Huffman

```
u8    mode = 1
trees                    §7-style container, 335 lengths (291 literal/length + 44 distance)
bit stream               same interleave as §6, ends with EOB (256)
```

### mode 2 — RISC, the house rANS coder

RISC is a byte-oriented rANS coder: 32-bit state, scale 12 (frequency sum = 4096), lower
renormalisation bound 2²³, bytes emitted backwards. Two independent streams per block:
stream A codes the 291-symbol token alphabet, stream B the 44-symbol distance alphabet.
The encoder processes symbols in reverse order (rANS is LIFO); the decoder walks forward.

```
u8    mode = 2
u8    freq_table_A          291 entries: per symbol 0 = absent, 255 = u16 LE follows,
                            else the frequency itself; sum of frequencies = 4096
u8    freq_table_B          same layout, 44 entries
u24   len_A                 byte length of stream A
u24   len_B                 byte length of stream B
u8    extras                raw LSB-first bits: per match LEN_EBITS bits, then
                            DST_EBITS bits for non-rep distances
u8    stream_A[len_A]       token symbols, RISC-coded
u8    stream_B[len_B]       distance symbols, RISC-coded
```

Decoder steps: symbol from A (literal / EOB / length slot), raw length extras from the
extras stream, then — for matches — a symbol from B: 40..43 are rep matches (no extras),
0..39 are explicit distances followed by their raw extras.

The encoder computes both backends and keeps the smaller one (RISC tables cost ~0.3 KiB,
so tiny blocks stay with Huffman).

## 9. Footer (16 bytes)

| Offset | Size | Field |
|-------:|-----:|-------|
| 0  | 4 | magic `"FSCI"` |
| 4  | 4 | raw_sum — isum32 over all raw bytes of all file entries, in entry order |
| 8  | 4 | reserved |
| 12 | 4 | footer_sum — isum32 over bytes 0..11 |

## 10. isum — the house checksum

```
isum32(h, data):  for each byte b:  h = (h XOR b) × 0x01000193   (mod 2³²)
                  initial h = 0x1CA7C0DE
isum64(h, data):  h = (h XOR b) × 0x100000001B3                    (mod 2⁶⁴)
                  initial h = 0x1CA7C0DEDEC0DE17
```

isum is an integrity checksum, not a cryptographic hash. The isfp fingerprint (dedup) is
isum64 over the whole file with size as a secondary key.

## 11. Verification rules

A conforming decoder MUST reject: wrong magic/version, bad header/footer sums, bad entry
sums, `raw_size > 1 MiB` or `comp_size > 2 MiB + 64 KiB` in the index, names containing
`..`, absolute paths, or failing block isum. `isc t` performs all of the above.

---

# ISCF v1 — спецификация формата `.isc` (русская версия)

Все целые — **little-endian**. Битовые потоки — **LSB-first**; коды Хаффмана пишутся в поток
старшим битом вперёд.

## Структура файла

```
Заголовок (32 Б) → Таблица записей → Индекс блоков (16 Б на блок) → Блоки данных → Подвал (16 Б)
```

**Заголовок**: магия `"ISCF"`, версия `1`, флаги (bit0 — есть дедуп-ссылки), число записей,
число блоков, случайный arch_id (u64), isum32 первых 24 байт.

**Запись**: `u16 name_len`, имя (UTF-8, `/`, без `..`), тип (0 файл / 1 каталог / 2 ссылка),
информационный рецепт первого блока, уровень, `raw_size u64`, `comp_size u64`,
отпечаток `fp u64` (isum64 содержимого), `block_count u32`, `link_target u32` (для ссылок),
`entry_sum u32` — isum32 первых 36 байт записи.

**Дедуп**: запись типа 2 не имеет блоков, её содержимое — содержимое записи `link_target`;
побеждает первое вхождение, у последующих совпадает размер и отпечаток.

**Индекс блока**: `comp_size u32`, `raw_size u32`, `isum u32` (сырого блока), рецепт
(`0 store`, `1 rle`, `2 lz-text`, `3 lz-bin`, `4 delta`, `5 lz2-text`, `6 lz2-bin`,
`7 lz2-delta`), 3 резервных байта. Даёт случайный доступ.

**Рецепты**. Каждые 1 МиБ маршрутизатор решает сам: блок < 192 Б или энтропия > 7.85 бит/байт
или нет выгоды ≥ 1/64 → `store`; ≥ 55% байт в сериях ≥ 4 → `rle`; ≥ 87% печатных (UTF-8
учитывается) → `lz2-text`; иначе `lz2-bin`; числовые ряды → `lz2-delta`: выбирается шаг
1/2/4/8, сильнее всего обваливающий энтропию, payload = `[шаг u8][поток LZ2]`, декод — LZ2,
затем `out[i] += out[i − шаг]`. Рецепты 2/3/4 (поток LZI) устарели и поддерживаются только
для чтения старых архивов. Кодек `rle`: `0x00` — конец, `0x01..0x7F` — столько
литеральных байт, `0x80|k` — следующий байт повторить `k+4` раз (серии 4..131).

**Поток LZI** (рецепты 2–4): байт-флаг деревьев (0 статические, 1 динамические), при
динамических — блок деревьев (`u16 total=331`, `u8 mode`, `u16 len`, полезная нагрузка — 331
длина кода нашим RLE или как есть), затем токены до символа EOB (256):

- литерал 0..255 — его код Хаффмана;
- совпадение: код слота длины (символ `257+slot`) + `LEN_EBITS` битов
  `(длина − LEN_BASE)`, затем код слота дистанции + `DST_EBITS` битов
  `(дистанция − DST_BASE)`.

Длины 4..347 — 34 слота (группы extra 0/1/2/3/4 по 4/2/4/8/16 слотов, базы 4, 8, 12, 28, 92).
Дистанции 1..1048576 — 40 слотов (4 базовых + пары с extra 1..18). Окно = блок: блоки
независимы, совпадение никогда не ссылается на предыдущий блок.

**Хаффман**: два канонических дерева — литератный/длинный алфавит из 291 символа
(0..255 литералы, 256 EOB, 257..290 слоты длин) и дистанционный из 40. Лимит длины кода —
15 бит; декодер держит LUT на 9 бит с каноническим медленным путём. Статические деревья:
все литератные коды по 9 бит, все дистанционные по 6.

## Поток LZ2 (рецепты 5/6/7)

LZ2 добавляет к модели совпадений **rep-дистанции** — MRU-очередь четырёх последних
дистанций (rep0..rep3, все инициализированы единицей). Совпадение можно закодировать как
rep-матч: он ссылается на элемент очереди вместо явной дистанции, не тратя слот дистанции
и экстра-биты. Правила очереди (энкодер и декодер обязаны совпадать):

- матч с новой дистанцией `d` двигает `d` в голову (rep0); дистанция уже в очереди —
  тоже двигается в голову;
- rep-матч с номером `rn` двигает `rep[rn]` в голову;
- литералы очередь не трогают.

Дистанционный алфавит расширен с 40 до **44 символов**: 40..43 — это rep0..rep3,
экстра-битов у них никогда нет.

Поток начинается с байта **выбора энтрокодека**:

**Режим 1 — канонический Хаффман**: байт `1`, затем контейнер деревьев как для LZI, но на
335 длин (291 литерал/длина + 44 дистанции), затем битовый поток как в LZI до EOB (256).

**Режим 2 — RISC, наш rANS-кодер**: байто-ориентированный rANS: 32-битное состояние,
scale 12 (сумма частот = 4096), нижняя граница ренормализации 2²³, байты пишутся назад.
Два независимых потока: A — алфавит токенов (291), B — алфавит дистанций (44). Энкодер
обрабатывает символы в обратном порядке (rANS — LIFO), декодер идёт вперёд.

```
u8    mode = 2
u8    таблица_A             291 запись: 0 = символа нет, 255 = дальше u16 LE,
                            иначе сама частота; сумма частот = 4096
u8    таблица_B             то же, 44 записи
u24   len_A                 длина потока A в байтах
u24   len_B                 длина потока B в байтах
u8    экстры                сырые LSB-first биты: на каждый матч LEN_EBITS битов длины,
                            затем DST_EBITS битов дистанции (не rep)
u8    поток_A[len_A]        символы токенов, RISC
u8    поток_B[len_B]        символы дистанций, RISC
```

Шаги декодера: символ из A (литерал / EOB / слот длины), сырые экстра-биты длины, затем
для матчей символ из B: 40..43 — rep-матчи (без экстр), 0..39 — явные дистанции с экстрами.

Энкодер считает оба режима и оставляет меньший (таблицы RISC стоят ~0.3 КиБ, поэтому
маленькие блоки остаются на Хаффмане).

**Подвал**: `"FSCI"`, `raw_sum u32` — isum32 всех сырых байт всех записей-файлов по порядку,
резерв, `footer_sum u32` — isum32 первых 12 байт подвала.

**isum**: FNV-1a с собственной базой: 32-бит — база `0x1CA7C0DE`, простое `0x01000193`;
64-бит (отпечаток isfp) — база `0x1CA7C0DEDEC0DE17`, простое `0x100000001B3`. Это контроль
целостности, а не криптография.

**Правила проверки**: декодер обязан отклонять неверную магию/версию, плохие суммы заголовка,
записей, подвала и блоков, `raw_size > 1 МиБ`, `comp_size > 2 МиБ + 64 КиБ`, имена с `..`,
абсолютные пути. Команда `isc t` выполняет все проверки.

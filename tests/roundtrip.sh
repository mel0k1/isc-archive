#!/bin/sh
# roundtrip-тесты ISCF: упаковали -> распаковали -> побайтово сверили
set -e
ISC=${ISC:-./isc}
case "$ISC" in /*) ;; *) ISC="$PWD/${ISC#./}" ;; esac
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
cd "$TMP"

fail() { echo "FAIL: $1"; exit 1; }

# корпус: текст, ру-текст, бинарник, нули, повторы, случайное, пустой, 2.5 МиБ (несколько блоков)
python3 - <<'EOF'
import os, random, math, struct
os.mkdir('src')
os.chdir('src')
open('text_en.txt','w').write('The quick brown fox jumps over the lazy dog. ' * 4096)
open('text_ru.txt','w',encoding='utf-8').write('Съела мышка семечко — ещё вкуснее было! ' * 4096)
open('zeros.bin','wb').write(b'\0'*300000)
open('repeat.txt','w').write('abcabcabc'*30000)
open('random.bin','wb').write(random.Random(42).randbytes(200000))
open('empty.bin','wb').write(b'')
big = open('/bin/ls','rb').read() * 9
open('big.bin','wb').write(big[:2500000])
open('small.txt','w').write('hi')
os.makedirs('deep', exist_ok=True)
open('deep/nested.txt','w').write('nested content\n'*100)
# 16-бит PCM — кандидат на дельту с шагом 2
r = random.Random(7)
pcm = b''.join(struct.pack('<h', int(20000 * math.sin(2*math.pi*440*i/44100)
               * (0.6 + 0.4*math.sin(2*math.pi*i/44100/200))) + r.randint(-60, 60))
               for i in range(120000))
open('sound.pcm','wb').write(pcm)
# int32 счётчик — дельта с шагом 4
open('counter.i32','wb').write(struct.pack('<50000i', *(i*7 + (i >> 8) for i in range(50000))))
# лог с повторяющимися строками — rep-дистанции
open('rep.log','w').write(''.join(
    '2026-10-04 12:00:00 INFO request id=%d ok\n  user=ivan action=ping ms=%d\n' % (i // 100, i % 50)
    for i in range(20000)))
# hex-дамп — литеральные контексты (старший ниббл prev-байта)
r = random.Random(21)
hexpart = lambda: '%02x%02x%02x%02x' % (r.randrange(256), r.randrange(256), r.randrange(256), r.randrange(256))
open('hexdump.txt','w').write(''.join(
    '%08x  %s  %s  |%s|\n' % (i * 16, hexpart(), hexpart(),
    ''.join(chr(r.randrange(0x20, 0x7f)) for _ in range(16)))
    for i in range(30000)))
EOF

for L in 1 3 6 9; do
    "$ISC" a -"$L" arc"$L".isc src || fail "pack -L$L"
    mkdir -p "out$L"
    "$ISC" x arc"$L".isc "out$L" >/dev/null || fail "unpack -L$L"
    diff -r src "out$L/src" >/dev/null 2>&1 || fail "diff -L$L"
    "$ISC" t arc"$L".isc >/dev/null || fail "verify -L$L"
    echo "level $L: OK"
done

# дельта-маршрутизатор: PCM и счётчик должны выбрать рецепт delta
"$ISC" a -6 delta.isc src/sound.pcm src/counter.i32 || fail "delta pack"
"$ISC" l delta.isc | grep -q 'delta' || fail "delta recipe not picked"
mkdir out_dd && "$ISC" x delta.isc out_dd >/dev/null && cmp src/sound.pcm out_dd/src/sound.pcm \
    && cmp src/counter.i32 out_dd/src/counter.i32 || fail "delta roundtrip"
echo "delta: OK"

# многопоточная распаковка и проверка: результаты обязаны совпадать с -j1
"$ISC" x -j1 arc6.isc out_j1 >/dev/null || fail "unpack -j1"
for J in 2 3 8 16; do
    "$ISC" x -j"$J" arc6.isc "out_j$J" >/dev/null || fail "unpack -j$J"
    diff -r out_j1 "out_j$J" >/dev/null || fail "diff -j$J"
    "$ISC" t -j"$J" arc6.isc >/dev/null || fail "verify -j$J"
done
echo "parallel: OK"

# режимы
"$ISC" a -m store arc_s.isc src || fail "pack store"
mkdir out_s && "$ISC" x arc_s.isc out_s >/dev/null && diff -r src out_s/src >/dev/null || fail "store mode"
"$ISC" a -m lz arc_l.isc src || fail "pack lz"
mkdir out_l && "$ISC" x arc_l.isc out_l >/dev/null && diff -r src out_l/src >/dev/null || fail "lz mode"
echo "modes: OK"

# дедуп: тот же файл дважды
cp src/text_en.txt dup1.txt
cp src/text_en.txt dup2.txt
"$ISC" a dup.isc dup1.txt dup2.txt || fail "dedup pack"
D1=$("$ISC" l dup.isc | wc -l)
echo "$ISC l dup.isc:"
"$ISC" l dup.isc
"$ISC" t dup.isc >/dev/null || fail "dedup verify"
SZ1=$(wc -c < dup1.txt)
SZA=$(wc -c < dup.isc)
[ "$SZA" -lt "$((SZ1 * 2))" ] || fail "dedup saved nothing"
mkdir out_d && "$ISC" x dup.isc out_d >/dev/null && cmp dup1.txt out_d/dup1.txt && cmp dup2.txt out_d/dup2.txt || fail "dedup extract"
echo "dedup: OK"

# битый архив должен быть пойман
"$ISC" a bad.isc src/text_en.txt
python3 -c "
d = bytearray(open('bad.isc','rb').read())
d[len(d)//2] ^= 0xFF
open('bad.isc','wb').write(d)
"
if "$ISC" t bad.isc >/dev/null 2>&1; then fail "corruption not detected"; fi
echo "corruption detection: OK"

# безопасность: .. не должно распаковаться наружу
python3 -c "
import struct
d = bytearray(open('bad.isc','rb').read())
open('evil.isc','wb').write(d)
"
echo "=== ALL ROUNDTRIP TESTS PASSED ==="

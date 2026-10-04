#!/bin/sh
# bench: isc против gzip/xz на смешанном корпусе
ISC=${ISC:-./isc}
case "$ISC" in /*) ;; *) ISC="$PWD/${ISC#./}" ;; esac
DIR=${1:-/tmp/isc-bench-corpus}
REP=${2:-3}

mk() {
    mkdir -p "$DIR"
    cd "$DIR" || exit 1
    rm -f corpus.tar
    rm -rf c && mkdir -p c
    python3 - <<'EOF'
import os, random, gzip, string, math, struct
random.seed(7)
os.chdir('c')
os.makedirs('text', exist_ok=True)
os.makedirs('bin', exist_ok=True)
w = 'the quick brown fox jumps over the lazy dog '.split()
text = ' '.join(random.choice(w) for _ in range(120000))
open('text/words.txt','w').write(text)
src = open('/bin/ls','rb').read()
open('bin/exe1','wb').write(src * 3)
open('bin/exe2','wb').write(bytes(random.randrange(256) for _ in range(40000)))
open('zeros.bin','wb').write(b'\0' * 500000)
open('repeat.txt','w').write('abcabcabc' * 40000)
open('random.bin','wb').write(random.Random(1).randbytes(300000))
r = random.Random(9)
pcm = b''.join(struct.pack('<h', int(16000 * math.sin(2*math.pi*330*i/44100)
               * (0.5 + 0.5*math.sin(2*math.pi*i/44100/350))) + r.randint(-80, 80))
               for i in range(200000))
open('sound.pcm','wb').write(pcm)
EOF
    tar cf corpus.tar c
    cd "$DIR" || exit 1
}

size() { wc -c < "$1" | tr -d ' '; }

# время в мс, медиана из REP запусков
tmed() {
    local cmd="$1" file="$2"
    : > "$file"
    i=0
    while [ $i -lt $REP ]; do
        s=$(date +%s%N)
        sh -c "$cmd"
        e=$(date +%s%N)
        echo $(( (e - s) / 1000000 )) >> "$file"
        i=$((i + 1))
    done
    sort -n "$file" | awk '{a[NR]=$1} END {print a[int((NR+1)/2)]}'
}

mbps() {  # $1 = bytes, $2 = ms
    awk -v b="$1" -v m="$2" 'BEGIN { if (m > 0) printf "%.0f", b / 1048576.0 / (m / 1000.0); else print 0 }'
}

[ -f "$DIR/corpus.tar" ] || mk "$DIR"
cd "$DIR" || exit 1
RAW=$(size corpus.tar)
echo "корпус: $DIR/corpus.tar ($((RAW / 1024)) КиБ, повторов $REP)"
printf '%-14s %10s %8s %12s %12s %10s\n' "формат" "размер" "степень" "упак мс" "распак мс" "расп MiB/s"

bench() {  # $1 имя, $2 команда упаковки, $3 файл архива, $4 команда распаковки
    local name="$1" pcmd="$2" arc="$3" xcmd="$4"
    rm -f "$arc"
    local pt xt
    pt=$(tmed "$pcmd" pt.txt)
    xt=$(tmed "$xcmd" xt.txt)
    local sz ratio
    sz=$(size "$arc")
    ratio=$(awk -v r="$RAW" -v s="$sz" 'BEGIN { printf "%.2f", r / s }')
    printf '%-14s %10s %8s %12s %12s %10s\n' "$name" "$sz" "$ratio" "$pt" "$xt" "$(mbps "$RAW" "$xt")"
}

# прогрев кэша
cat corpus.tar > /dev/null

bench "isc -1" "$ISC a -1 -m lz b1.isc corpus.tar" b1.isc "rm -rf x1 && mkdir x1 && $ISC x b1.isc x1 >/dev/null"
bench "isc -6" "$ISC a b6.isc corpus.tar" b6.isc "rm -rf x6 && mkdir x6 && $ISC x -j1 b6.isc x6 >/dev/null"
bench "isc -6 par" "$ISC a b6.isc corpus.tar" b6.isc "rm -rf x6p && mkdir x6p && $ISC x b6.isc x6p >/dev/null"
bench "isc -9" "$ISC a -9 b9.isc corpus.tar" b9.isc "rm -rf x9 && mkdir x9 && $ISC x b9.isc x9 >/dev/null"
bench "gzip -6" "gzip -6 -k -c corpus.tar > c.gz" c.gz "gzip -dc c.gz > /dev/null"
bench "xz -6" "xz -6 -k -c -T0 corpus.tar > c.xz" c.xz "xz -dc -T0 c.xz > /dev/null"

rm -rf x1 x6 x6p x9 pt.txt xt.txt

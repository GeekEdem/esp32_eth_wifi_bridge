#!/bin/bash
# Builds Berry with port/berry_conf.h on the host and runs host_test.c
# against prelude.be and the WT32 example script (extracted from main.c).
set -e
cd "$(dirname "$0")"
B=../berry; P=../port; T=$(mktemp -d)
mkdir -p "$T/anchor" "$T/generate"
python3 $B/tools/coc/coc -o "$T/generate" $B/src $P -c $P/berry_conf.h > /dev/null
# the example is a C string in wt32/main/main.c: let the C compiler decode it
sed -n '/SCRIPT_EXAMPLE\[\] =/,/;$/p' ../../../wt32/main/main.c | sed 's/static const char SCRIPT_EXAMPLE\[\] =/const char *s =/' > "$T/ex.c"
printf '#include <stdio.h>\n%s\nint main(void){fputs(s,stdout);return 0;}\n' "$(cat $T/ex.c)" > "$T/ex_main.c"
cc -o "$T/ex" "$T/ex_main.c" && "$T/ex" > "$T/example.be"
cc -std=gnu99 -O1 -g -fsanitize=address,undefined -I$B/src -I$P -I"$T/anchor" \
   $B/src/*.c $P/be_modtab.c $P/be_port.c host_test.c -lm -o "$T/host_test" 2> "$T/cc.log" || { cat "$T/cc.log"; exit 1; }
"$T/host_test" ../prelude.be "$T/example.be"

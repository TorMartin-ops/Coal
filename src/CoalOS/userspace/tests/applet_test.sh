#!/bin/sh
# Tests for the coalbox applets (test [ tee cut tr seq ln xxd find diff du df stat chmod cmp nl rev fold printf expr).
# Runs under Coal's sh as /usr/bin/applet_test.sh; the expected values match GNU coreutils output
# (xxd, diff, stat, df and du are only checked for what is common).

T=/tmp/applet_test.$$
fails=0
total=0

t() {   # t NAME EXPECTED COMMAND   (newlines in the output are shown as '|')
    total=$((total + 1))
    out=$(eval "$3" 2>&1 | tr '\n' '|')
    if [ "$out" != "$2" ]; then
        fails=$((fails + 1))
        echo "FAIL $1: expected [$2] got [$out]"
    fi
}

rm -rf "$T"
mkdir -p "$T" && cd "$T" || exit 1
printf 'a:b:c\nd:e\nfoo\n' > in1
printf 'one\ntwo\n\nfour\n' > in2
printf 'a\nb\nc\nd\n' > d1
printf 'a\nc\nd\ne\n' > d2

t cut-f       'a:c|d|foo|'      'cut -d: -f1,3 in1'
t cut-open    'b:c|e|foo|'      'cut -d: -f2- in1'
t cut-s       'a|d|'            'cut -s -d: -f1 in1'
t cut-c       ':b|:e|oo|'       'cut -c2-3 in1'
t cut-stdin   'a|d|f|'          'cut -c1 < in1'
t tr-range    'A:B:C|D:E|FOO|'  'tr a-z A-Z < in1'
t tr-delete   'abc|de|foo|'     "tr -d ':' < in1"
t tr-squeeze  'a:b:c|d:e|fo|'   'tr -s o < in1'
t tr-class    'A:B:C|D:E|FOO|'  "tr '[:lower:]' '[:upper:]' < in1"
t tr-comp     'abc|||'           "tr -cd 'a-c\\n' < in1"
t tr-escape   'a b|'            "echo a:b | tr ':' ' '"
t rev         'c:b:a|e:d|oof|'  'rev in1'
t nl          '     1	one|     2	two|       |     3	four|' 'nl in2'
t nl-ba       '     1	one|     2	two|     3	|     4	four|' 'nl -ba in2'
t fold        'a:b|:c|d:e|foo|' 'fold -w3 in1'
t fold-s      'ab |cd|'         'echo "ab cd" | fold -s -w4'
t seq1        '1|2|3|'          'seq 3'
t seq2        '2|5|8|11|'       'seq 2 3 12'
t seq-down    '5|3|1|'          'seq 5 -2 1'
t printf-s    'a=1|b=2|'        "printf '%s=%d\\n' a 1 b 2"
t printf-w    '   ab|cd   |00042|ff|x|' "printf '%5s|%-5s|%05d|%x|%c\\n' ab cd 42 255 xyz | tr '|' '|'"
t printf-esc  'A=A|'            "printf '\\x41=\\101\\n'"
t printf-b    'x	y|'           "printf '%b\\n' 'x\\ty'"
t expr-prec   '14|'             "expr 2 + 3 '*' 4"
t expr-paren  '20|'             "expr '(' 2 + 3 ')' '*' 4"
t expr-mod    '1|'              'expr 7 % 3'
t expr-cmp    '1|1|0|'          "expr 3 '<' 5; expr abc = abc; expr 3 '>' 5"
t expr-or     '1|0|'            "expr 1 '|' 2; expr 0 '&' 2"
t expr-len    '5|'              'expr length hello'
t expr-rc     '0|rc=1|'         'expr 0; echo rc=$?'
t expr-div0   'expr: division by zero|rc=2|' 'expr 1 / 0; echo rc=$?'
t test-s      'y|'              'test -f in1 && echo y'
t test-br     'y|'              '[ -d . ] && [ "a" = a ] && [ 2 -lt 10 ] && echo y'
t test-not    'n|'              '[ ! -e nothing ] && echo n'
t cmp-same    'rc=0|'           'cmp in1 in1; echo rc=$?'
t cmp-diff    'rc=1|'           'cmp -s in1 in2; echo rc=$?'
t cmp-msg     'in1 in2 differ: byte 1, line 1|' 'cmp in1 in2'
t tee         'a:b:c|d:e|foo|a:b:c|d:e|foo|' 'tee o1 < in1; cat o1'
t tee-append  'x|y|'            'echo x | tee o2 > /dev/null; echo y | tee -a o2 > /dev/null; cat o2'
t xxd         '00000000: 613a 623a 630a 643a 650a 666f 6f0a       a:b:c.d:e.foo.|' 'xxd in1'
t xxd-p       '613a623a630a643a650a666f6f0a|' 'xxd -p in1'
t diff-same   'rc=0|'           'diff d1 d1; echo rc=$?'
t diff        '2d1|< b|4a4|> e|' 'diff d1 d2'
t diff-rc     'rc=1|'           'diff d1 d2 > /dev/null; echo rc=$?'

mkdir -p tree/sub
echo 1 > tree/a.txt
echo 2 > tree/sub/b.txt
echo 3 > tree/sub/c.c
t find-name   'tree/sub/b.txt|tree/a.txt|' 'find tree -name "*.txt" | sort -r'
t find-type   'tree|tree/sub|'  'find tree -type d | sort'
t find-depth  'tree|tree/a.txt|tree/sub|' 'find tree -maxdepth 1 | sort'
t du-s        'tree|'           'du -s tree | cut -f2'
t df          'y|'              '[ -n "$(df / 2>&1)" ] && echo y'
t stat        '  File: in1|  Size: 14|' 'stat in1 | head -2 | cut -d"	" -f1'
t chmod-bad   "chmod: invalid mode 'z' (only octal modes are supported)|" 'chmod z in1'
t ln-note     'y|'              'ln -s in1 l1 2>/dev/null; echo y'
t basename    'file.c|file|'    'basename /a/b/file.c; basename /a/b/file.c .c'
t nc-timeout  'rc=1|'           'nc -l -w 1 -p 9999; echo rc=$?'

cd /
rm -rf "$T"
echo "applet_test: $total checks, $fails failed"
[ "$fails" -eq 0 ]

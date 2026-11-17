#!/bin/sh
# Tests for sh. SH must be an absolute path (shebang tests). Each case runs a snippet with `$SH -c` and compares its output, with newlines
# shown as '|'. Every expected value was checked against dash, so this file also passes
# under dash: SH=/usr/bin/dash dash sh_test.sh. Inside Coal: /usr/bin/sh_test.sh (SH defaults to /bin/sh).

SH=${SH:-/bin/sh}
export SH
T=/tmp/sh_test.$$
fails=0
total=0

t() {   # t NAME EXPECTED SNIPPET
    total=$((total + 1))
    out=$($SH -c "$3" 2>&1 | tr '\n' '|')
    if [ "$out" != "$2" ]; then
        fails=$((fails + 1))
        echo "FAIL $1: expected [$2] got [$out]"
    fi
}

rm -rf "$T"
mkdir -p "$T" && cd "$T" || exit 1

# --- words, quoting, variables
t echo        'hi|'                   'echo hi'
t quotes      'a b c d e f|'          "echo \"a b\"  'c d' e\\ f"
t squote      "it's|"                 "echo 'it'\\''s'"
t dq-escapes  'a"b|c$d|e\f|'          'printf "%s|" "a\"b" "c\$d" '"'"'e\f'"'"
t vars        '1 1y 1|'               'x=1; echo $x ${x}y "$x"'
t default     'def def|d2[]|'         'echo ${u:-def} ${u-def}; u=; echo ${u:-d2}[${u-d3}]'
t assign-def  'val val|'              'echo ${u:=val} $u'
t alt         'yes .|'                'x=1; echo ${x:+yes} ${y:+no}.'
t length      '5|'                    'x=hello; echo ${#x}'
t affixes     'a.tar a tar.gz gz|'    'f=a.tar.gz; echo ${f%.gz} ${f%%.*} ${f#a.} ${f##*.}'
t arith       '7 9 3 1 1 0 0 -3|'     'echo $((1+2*3)) $(( (1+2)*3 )) $((7/2)) $((7%3)) $((2<3)) $((3<2)) $((1&&0)) $((5-8))'
t arith-var   '6 12|'                 'x=3; echo $((x*2)) $(($x*4))'
t status      '1|0|'                  'false; echo $?; true; echo $?'
t cmdsub      'a b|'                  'echo $(echo a; echo b)'
t cmdsub-nest 'deep|'                 'echo $(echo $(echo deep))'
t backtick    'bt|'                   'echo `echo bt`'
t cmdsub-nl   '[a]|'                  'x=$(printf "a\n\n"); echo "[$x]"'
t split       '3|1|'                  'x="a  b   c"; set -- $x; echo $#; set -- "$x"; echo $#'
t ifs         '4|'                    'IFS=:; x=a:b::c; set -- $x; echo $#'
t at          '[a b]|[c]|[]|'         'f() { for a in "$@"; do echo "[$a]"; done; }; f "a b" c ""'
t star        'a b c|a-b-c|'          'set -- a b c; echo "$*"; IFS=-; echo "$*"'
t shift       'b 2|0|'                'set -- a b c; shift; echo $1 $#; shift 2; echo $#'
t args        'name arg|'             '$SH -c '"'"'echo $0 $1'"'"' name arg'
t tilde       '/x /x/y ~|'            'HOME=/x; echo ~ ~/y "~"'
t comment     'a|'                    'echo a # not this'
t continue    'ab|'                   'echo a\
b'
t unset       'gone|'                 'x=1; unset x; echo ${x-gone}'
t export      '[]|[7]|'               'x=7; $SH -c '"'"'echo [$x]'"'"'; export x; $SH -c '"'"'echo [$x]'"'"
t prefix-env  '5|[]|'                 'x=5 $SH -c '"'"'echo $x'"'"'; echo [$x]'

# --- lists, pipelines, status
t and-or      'a|d|'                  'true && echo a || echo b; false && echo c || echo d'
t pipe        'a|'                    'echo a | cat | cat | cat'
t pipe-status '0|1|'                  'false | true; echo $?; true | false; echo $?'
t not         '0|1|'                  '! false; echo $?; ! true; echo $?'
t group       'b|'                    '{ echo a; echo b; } | grep b'
t subshell    '2|1|'                  'x=1; (x=2; echo $x); echo $x'
t subst-exit  '4|'                    '(exit 4); echo $?'
t notfound    '127|'                  'nonexistent_cmd 2>/dev/null; echo $?'
t bg-wait     '0|3|'                  'true & wait $!; echo $?; $SH -c "exit 3" & wait $!; echo $?'
t sequence    '1|2|'                  'echo 1; echo 2;'

# --- compound commands and their status
t if          'yes|'                  'if true; then echo yes; fi'
t if-else     'no|'                   'if false; then echo yes; else echo no; fi'
t elif        'two|'                  'if false; then echo one; elif true; then echo two; else echo three; fi'
t if-nl       'x|'                    'if true
then
  echo x
fi'
t while       '0|1|2|'                'i=0; while [ $i -lt 3 ]; do echo $i; i=$((i+1)); done'
t until       '0|1|'                  'i=0; until [ $i -ge 2 ]; do echo $i; i=$((i+1)); done'
t for         'a|b|c|'                'for x in a b c; do echo $x; done'
t for-args    '1|2|'                  'set -- 1 2; for x; do echo $x; done'
t for-glob    '[*.nomatch]|'          'for x in *.nomatch; do echo "[$x]"; done'
t break       '1|2|'                  'for i in 1 2 3 4; do if [ $i -eq 3 ]; then break; fi; echo $i; done'
t continue-lp '1|3|'                  'for i in 1 2 3; do if [ $i -eq 2 ]; then continue; fi; echo $i; done'
t break-2     '1a|1b|'                'for i in 1 2; do for j in a b c; do [ $j = c ] && break 2; echo $i$j; done; done'
t continue-2  '1a|2a|'                'for i in 1 2; do for j in a b; do [ $j = b ] && continue 2; echo $i$j; done; done'
t case        'b|'                    'case b in a) echo a;; b|c) echo b;; *) echo other;; esac'
t case-glob   'txt|'                  'case file.txt in *.c) echo c;; *.txt) echo txt;; esac'
t case-quoted 'star|'                 'case "*" in "*") echo star;; *) echo no;; esac'
t case-none   '0|'                    'case x in y) echo y;; esac; echo $?'
t case-nl     'm|'                    'case m in
  a) echo a ;;
  m) echo m ;;
esac'
t st-if       '0|'                    'if false; then :; fi; echo $?'
t st-for      '1|'                    'for i in 1; do false; done; echo $?'
t st-while    '0|'                    'while false; do :; done; echo $?'
t st-group    '1|'                    '{ false; }; echo $?'

# --- functions
t func        'args: 2 a b c|3|'      'f() { echo "args: $# $@"; return 3; }; f a "b c"; echo $?'
t func-fact   '120|'                  'fact() { if [ $1 -le 1 ]; then echo 1; else echo $(( $1 * $(fact $(($1-1))) )); fi; }; fact 5'
t func-params 'inner|top|'            'f() { echo $1; }; set -- top; f inner; echo $1'
t func-return 'a|'                    'f() { echo a; return; echo b; }; f'
t func-redef  '2|'                    'f() { echo 1; }; f() { echo 2; }; f'
t func-status '1|'                    'f() { false; }; f; echo $?'
t func-pipe   'B|'                    'up() { tr a-z A-Z; }; echo b | up'
t func-nl     'x|'                    'f()
{
  echo x
}
f'
t func-temp   '1|[]|'                 'f() { echo $v; }; v=1 f; echo [$v]'
t func-unset  'ok|'                   'f() { :; }; unset -f f; f 2>/dev/null || echo ok'

# --- test and [
t test-str    'a|b|c|'                '[ -z "" ] && echo a; [ -n x ] && echo b; [ x = x ] && [ x != y ] && echo c'
t test-int    'a|b|c|'                '[ 3 -gt 2 ] && echo a; [ 2 -le 2 ] && echo b; [ 1 -ne 2 ] && echo c'
t test-file   'f|d|e|s|'              'echo x > ff; mkdir dd; [ -f ff ] && echo f; [ -d dd ] && echo d; [ -e ff ] && echo e; [ -s ff ] && echo s'
t test-not    'ok|'                   '[ ! -e nofile ] && echo ok'
t test-and-or 'a|b|'                  '[ a = a -a b = b ] && echo a; [ a = b -o b = b ] && echo b'
t test-paren  'ok|'                   '[ \( 1 -eq 1 \) ] && echo ok'
t test-empty  '1|'                    'test; echo $?'
t test-one    '0|1|'                  '[ x ]; echo $?; [ "" ]; echo $?'
t test-err    '2|'                    '[ a -eq 1 ] 2>/dev/null; echo $?'
t test-bracket '2|'                   '[ a = a 2>/dev/null; echo $?'

# --- redirections and here-documents
t redir       'hi|more|hi|more|'      'echo hi > f; cat f; echo more >> f; tail -n 1 f; cat < f'
t redir-err   'err|'                  '{ echo err >&2; } 2>&1'
t redir-null  'a|'                    '{ echo a; echo b >&2; } 2>/dev/null'
t redir-dup   'out|err|'              '{ echo out; echo err >&2; } 2>&1 | cat'
t redir-in    'x y|'                  'echo x y > in; cat < in'
t redir-lp    '1|2|'                  'printf "1\n2\n" > nums; while read n; do echo $n; done < nums'
t redir-group 'a|'                    '{ echo a; } > g; cat g'
t redir-order 'e|'                    'echo e 2>&1 >/dev/null 2>&1; echo e'
t heredoc     'hello 1|'              'x=1; cat <<EOF
hello $x
EOF'
t heredoc-q   'hello $x|'             'x=1; cat <<'"'"'EOF'"'"'
hello $x
EOF'
t heredoc-sub 'sum 3|'                'cat <<EOF
sum $((1+2))
EOF'
t heredoc-2   'a|b|'                  'cat <<A; cat <<B
a
A
b
B'
t heredoc-pipe 'X|'                   'tr a-z A-Z <<EOF
x
EOF'
t heredoc-tab  'a|'                   'cat <<-EOF
	a
	EOF'

# --- builtins
t eval        'evald|5|'              'x="echo evald"; eval $x; eval "y=5"; echo $y'
t read        'a|b c|'                'echo "a b c" | { read x y; echo "$x"; echo "$y"; }'
t read-loop   'n=1|n=2|n=3|'          'printf "1\n2\n3\n" | while read n; do echo n=$n; done'
t read-ifs    'x-y|'                  'echo x:y | { IFS=: read a b; echo $a-$b; }'
t read-eof    '1|'                    'read x < /dev/null; echo $?'
t dot         'from lib|7|'           'echo "lf() { echo from lib; }; lv=7" > lib; . ./lib; lf; echo $lv'
t exec        'replaced|'             'exec echo replaced; echo not'
t cd          '/tmp|/|'               'cd /tmp; pwd; cd /; pwd'
t cd-dash     '/tmp|'                 'cd /tmp; cd /; cd - >/dev/null; pwd'
t colon       '0|'                    ': anything; echo $?'
t set-pos     '3 a|'                  'set -- a b c; echo $# $1'
t type-order  'f|'                    'echo() { printf "f\n"; }; echo x'

# --- set -e and set -x
t errexit     ''                      'set -e; false; echo not'
t errexit-or  'ok|'                   'set -e; false || true; echo ok'
t errexit-if  'ok|'                   'set -e; if false; then :; fi; echo ok'
t errexit-and 'ok|'                   'set -e; false && true; echo ok'
t errexit-not 'ok|'                   'set -e; ! false; echo ok'
t errexit-fn  ''                      'set -e; f() { false; echo in; }; f; echo after'
t errexit-sub ''                      'set -e; (false; echo in); echo after'
t errexit-off 'a|'                    'set -e; set +e; false; echo a'
t xtrace      '+ echo hi|hi|'         'set -x; echo hi'
t xtrace-exp  '+ echo 1 two|1 two|'   'x=1; set -x; echo $x two'

# --- shebang scripts
printf '#!%s\necho shebang $1\n' "$SH" > s.sh
chmod +x s.sh
t shebang     'shebang arg|'          './s.sh arg'
printf '#!%s -x\necho flagged\n' "$SH" > s2.sh
chmod +x s2.sh
t shebang-arg '+ echo flagged|flagged|' './s2.sh'
printf 'echo plain $1\n' > s3.sh
chmod +x s3.sh
t no-shebang  'plain z|'              './s3.sh z'

# --- globbing
mkdir gd && cd gd || exit 1
touch a.c b.c c.h .hidden
mkdir sub && touch sub/x.c sub/y.h
t glob        'a.c b.c|*.x|a.c b.c|c.h|' 'echo *.c; echo *.x; echo [ab].c; echo ?.h'
t glob-hidden 'a.c b.c c.h sub|'  'echo *'
t glob-dot    '.hidden|'          'echo .h*'
t glob-quote  '*.c *.c|'          'echo "*.c" \*.c'
t glob-dir    'sub/x.c sub/y.h|'  'echo sub/*'
t glob-multi  'sub/x.c|'          'echo */x.c'
t glob-trail  'sub/|'             'echo */'
t glob-abs    '/tmp|'             'echo /tm?'
t glob-class  'a.c|'              'echo [a-a].c'
t glob-neg    'b.c|'              'echo [!a].c'
t glob-var    'a.c b.c|'          'p="*.c"; echo $p'
t glob-qvar   '*.c|'              'p="*.c"; echo "$p"'
cd ..

# --- syntax errors
t err-quote   'rc=2|'             '$SH -c "echo \"abc" 2>/dev/null; echo rc=$?'
t err-open    'rc=2|'             '$SH -c "if true; then" 2>/dev/null; echo rc=$?'
t err-stray   'rc=2|'             '$SH -c "echo a )" 2>/dev/null; echo rc=$?'
t err-fi      'rc=2|'             '$SH -c "fi" 2>/dev/null; echo rc=$?'

# --- a script file with arguments, and stdin
printf 'echo "$0 $# $1"\nexit 5\n' > sc.sh
t script      'sc.sh 2 x|rc=5|'   '$SH sc.sh x y; echo rc=$?'
t stdin       'from stdin|'       'echo "echo from stdin" | $SH'
t script-fn   'ok|'               'printf "f() { echo ok; }\nf\n" | $SH'

cd /
rm -rf "$T"
echo "sh_test: $total checks, $fails failed"
[ "$fails" -eq 0 ]

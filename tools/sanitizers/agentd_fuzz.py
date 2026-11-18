# Sends mutated requests to a trap-on-UB host build of agentd (COAL_AGENTD_SAN, see userspace/agentd/test/build_host.sh).
# Run from tools/coal-gateway: COAL_AGENTD_SAN="-fsanitize=undefined -fsanitize-undefined-trap-on-error -fno-stack-protector" python3 ../sanitizers/agentd_fuzz.py SEED SECONDS
import sys,os,random,time,json
sys.path.insert(0,'/home/claude/wt_san/tools/coal-gateway'); sys.path.insert(0,'/home/claude/wt_san/tools/coal-gateway/tests')
import support
import socket
fx=support.HostAgent(); sk=socket.socket(socket.AF_UNIX); sk.connect(fx.socket); sk.setblocking(False)
base=[b'{"id":"1","op":"exec","cmd":"echo hi","timeout_ms":1000}',b'{"id":"2","op":"fs.write","path":"/tmp/agfz","data":"aGVsbG8=","more":false}',
 b'{"id":"3","op":"fs.read","path":"/etc/hostname","offset":0,"limit":100}',b'{"id":"4","op":"fs.list","path":"/tmp"}',b'{"id":"5","op":"hello"}',
 b'{"id":"6","op":"spawn","cmd":"sleep 1","env":{"A":"b"}}']
rng=random.Random(int(sys.argv[1]) if len(sys.argv)>1 else 1)
def mut(b):
    b=bytearray(rng.choice(base))
    for _ in range(rng.randint(1,6)):
        k=rng.random(); i=rng.randrange(len(b)+1)
        if k<.3 and b: b[min(i,len(b)-1)]=rng.randrange(256)
        elif k<.5: b[i:i]=bytes(rng.randrange(256) for _ in range(rng.randint(1,40)))
        elif k<.7 and b: del b[i:i+rng.randint(1,20)]
        elif k<.85: b[i:i]=b'\\u' + bytes(rng.choice(b'0123456789abcdefg"\\') for _ in range(rng.randint(0,5)))
        else: b+=b'A'*rng.randint(100,70000)
    return bytes(b)
import select
n=0;t=time.time()
while time.time()-t<float(sys.argv[2]) if len(sys.argv)>2 else 60:
    line=mut(None).replace(b"\n",b" ")+b"\n"
    try: sk.sendall(line)
    except BlockingIOError: pass
    except OSError: pass
    n+=1
    if fx.proc.poll() is not None: print("AGENTD DIED rc",fx.proc.returncode,"after",n); open('/tmp/agfz_crash.bin','wb').write(line); break
    try:
        while select.select([sk],[],[],0.002)[0]:
            if not sk.recv(65536): break
    except Exception: pass
else: print("no crash in",n)
fx.close()

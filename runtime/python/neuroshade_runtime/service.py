from __future__ import annotations
import collections
import json
import os
import socket
import struct
import threading
from pathlib import Path
from .engines import create_engine,process_frame

HEADER=struct.Struct('<8I');MAGIC=0x3152534e;VERSION=1;MAX_BYTES=4096*4096*4

def read_exact(sock,size):
    data=bytearray()
    while len(data)<size:
        part=sock.recv(min(size-len(data),1024*1024))
        if not part: raise EOFError('runtime peer closed')
        data.extend(part)
    return bytes(data)

def request(sock,op,payload=b'',width=0,height=0,bgra=False):
    sock.sendall(HEADER.pack(MAGIC,VERSION,op,width,height,int(bgra),len(payload),0)+payload)
    magic,version,status,w,h,fmt,size,micros=HEADER.unpack(read_exact(sock,HEADER.size))
    if magic!=MAGIC or version!=VERSION or size>MAX_BYTES: raise ValueError('invalid runtime response')
    body=read_exact(sock,size)
    if status: raise RuntimeError(body.decode('utf-8',errors='replace'))
    return body,micros/1000

def serve(path:Path,config_path:Path,ready=None):
    path.parent.mkdir(parents=True,exist_ok=True)
    # An active endpoint belongs to its running server. Never unlink it.
    if path.exists():
        probe=socket.socket(socket.AF_UNIX);probe.settimeout(1)
        try: probe.connect(str(path))
        except ConnectionRefusedError: path.unlink()
        else: raise RuntimeError('runtime already listening')
        finally: probe.close()
    server=socket.socket(socket.AF_UNIX);server.bind(str(path));os.chmod(path,0o600);server.listen(4)
    warmed=set();cache=collections.OrderedDict();lock=threading.Lock();slots=threading.BoundedSemaphore(4)
    if ready: ready.set()
    print('runtime=ready socket='+str(path),flush=True)
    def handle(peer):
        engine=None;config={}
        try:
            _,uid,_=struct.unpack('3i',peer.getsockopt(socket.SOL_SOCKET,socket.SO_PEERCRED,12))
            if uid!=os.getuid(): return
            peer.settimeout(120)
            while True:
                magic,version,op,w,h,fmt,size,_=HEADER.unpack(read_exact(peer,HEADER.size))
                if magic!=MAGIC or version!=VERSION or op not in (1,2,3): raise ValueError('invalid protocol header')
                if size>MAX_BYTES or (op==1 and not 1<=size<=4096) or (op==3 and size): raise ValueError('payload limit exceeded')
                if op==2 and (not 1<=w<=4096 or not 1<=h<=4096 or fmt not in (0,1) or size!=w*h*4):
                    raise ValueError('frame contract mismatch')
                payload=read_exact(peer,size)
                micros=0
                try:
                    if op==3:
                        body=json.dumps({'version':VERSION,'engines':['pytorch','onnxruntime','dlssnr_hip'],'pid':os.getpid()}).encode()
                    elif op==1:
                        config=json.loads(config_path.read_text()) if config_path.is_file() else {}
                        model=Path(payload.decode()).resolve()
                        key=(str(model), (model/'manifest.json').stat().st_mtime_ns,json.dumps(config,sort_keys=True))
                        # Native graphs own internal recurrent buffers: never share across peers.
                        if json.loads((model/'manifest.json').read_text()).get('runtime') == 'dlssnr_hip':
                            key=key+(object(),)
                        with lock:
                            if key not in cache:
                                # Drop old engines before loading a new large model.
                                while len(cache)>=2: cache.popitem(last=False)
                                cache[key]=create_engine(model,config)
                            engine=cache[key];cache.move_to_end(key)
                            if w or h:
                                if not 1<=w<=4096 or not 1<=h<=4096: raise ValueError('invalid warmup extent')
                                warm_key=(id(engine),w,h)
                                if warm_key not in warmed:
                                    process_frame(engine,bytes(w*h*4),w,h,False,config)
                                    if hasattr(engine,'reset'): engine.reset()
                                    warmed.add(warm_key)
                        body=json.dumps(engine.info).encode()
                        print('model=loaded '+body.decode(),flush=True)
                    else:
                        if engine is None: raise ValueError('load a model first')
                        with lock: body,ms=process_frame(engine,payload,w,h,bool(fmt),config)
                        micros=min(int(ms*1000),0xffffffff)
                    peer.sendall(HEADER.pack(MAGIC,VERSION,0,w,h,fmt,len(body),micros)+body)
                except Exception as error:
                    body=str(error).encode()[:4096]
                    print('runtime_error='+str(error),flush=True)
                    peer.sendall(HEADER.pack(MAGIC,VERSION,1,0,0,0,len(body),0)+body)
        except (EOFError,ConnectionError,TimeoutError,OSError,ValueError) as error:
            if not isinstance(error,EOFError): print('client_closed='+str(error),flush=True)
        finally:
            with lock:
                if engine is not None and hasattr(engine,'close'):
                    engine.close()
                    for key in list(cache):
                        if cache[key] is engine: del cache[key]
                    warmed.difference_update({k for k in warmed if k[0]==id(engine)})
            peer.close();slots.release()
    try:
        while True:
            peer,_=server.accept()
            if not slots.acquire(blocking=False): peer.close();continue
            threading.Thread(target=handle,args=(peer,),daemon=True).start()
    finally:
        server.close();path.unlink(missing_ok=True)

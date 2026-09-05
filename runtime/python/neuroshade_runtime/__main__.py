import argparse,json,os,socket,subprocess,sys,time
from pathlib import Path
from .service import request,serve

def main():
    parser=argparse.ArgumentParser(description='NeuroShade host64 reconstruction runtime')
    parser.add_argument('--socket',type=Path,default=Path.home()/'.local/state/neuroshade/runtime.sock')
    parser.add_argument('--config',type=Path,default=Path.home()/'.config/neuroshade/runtime.json')
    sub=parser.add_subparsers(dest='command',required=True)
    sub.add_parser('serve');sub.add_parser('start');sub.add_parser('status')
    imp=sub.add_parser('import');imp.add_argument('source',type=Path);imp.add_argument('destination',type=Path)
    bench=sub.add_parser('benchmark');bench.add_argument('model',type=Path);bench.add_argument('--width',type=int,default=640);bench.add_argument('--height',type=int,default=360);bench.add_argument('--frames',type=int,default=3)
    run=sub.add_parser('process');run.add_argument('model',type=Path);run.add_argument('input',type=Path);run.add_argument('output',type=Path)
    args=parser.parse_args()
    if args.command=='import':
        from .import_model import import_checkpoint
        print(json.dumps(import_checkpoint(args.source,args.destination),indent=2));return
    if args.command=='serve': serve(args.socket,args.config);return
    if args.command=='start':
        try:
            with socket.socket(socket.AF_UNIX) as s:
                s.settimeout(2);s.connect(str(args.socket));body,_=request(s,3);print(body.decode());return
        except (OSError,EOFError): pass
        args.socket.parent.mkdir(parents=True,exist_ok=True)
        with (args.socket.parent/'host64.log').open('ab') as log:
            process=subprocess.Popen([sys.executable,'-m','neuroshade_runtime','--socket',str(args.socket),'--config',str(args.config),'serve'],stdin=subprocess.DEVNULL,stdout=log,stderr=log,start_new_session=True)
        for _ in range(100):
            if process.poll() is not None: raise RuntimeError('host64 failed; see host64.log')
            try:
                with socket.socket(socket.AF_UNIX) as s:
                    s.settimeout(.2);s.connect(str(args.socket));body,_=request(s,3);print(body.decode());return
            except (OSError,EOFError): time.sleep(.1)
        raise RuntimeError('host64 startup timed out')
    with socket.socket(socket.AF_UNIX) as s:
        s.settimeout(120);s.connect(str(args.socket))
        if args.command=='status': body,_=request(s,3);print(body.decode());return
        body,_=request(s,1,str(args.model.resolve()).encode());print(body.decode())
        import numpy as np
        from PIL import Image
        if args.command=='process':
            source=np.asarray(Image.open(args.input).convert('RGBA'));h,w=source.shape[:2]
            body,ms=request(s,2,source.tobytes(),w,h)
            Image.fromarray(np.frombuffer(body,np.uint8).reshape(h,w,4)).save(args.output)
            print(json.dumps({'output':str(args.output),'inference_ms':ms}));return
        rng=np.random.default_rng(42);source=rng.integers(0,256,(args.height,args.width,4),dtype=np.uint8);source[:,:,3]=255
        times=[]
        for _ in range(args.frames):
            body,ms=request(s,2,source.tobytes(),args.width,args.height);times.append(ms)
            result=np.frombuffer(body,np.uint8).reshape(source.shape)
            if not np.array_equal(result[:,:,3],source[:,:,3]): raise RuntimeError('alpha changed')
        print(json.dumps({'width':args.width,'height':args.height,'inference_ms':times,'output_changed':not np.array_equal(result,source)},indent=2))
if __name__=='__main__': main()

"""Real model inference. No generated weights or synthetic enhancement fallback."""
from __future__ import annotations
import hashlib
import json
import math
import time
from pathlib import Path
import numpy as np


def load_manifest(path: Path) -> dict:
    path = path.resolve()
    if path.suffix != '.nsmodel' or not path.is_dir():
        raise ValueError('expected a .nsmodel directory')
    manifest = json.loads((path / 'manifest.json').read_text())
    if manifest.get('schema') != 1 or manifest.get('history') != 0:
        raise ValueError('host64 supports schema 1 spatial image models with history=0')
    if len(manifest.get('inputs', [])) != 1:
        raise ValueError('spatial engine requires exactly one image input')
    for tensor in [manifest['inputs'][0], manifest['output']]:
        if tensor['layout'] != 'NCHW' or tensor['dtype'] not in ('fp32','fp16'):
            raise ValueError('spatial engine requires NCHW fp32/fp16 tensors')
    scale = manifest['scale']
    if scale['x'] != scale['y'] or not 1 <= scale['x'] <= 8 or int(scale['x']) != scale['x']:
        raise ValueError('supported image scales are integer 1x through 8x')
    metadata = json.loads((path / 'metadata.json').read_text())
    for name, digest in metadata.get('sha256', {}).items():
        if name not in ('model.onnx','model.pth','model.safetensors'):
            raise ValueError('unrecognized model artifact in hash table')
        if hashlib.sha256((path / name).read_bytes()).hexdigest() != digest:
            raise ValueError(f'artifact checksum mismatch: {name}')
    return manifest


class TorchEngine:
    """Spandrel: ESRGAN/RRDB, RealESRGAN variants, SRVGG, SwinIR, etc."""
    def __init__(self, path: Path, manifest: dict, config: dict):
        import torch
        from spandrel import ModelLoader, ImageModelDescriptor
        self.torch = torch
        self.device = config.get('device', 'cuda:0')
        if self.device.startswith('cuda') and not torch.cuda.is_available():
            raise RuntimeError('GPU PyTorch/ROCm unavailable; select device=cpu explicitly')
        artifact = path / 'model.safetensors'
        if artifact.is_file():
            from safetensors.torch import load_file
            state = load_file(str(artifact), device='cpu')
        else:
            # Never fall back to arbitrary pickle execution for checkpoints.
            state = torch.load(path / 'model.pth', map_location='cpu', weights_only=True)
        self.model = ModelLoader().load_from_state_dict(state)
        if not isinstance(self.model, ImageModelDescriptor):
            raise ValueError('model is not an image-to-image reconstruction network')
        if self.model.input_channels != 3 or self.model.output_channels != 3:
            raise ValueError('Spandrel reconstruction currently requires RGB input/output')
        self.scale = self.model.scale
        if self.scale != manifest['scale']['x']:
            raise ValueError('checkpoint scale differs from package manifest')
        self.model.to(self.device).eval()
        self.dtype = torch.float32
        if config.get('precision','auto') in ('auto','fp16') and self.device.startswith('cuda') and self.model.supports_half:
            self.model.half(); self.dtype=torch.float16
        elif config.get('precision') == 'fp16':
            raise ValueError('this model/device does not support fp16')
        self.tile = int(config.get('tile', 192))
        self.overlap = int(config.get('overlap', 24))
        if not 32 <= self.tile <= 1024 or not 0 <= self.overlap <= self.tile // 2:
            raise ValueError('invalid tile/overlap settings')
        self.backend = 'PyTorch/' + ('ROCm' if torch.version.hip else 'CUDA') if self.device.startswith('cuda') else 'PyTorch/CPU'
        self.info = dict(backend=self.backend, architecture=self.model.architecture.id,
                         device=torch.cuda.get_device_name(self.device) if self.device.startswith('cuda') else 'CPU',
                         scale=self.scale, precision=str(self.dtype), tile=self.tile, overlap=self.overlap)
        with torch.inference_mode():
            self.model(torch.zeros((1,3,32,32),device=self.device,dtype=self.dtype))
        if self.device.startswith('cuda'): torch.cuda.synchronize(self.device)

    def run(self, image: np.ndarray, width: int, height: int, mode: str) -> np.ndarray:
        torch=self.torch
        with torch.inference_mode():
            tensor=torch.from_numpy(np.ascontiguousarray(image[:,:,:3])).to(self.device,dtype=self.dtype)
            tensor=tensor.permute(2,0,1).unsqueeze(0)/255
            if mode == 'reconstruct':
                tensor=torch.nn.functional.interpolate(tensor,size=(math.ceil(height/self.scale),math.ceil(width/self.scale)),mode='area')
            elif mode != 'native':
                raise ValueError('input_mode must be reconstruct or native')
            ih,iw=tensor.shape[-2:]
            # Tile output is reduced to presentation resolution as it is assembled;
            # native mode never allocates a full 4x 8K/16K intermediate image.
            out=torch.empty((1,3,height,width),device=self.device,dtype=self.dtype)
            for y in range(0,ih,self.tile):
                for x in range(0,iw,self.tile):
                    ey,ex=min(y+self.tile,ih),min(x+self.tile,iw)
                    y0,x0=max(0,y-self.overlap),max(0,x-self.overlap)
                    y1,x1=min(ih,ey+self.overlap),min(iw,ex+self.overlap)
                    tile=self.model(tensor[:,:,y0:y1,x0:x1])
                    core=tile[:,:,(y-y0)*self.scale:(ey-y0)*self.scale,(x-x0)*self.scale:(ex-x0)*self.scale]
                    oy0,oy1=round(y*height/ih),round(ey*height/ih)
                    ox0,ox1=round(x*width/iw),round(ex*width/iw)
                    if core.shape[-2:] != (oy1-oy0,ox1-ox0):
                        core=torch.nn.functional.interpolate(core,size=(oy1-oy0,ox1-ox0),mode='bilinear',align_corners=False)
                    out[:,:,oy0:oy1,ox0:ox1]=core
            if not torch.isfinite(out).all(): raise RuntimeError('model produced NaN/Inf')
            return out.clamp(0,1).mul(255).round().byte()[0].permute(1,2,0).cpu().numpy()


class OnnxEngine:
    """Portable .nsmodel ONNX execution with explicit provider selection."""
    def __init__(self,path: Path,manifest: dict,config: dict):
        import onnxruntime as ort
        provider=config.get('onnx_provider','CPUExecutionProvider')
        if provider not in ort.get_available_providers(): raise RuntimeError(f'ONNX provider unavailable: {provider}')
        options=ort.SessionOptions();options.intra_op_num_threads=int(config.get('cpu_threads',4))
        self.session=ort.InferenceSession(str(path/'model.onnx'),sess_options=options,providers=[provider])
        self.session.disable_fallback()
        inputs=self.session.get_inputs();outputs=self.session.get_outputs()
        if len(inputs)!=1 or len(outputs)!=1: raise ValueError('ONNX model must have one image input and output')
        self.input=inputs[0];self.output=outputs[0];self.scale=int(manifest['scale']['x'])
        shape=self.input.shape
        if len(shape)!=4 or shape[1] not in (3,4): raise ValueError('ONNX input must be RGB/RGBA NCHW')
        if inputs[0].name != manifest['inputs'][0]['tensor'] or outputs[0].name != manifest['output']['tensor']:
            raise ValueError('ONNX tensor names differ from manifest')
        self.channels=shape[1]
        self.fixed=all(isinstance(d,int) and d>0 for d in shape[-2:])
        self.tile_h,self.tile_w=shape[-2:] if self.fixed else (int(config.get('tile',192)),)*2
        self.overlap=min(int(config.get('overlap',24)),(min(self.tile_h,self.tile_w)-1)//2)
        self.dtype=np.float16 if self.input.type=='tensor(float16)' else np.float32
        if self.input.type not in ('tensor(float)','tensor(float16)'): raise ValueError('ONNX input must be float16/float32')
        self.info=dict(backend='ONNXRuntime/'+provider,architecture=manifest.get('name','ONNX'),device=provider,scale=self.scale,precision=str(self.dtype))
        probe=np.zeros((1,self.channels,self.tile_h,self.tile_w),dtype=self.dtype)
        self._eval(probe)

    def _eval(self,tensor):
        out=self.session.run([self.output.name],{self.input.name:tensor})[0]
        if out.ndim!=4 or out.shape[:2] not in ((1,3),(1,4)) or out.shape[-2:] != (tensor.shape[-2]*self.scale,tensor.shape[-1]*self.scale):
            raise ValueError('ONNX output shape/scale violates image contract')
        if not np.isfinite(out).all(): raise RuntimeError('ONNX produced NaN/Inf')
        return out

    def run(self,image:np.ndarray,width:int,height:int,mode:str)->np.ndarray:
        from PIL import Image
        channels=self.channels
        src=image[:,:,:channels]
        if mode=='reconstruct':
            src=np.asarray(Image.fromarray(src).resize((math.ceil(width/self.scale),math.ceil(height/self.scale)),Image.Resampling.BOX))
        elif mode!='native': raise ValueError('invalid input_mode')
        ih,iw=src.shape[:2];result=np.empty((height,width,3),dtype=np.uint8)
        pad=self.overlap;ch,cw=self.tile_h-2*pad,self.tile_w-2*pad
        padded=np.pad(src,((pad,pad+self.tile_h),(pad,pad+self.tile_w),(0,0)),mode='edge')
        for y in range(0,ih,ch):
            for x in range(0,iw,cw):
                ey,ex=min(y+ch,ih),min(x+cw,iw)
                crop=padded[y:y+self.tile_h,x:x+self.tile_w]
                tensor=np.ascontiguousarray(crop.transpose(2,0,1)[None],dtype=self.dtype)/self.dtype(255)
                tile=self._eval(tensor)[0,:3,pad*self.scale:(pad+ey-y)*self.scale,pad*self.scale:(pad+ex-x)*self.scale]
                tile=(tile.transpose(1,2,0).clip(0,1)*255).round().astype(np.uint8)
                oy0,oy1=round(y*height/ih),round(ey*height/ih);ox0,ox1=round(x*width/iw),round(ex*width/iw)
                result[oy0:oy1,ox0:ox1]=np.asarray(Image.fromarray(tile).resize((ox1-ox0,oy1-oy0),Image.Resampling.BILINEAR))
        return result


def create_engine(path:Path,config:dict):
    manifest=load_manifest(path)
    backend=config.get('backend','auto')
    if backend=='auto': backend='pytorch' if manifest['runtime']=='pytorch' else 'onnxruntime'
    if backend=='pytorch': return TorchEngine(path,manifest,config)
    if backend=='onnxruntime': return OnnxEngine(path,manifest,config)
    raise ValueError('unknown host64 backend: '+backend)


def process_frame(engine,raw:bytes,width:int,height:int,bgra:bool,config:dict):
    if not 1<=width<=4096 or not 1<=height<=4096 or len(raw)!=width*height*4:
        raise ValueError('frame dimensions/payload invalid')
    started=time.perf_counter()
    source=np.frombuffer(raw,dtype=np.uint8).reshape(height,width,4)
    rgba=source[:,:,[2,1,0,3]] if bgra else source
    rgb=engine.run(rgba,width,height,config.get('input_mode','reconstruct'))
    if rgb.shape!=(height,width,3) or rgb.dtype!=np.uint8: raise ValueError('invalid reconstruction output')
    out=np.array(rgba,copy=True);out[:,:,:3]=rgb # Preserve game alpha exactly.
    if bgra: out=out[:,:,[2,1,0,3]]
    return out.tobytes(),(time.perf_counter()-started)*1000

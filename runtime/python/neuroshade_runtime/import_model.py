"""Export detected image checkpoints as portable ONNX + PyTorch .nsmodel packages."""
from pathlib import Path
import hashlib,json,shutil,tempfile,re

def import_checkpoint(source:Path,destination:Path):
    import torch,onnx,onnxruntime as ort,numpy as np
    from spandrel import ModelLoader,ImageModelDescriptor
    source=source.resolve();destination=destination.resolve()
    if destination.exists(): raise ValueError('destination already exists')
    if destination.suffix!='.nsmodel': raise ValueError('destination must end in .nsmodel')
    state=torch.load(source,map_location='cpu',weights_only=True) if source.suffix!='.safetensors' else None
    if state is None:
        from safetensors.torch import load_file
        state=load_file(str(source))
    model=ModelLoader().load_from_state_dict(state)
    if not isinstance(model,ImageModelDescriptor) or model.input_channels!=3 or model.output_channels!=3:
        raise ValueError('checkpoint must be an RGB image reconstruction model')
    model.eval().float();torch.set_num_threads(4)
    destination.parent.mkdir(parents=True,exist_ok=True)
    temp=Path(tempfile.mkdtemp(prefix='.nsmodel-',dir=destination.parent))
    try:
        side=max(32,model.size_requirements.minimum)
        mult=max(1,model.size_requirements.multiple_of);side=((side+mult-1)//mult)*mult
        sample=torch.rand(1,3,side,side)
        # The underlying architecture is exported; descriptor padding is performed
        # by the runtime. Unsupported exporters fail explicitly without installing.
        torch.onnx.export(model.model,(sample,),str(temp/'model.onnx'),dynamo=False,
            input_names=['color'],output_names=['output'],opset_version=18,
            dynamic_axes={'color':{2:'height',3:'width'},'output':{2:'out_height',3:'out_width'}})
        graph=onnx.load(temp/'model.onnx');onnx.checker.check_model(graph)
        options=ort.SessionOptions();options.intra_op_num_threads=4
        session=ort.InferenceSession(str(temp/'model.onnx'),sess_options=options,providers=['CPUExecutionProvider'])
        with torch.inference_mode(): expected=model.model(sample).numpy()
        actual=session.run(None,{'color':sample.numpy()})[0]
        if expected.shape!=actual.shape or not np.isfinite(actual).all(): raise ValueError('export output contract failed')
        difference=float(np.max(np.abs(expected-actual)))
        if difference>0.003: raise ValueError(f'ONNX/PyTorch verification failed: {difference}')
        torch.save(state,temp/'model.pth')
        scale=int(model.scale);ident=re.sub('[^a-z0-9._-]','-',source.stem.lower())
        manifest={'schema':1,'id':'org.neuroshade.imported.'+ident,'name':source.stem,'version':'1.0.0','runtime':'pytorch',
            'inputs':[{'semantic':'Color.Final','tensor':'color','dtype':'fp32','layout':'NCHW'}],
            'output':{'semantic':'Output.Color','tensor':'output','dtype':'fp32','layout':'NCHW'},
            'history':0,'scale':{'x':scale,'y':scale},'first_frame':'spatial',
            'shapes':{'kind':'dynamic','buckets':[{'input':[1,3,side,side],'output':[1,3,side*scale,side*scale]}]}}
        signature={'schema':1,'inputs':{'color':{'dtype':'fp32','shape':[1,3,-1,-1]}},'outputs':{'output':{'dtype':'fp32','shape':[1,3,-1,-1]}}}
        metadata={'schema':1,'architecture':model.architecture.id,'source':source.name,'onnx_verification_max_abs':difference,
                  'sha256':{name:hashlib.sha256((temp/name).read_bytes()).hexdigest() for name in ['model.onnx','model.pth']}}
        for name,data in [('manifest.json',manifest),('signature.json',signature),('metadata.json',metadata)]:
            (temp/name).write_text(json.dumps(data,indent=2)+'\n')
        from PIL import Image
        Image.fromarray((actual[0].transpose(1,2,0).clip(0,1)*255).astype(np.uint8)).save(temp/'preview.webp')
        temp.rename(destination)
        return {'path':str(destination),'architecture':model.architecture.id,'scale':scale,'verification_max_abs':difference}
    finally:
        if temp.exists(): shutil.rmtree(temp)

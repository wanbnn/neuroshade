import json,os,sys,tempfile,threading,time,socket,unittest
from pathlib import Path
import numpy as np
import onnx
from onnx import helper,TensorProto,numpy_helper
from neuroshade_runtime.engines import create_engine,process_frame,load_manifest
from neuroshade_runtime.service import serve,request,HEADER,MAGIC,VERSION

class RuntimeTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.root=Path(self.temp.name)
        self.model=self.root/'gain.nsmodel';self.model.mkdir()
        manifest={'schema':1,'runtime':'onnxruntime','history':0,'name':'Channel gain test',
            'inputs':[{'tensor':'color','layout':'NCHW','dtype':'fp32'}],
            'output':{'tensor':'output','layout':'NCHW','dtype':'fp32'},'scale':{'x':1,'y':1}}
        (self.model/'manifest.json').write_text(json.dumps(manifest));(self.model/'metadata.json').write_text('{}')
        graph=helper.make_graph([helper.make_node('Mul',['color','gain'],['output'])],'test',
            [helper.make_tensor_value_info('color',TensorProto.FLOAT,[1,4,32,32])],
            [helper.make_tensor_value_info('output',TensorProto.FLOAT,[1,4,32,32])],
            [numpy_helper.from_array(np.array([.5,.75,1.,0.],np.float32).reshape(1,4,1,1),'gain')])
        model=helper.make_model(graph,opset_imports=[helper.make_opsetid('',18)]);model.ir_version=10
        onnx.save(model,self.model/'model.onnx')
        self.config={'backend':'onnxruntime','overlap':4,'input_mode':'native'}
    def tearDown(self): self.temp.cleanup()
    def test_rgb_bgra_tiling_alpha(self):
        engine=create_engine(self.model,self.config)
        pixels=np.zeros((39,57,4),np.uint8);pixels[:]=[100,120,140,77]
        for bgra in [False,True]:
            body,ms=process_frame(engine,pixels.tobytes(),57,39,bgra,self.config)
            out=np.frombuffer(body,np.uint8).reshape(pixels.shape)
            expected=[100,90,70,77] if bgra else [50,90,140,77]
            np.testing.assert_array_equal(out,np.broadcast_to(expected,pixels.shape));self.assertGreater(ms,0)
    def test_reject_invalid_contract_and_checksum(self):
        engine=create_engine(self.model,self.config)
        with self.assertRaises(ValueError):process_frame(engine,b'',10,10,False,self.config)
        (self.model/'metadata.json').write_text(json.dumps({'sha256':{'model.onnx':'bad'}}))
        with self.assertRaisesRegex(ValueError,'checksum'):load_manifest(self.model)
    def test_host_protocol(self):
        config=self.root/'config.json';config.write_text(json.dumps(self.config));endpoint=self.root/'runtime.sock'
        # Separate process so the test owns and terminates the listener.
        import subprocess
        server=subprocess.Popen([sys.executable,'-m','neuroshade_runtime','--socket',str(endpoint),'--config',str(config),'serve'],stdout=subprocess.DEVNULL)
        try:
            for _ in range(100):
                if endpoint.exists():break
                time.sleep(.05)
            with socket.socket(socket.AF_UNIX) as peer:
                peer.settimeout(5);peer.connect(str(endpoint))
                body,_=request(peer,3);self.assertIn('engines',json.loads(body))
                body,_=request(peer,1,str(self.model).encode(),57,39);self.assertIn('ONNXRuntime',body.decode())
                source=np.full((39,57,4),100,np.uint8)
                body,_=request(peer,2,source.tobytes(),57,39)
                self.assertEqual(len(body),source.nbytes)
                self.assertEqual(body[0],50);self.assertEqual(body[3],100)
                with self.assertRaises(RuntimeError):request(peer,1,str(self.root/'missing.nsmodel').encode())
            self.assertEqual(endpoint.stat().st_mode & 0o777,0o600)
        finally:server.terminate();server.wait(timeout=10)
if __name__=='__main__':unittest.main()

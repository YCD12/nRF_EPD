const fs=require('fs'),vm=require('vm'),path=require('path');
// Pinned reference from the EPD-nRF5_DYC Web version used for the App port.
const repoWeb=path.resolve(__dirname,'fixtures/legacy-web-dithering.js');
const src=fs.readFileSync(repoWeb,'utf8');
function runWeb(data,w,h,alg,strength,contrast,mode){
  const ctx={Math,Uint8Array,Uint8ClampedArray,console};
  vm.createContext(ctx);
  vm.runInContext(src+";globalThis.__f={adjustContrast,ditherImage,processImageData};",ctx);
  const arr=Array.from(data);
  const res=vm.runInContext(`(()=>{const img={width:${w},height:${h},data:new Uint8ClampedArray(globalThis.__d)};__f.adjustContrast(img,${contrast});__f.ditherImage(img,'${alg}',${strength},'${mode}');const out=__f.processImageData(img,'${mode}');return Array.from(out);})()`,Object.assign(ctx,{__d:arr}));
  return Uint8Array.from(res);
}
(async()=>{
  const D=await import('./dither.mts');
  const w=40,h=30;
  const rgba=new Uint8ClampedArray(w*h*4);
  for(let y=0;y<h;y++)for(let x=0;x<w;x++){const i=(y*w+x)*4;rgba[i]=(x*7+y*13)%256;rgba[i+1]=(x*11+y*3)%256;rgba[i+2]=(x*5+y*17)%256;rgba[i+3]=255;}
  const modes=['blackWhiteColor','threeColor','fourColor'];
  const algs=['floydSteinberg','atkinson','bayer','stucki','jarvis','none'];
  let pass=0,fail=0;
  for(const mode of modes)for(const alg of algs){
    const work=new Uint8ClampedArray(rgba);
    const opt=new D.ProcessOptions(); opt.contrast=1.2;
    D.applyImageTone(work,opt);
    D.ditherImage(w,h,work,alg,1.0,mode);
    const mine=D.packImage(work,w,h,mode);
    const web=runWeb(rgba,w,h,alg,1.0,1.2,mode);
    let same=mine && mine.length===web.length;
    if(same){for(let i=0;i<mine.length;i++)if(mine[i]!==web[i]){same=false;break;}}
    if(same){pass++;}else{fail++;console.log('MISMATCH',mode,alg,'mine',mine?mine.length:null,'web',web.length);}
  }
  console.log('RESULT pass='+pass+' fail='+fail);
  if(fail) throw new Error('Web parity failed');
  // protocol checks
  const P=await import('./Protocol.mts');
  const block=P.buildBlockFrame(1,300,0xF0|0x0F,new Uint8Array([1,2,3,4]));
  console.log('block frame:',P.bytesToHex(new Uint8Array(block)));
  const st=P.parseStatus(new Uint8Array([0xA1,0x2C,0x01,0x10,0x00,0x07,0x01,0xFF,0x0F]));
  console.log('status total/received/session/active:',st.total,st.received,st.session,st.active,'missing(300):',P.missingBlocks(st,300).length);
})();
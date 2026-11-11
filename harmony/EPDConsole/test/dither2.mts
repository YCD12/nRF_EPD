import { processImage, ProcessOptions } from './Dither.mts';
function make(w:number,h:number,fn:(x:number,y:number)=>[number,number,number]):Uint8ClampedArray{
  const d=new Uint8ClampedArray(w*h*4);
  for(let y=0;y<h;y++)for(let x=0;x<w;x++){const [r,g,b]=fn(x,y);const i=(y*w+x)*4;d[i]=r;d[i+1]=g;d[i+2]=b;d[i+3]=255;}
  return d;
}
const w=16,h=8;
// pure red
let a=make(w,h,()=>[255,0,0]);
let o=new ProcessOptions(); o.mode='threeColor'; o.alg='none'; o.contrast=1; o.brightness=0; o.gamma=1; o.saturation=1; o.redSensitivity=0.45;
let p=processImage(a,w,h,o)!;
let bw=p.slice(0,w/8*h), red=p.slice(w/8*h);
console.log('pure red: bw all ones?', bw.every(v=>v===255), 'red all zero?', red.every(v=>v===0));
// pure white
a=make(w,h,()=>[255,255,255]); p=processImage(a,w,h,o)!;
bw=p.slice(0,w/8*h); red=p.slice(w/8*h);
console.log('pure white: bw ones?', bw.every(v=>v===255), 'red ones?', red.every(v=>v===255));
// pure black
a=make(w,h,()=>[0,0,0]); p=processImage(a,w,h,o)!;
bw=p.slice(0,w/8*h); red=p.slice(w/8*h);
console.log('pure black: bw zeros?', bw.every(v=>v===0), 'red ones?', red.every(v=>v===255));
// gray 50% with FS -> roughly half bits set
a=make(w,h,()=>[128,128,128]); o.alg='floydSteinberg';
p=processImage(a,w,h,o)!; bw=p.slice(0,w/8*h);
let cnt=0; for(const v of bw) for(let i=0;i<8;i++) if(v&(1<<i)) cnt++;
console.log('gray FS white ratio:', (cnt/(w*h)).toFixed(3));
// bw mode packed length
o.mode='blackWhiteColor'; a=make(w,h,()=>[200,200,200]); p=processImage(a,w,h,o)!;
console.log('bw packed len ok:', p.length===Math.ceil(w/8)*h);

'use strict';
const {test} = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs'), vm = require('node:vm');
const source=fs.readFileSync(require.resolve('../html/js/site_statistics.js'),'utf8');
function fixture(protocol,readyState='complete',idle=true) {
  const scripts=[],tasks=[],listeners=[];
  const window={location:{protocol},addEventListener(type,callback,options){listeners.push({type,callback,options});},
    setTimeout(callback,delay){tasks.push({callback,delay});}};
  if(idle)window.requestIdleCallback=(callback,options)=>tasks.push({callback,options});
  const document={readyState,querySelector(){return scripts[0]||null;},createElement(){return {};},
    body:{appendChild(script){scripts.push(script);}}};
  vm.runInNewContext(source,{window,document});
  return {scripts,tasks,listeners};
}
test('local files and non-web pages never schedule or load third-party statistics',()=>{
  for(const protocol of ['file:','about:','chrome-extension:']) {
    const f=fixture(protocol);
    assert.equal(f.tasks.length,0);assert.equal(f.listeners.length,0);assert.equal(f.scripts.length,0);
  }
});
for(const protocol of ['https:','http:'])test(protocol+' statistics remain lazy and only load once',()=>{
  const f=fixture(protocol);assert.equal(f.scripts.length,0);assert.equal(f.tasks.length,1);
  f.tasks[0].callback();f.tasks[0].callback();assert.equal(f.scripts.length,1);
  assert.equal(f.scripts[0].async,true);assert.match(f.scripts[0].src,/^https:\/\/busuanzi\.ibruce\.info\//);
});
test('web-page statistics wait for load before scheduling idle work',()=>{
  const f=fixture('https:','loading');assert.equal(f.tasks.length,0);
  assert.equal(f.listeners[0].type,'load');assert.equal(f.listeners[0].options.once,true);
  f.listeners[0].callback();assert.equal(f.tasks.length,1);assert.equal(f.scripts.length,0);
});
test('web browsers without idle callbacks retain the delayed load fallback',()=>{
  const f=fixture('https:','complete',false);assert.equal(f.tasks[0].delay,200);
  f.tasks[0].callback();assert.equal(f.scripts.length,1);
});

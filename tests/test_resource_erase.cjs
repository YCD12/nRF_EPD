'use strict';
const {test} = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs'), vm = require('node:vm');
const api = require('../html/js/resource_store.js');
const firmwarePolicy = require('../html/js/main.js');
const base = api.prepareBundle(require('../html/resources/default-project.json').files.map(file => ({
  name:file.name, bytes:new Uint8Array(Buffer.from(file.base64,'base64'))
})));
const deferred = () => { let resolve; const promise=new Promise(yes=>{resolve=yes;}); return {resolve,promise}; };

function fixture(options={}) {
  const memory = new Uint8Array(524288).fill(0x31), nodes=new Map(), calls=[], uploads=[], prompts=[];
  for(const item of base.packages) memory.set(item.bytes,base.partitions.find(p=>p.id===item.pack.id).offset);
  const node = id => {
    if(!nodes.has(id)) nodes.set(id,{value:'',checked:false,classList:{add(){},remove(){}},addEventListener(){}});
    return nodes.get(id);
  };
  const partition = id => id===1 ? {id:1,offset:0,size:4096} : base.partitions.find(p=>p.id===id);
  const pack = id => {
    const p=partition(id), header=memory.slice(p.offset,p.offset+32), info=api.parseHeader(header,p);
    return api.parsePackage(memory.slice(p.offset,p.offset+32+info.length));
  };
  class Client {
    status() {
      const b=new Uint8Array(16); b.set([0xa7,0,0,0,options.legacy?0x3f:0x7f,1,0,this.entered?1:0]);
      new DataView(b.buffer).setUint32(12,524288,true);
      try { pack(1); } catch(_) { b[3]=4; }
      if(options.active)b[6]=7;
      this.capabilities=api.parseStatus(b);return b;
    }
    partitions() { return api.parseTable(pack(1)); }
    inspect(id) { try { const p=pack(id);return {code:0,revision:p.revision,length:p.payload.length,crc:p.crc}; }
      catch(error) { return {code:/CRC/.test(error.message)?3:4}; } }
    readPackage(p) { const info=pack(p.id);return memory.slice(p.offset,p.offset+32+info.payload.length); }
    readRange(at,length) { return memory.slice(at,at+length); }
    async request(op,bytes) { calls.push({op,bytes});if(op===6)this.entered=true; }
    async eraseSector(id,offset) {
      calls.push({op:11,id,offset});
      await options.beforeErase?.(id,offset);
      if(options.failAt===offset)throw Object.assign(new Error('擦除校验失败'),{code:3});
      const p=id?partition(id):{offset:0};memory.fill(255,p.offset+offset,p.offset+offset+4096);
    }
    async leave() {
      calls.push({op:7});this.entered=false;
      if(options.leaveFail)throw new Error('应用响应超时');
      for(const id of [1,2,3,5,6,7]) if(this.inspect(id).code)
        throw Object.assign(new Error('资源未安装'),{code:id===1?4:1});
    }
    async upload(bytes) { const p=api.parsePackage(bytes);uploads.push(p.id);memory.set(bytes,partition(p.id).offset); }
    disconnect() {}
  }
  const context={Uint8Array,appVersion:firmwarePolicy.newWebMinVersion,document:{getElementById:node},EPDResourceStore:{...api,ResourceClient:Client},
    confirm(message){prompts.push(message);return options.confirm!==false;}};
  context.EpdFirmwarePolicy = firmwarePolicy;
  vm.runInNewContext(fs.readFileSync(require.resolve('../html/js/resource_installer.js'),'utf8'),context);
  const installer=context.ExternalResourceInstaller;
  installer.bind({});
  return {memory,node,calls,uploads,prompts,context,installer,partition,pack};
}

test('erase client negotiates support and sends bounded 16-bit IDs with sector offsets',async()=>{
  const packets=[];
  const client=new api.ResourceClient(async packet=>{packets.push(packet);client.receive(Uint8Array.of(0xa7,packet[1],packet[2],0));});
  await assert.rejects(client.eraseSector(0,0),/删除能力/);
  client.capabilities={erase:true};
  for(const [id,offset] of [[-1,0],[65536,0],[1,1],[0,-4096],[0,524288],[0,1.5]])
    await assert.rejects(client.eraseSector(id,offset),/范围/);
  assert.equal(packets.length,0);
  await client.eraseSector(300,4096);
  assert.deepEqual(Array.from(packets[0]),[0x26,11,0,44,1,0,16,0,0]);
  await client.eraseSector(0,520192);
  assert.equal(new DataView(packets[1].buffer).getUint32(5,true),520192);
});

test('deletion requires the reported capability and cancelled confirmation never sends erase or enter',async()=>{
  for(const options of [{legacy:true},{confirm:false},{active:true}]){
    const f=fixture(options), before=f.memory.slice();
    await f.installer.refresh();f.node('externalResourceErasePartition').value='6';
    if(options.legacy) assert.equal(f.node('externalResourceEraseAll').disabled,true);
    await f.installer.erase();
    assert.deepEqual(f.memory,before);assert.equal(f.calls.length,0);
    assert.match(f.node('externalResourceStatus').textContent,options.legacy?/资源删除或即时应用能力/:options.active?/未完成资源事务/:/取消/);
  }
});

test('every data/system/reserved partition is erased completely while adjacent bytes remain unchanged',async()=>{
  for(const id of [2,3,4,5,6,7,8,9]){
    const f=fixture(),p=f.partition(id),before=f.memory.slice();
    f.node('externalResourceErasePartition').value=String(id);
    await f.installer.erase();
    assert.ok(f.memory.subarray(p.offset,p.offset+p.size).every(n=>n===255));
    assert.deepEqual(f.memory.subarray(0,p.offset),before.subarray(0,p.offset));
    assert.deepEqual(f.memory.subarray(p.offset+p.size),before.subarray(p.offset+p.size));
    const erases=f.calls.filter(c=>c.op===11);
    assert.equal(erases.length,p.size/4096);assert.ok(erases.every(c=>c.id===id));
    assert.equal(f.prompts.length,1);assert.match(f.prompts[0],/无法撤销/);
    assert.match(f.node('externalResourceStatus').textContent,/已擦除并校验/);
    assert.equal(f.node('externalResourceProgress').value,1);
    assert.equal(f.installer.busy(),false);
  }
});

test('table deletion and full-chip removal work even without a valid table, followed by full reinstall',async()=>{
  const f=fixture();f.node('externalResourceErasePartition').value='1';
  await f.installer.erase();assert.throws(()=>f.pack(1));assert.doesNotThrow(()=>f.pack(7));
  assert.equal(f.node('externalResourceEraseAll').disabled,false);
  await f.installer.erase(true);
  assert.equal(f.calls.filter(c=>c.op===11&&c.id===0).length,128);
  assert.ok(f.memory.every(n=>n===255));
  assert.match(f.prompts[1],/全部外置 Flash.*配置.*标注.*待办.*字库.*保留区/);
  await f.installer.loadBundle(base.packages);await f.installer.install();
  assert.deepEqual(f.uploads,[1,5,6,7,2,3,9]);
  for(const item of base.packages)assert.deepEqual(f.pack(item.pack.id).payload,item.pack.payload);
});

test('normal install restores only missing personal partitions and retains the other records',async()=>{
  for(const id of [2,3,9]){
    const f=fixture();f.node('externalResourceErasePartition').value=String(id);
    const originals=new Map(base.packages.map(item=>[item.pack.id,item.pack.payload.slice()]));
    await f.installer.erase();await f.installer.loadBundle(base.packages);await f.installer.install();
    assert.deepEqual(f.uploads,[id]);
    for(const [pid,bytes]of originals)assert.deepEqual(f.pack(pid).payload,bytes);
  }
});

test('reconnecting to erased Flash reads status without writes and explains how to restore resources',async()=>{
  const f=fixture();
  await f.installer.erase(true);
  f.installer.disconnect();f.installer.bind({});
  const operations=f.calls.length;
  await f.installer.refresh();
  assert.equal(f.calls.length,operations);assert.equal(f.uploads.length,0);
  assert.ok(f.memory.every(n=>n===255));
  assert.match(f.node('externalResourceStatus').textContent,/显示资源尚未就绪.*连接不会自动重装资源.*发送资源到设备/);
  assert.match(f.node('externalResourceUsage').textContent,/重新生成并载入/);
  assert.equal(f.node('externalResourceInstall').disabled,true);
  await f.installer.loadBundle(base.packages);
  assert.equal(f.node('externalResourceInstall').disabled,false);
  await f.installer.install();
  assert.deepEqual(f.uploads,[1,5,6,7,2,3,9]);
});

test('partial erase failure reports uncertainty and stops subsequent sectors',async()=>{
  const f=fixture({failAt:4096});f.node('externalResourceErasePartition').value='8';
  await f.installer.erase();
  assert.equal(f.calls.filter(c=>c.op===11).length,2);
  assert.match(f.node('externalResourceStatus').textContent,/删除未完成.*部分数据可能已删除.*校验失败/);
  assert.equal(f.installer.busy(),false);
});

test('a late erase ACK cannot continue deleting or change the state of a new connection',async()=>{
  const started=deferred(),ack=deferred();
  const f=fixture({beforeErase:async()=>{started.resolve();await ack.promise;}});
  const work=f.installer.erase(true);await started.promise;
  f.installer.disconnect();assert.match(f.node('externalResourceStatus').textContent,/部分数据可能已删除/);
  f.installer.bind({});f.node('externalResourceStatus').textContent='new connection';
  ack.resolve();await work;
  assert.equal(f.calls.filter(c=>c.op===11).length,1);
  assert.equal(f.node('externalResourceStatus').textContent,'new connection');
  assert.equal(f.installer.busy(),false);
});

test('erase holds the device lock and disables overlapping actions until completion',async()=>{
  const started=deferred(),ack=deferred();
  const f=fixture({beforeErase:async()=>{started.resolve();await ack.promise;}});
  f.context.DeviceOperations={current:null,begin(){assert.equal(this.current,null);return this.current={};},
    assert(owner){assert.equal(owner,this.current);},end(owner){assert.equal(owner,this.current);this.current=null;}};
  const work=f.installer.erase(true);await started.promise;
  for(const id of ['externalResourceInstall','externalResourceErase','externalResourceEraseAll','externalResourceErasePartition','externalResourceReadback'])
    assert.equal(f.node(id).disabled,true);
  const count=f.calls.length;await f.installer.refresh();assert.equal(f.calls.length,count);
  ack.resolve();await work;assert.equal(f.context.DeviceOperations.current,null);
  assert.equal(f.installer.busy(),false);
});

test('failed activation after verified deletion reports the actual erased result',async()=>{
  const f=fixture({leaveFail:true});f.node('externalResourceErasePartition').value='6';
  await f.installer.erase();
  assert.match(f.node('externalResourceStatus').textContent,/已擦除并校验.*应用.*失败/);
  assert.ok(f.memory.subarray(f.partition(6).offset,f.partition(6).offset+65536).every(n=>n===255));
  assert.equal(f.installer.busy(),false);
});

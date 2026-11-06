'use strict';
const {test} = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs'), vm = require('node:vm');

function fixture() {
  let now = 0, nextTimer = 0;
  const timers = new Map(), packets = [];
  const context = {Uint8Array, DataView, module:{exports:{}}, Date:{now:()=>now},
    setTimeout(callback, delay) { const id=++nextTimer;timers.set(id,{at:now+delay,callback});return id; },
    clearTimeout(id) { timers.delete(id); },
  };
  vm.runInNewContext(fs.readFileSync(require.resolve('../html/js/resource_store.js'),'utf8'),context);
  const client = new context.module.exports.ResourceClient(async packet=>packets.push(packet.slice()));
  const settle = async()=>{for(let i=0;i<16;i++)await Promise.resolve();};
  const advance = async milliseconds => {
    const end=now+milliseconds;
    while(true) {
      const next=[...timers.entries()].sort((a,b)=>a[1].at-b[1].at)[0];
      if(!next||next[1].at>end)break;
      now=next[1].at;timers.delete(next[0]);next[1].callback();await settle();
    }
    now=end;await settle();
  };
  const reply = (code=0) => {
    const packet=packets.at(-1);client.receive(Uint8Array.of(0xa7,packet[1],packet[2],code));
  };
  return {client,packets,timers,advance,settle,reply};
}

test('activation accepts a valid acknowledgement after the old 15-second retry budget',async()=>{
  const f=fixture();let completed=false;
  const work=f.client.leave().finally(()=>{completed=true;});work.catch(()=>{});
  await f.settle();await f.advance(20000);
  assert.equal(completed,false);assert.equal(f.packets.length,1);
  f.reply();assert.equal((await work)[3],0);
  assert.equal(f.timers.size,0);
});

test('offline application also waits for a slow activation acknowledgement',async()=>{
  const f=fixture();const work=f.client.runOffline();work.catch(()=>{});
  await f.settle();await f.advance(20000);
  assert.equal(f.packets.length,1);
  f.reply();assert.equal((await work)[3],0);
});

test('post-activation requests tolerate a blocking refresh even after an immediate status response',async()=>{
  const f=fixture();
  let work=f.client.leave();await f.settle();f.reply();await work;
  work=f.client.request(0);await f.settle();f.reply();await work;
  const before=f.packets.length;
  work=f.client.request(6,Uint8Array.of(77,73,71,49));work.catch(()=>{});
  await f.settle();await f.advance(35000);
  assert.equal(f.packets.length,before+2); // One bounded retry, same token/body.
  assert.deepEqual(f.packets.at(-1),f.packets.at(-2));
  f.reply();assert.equal((await work)[3],0);
  await f.advance(90000);
  work=f.client.request(1);await f.settle();
  assert.equal([...f.timers.values()][0].at,130000);
  f.reply();await work;
});

test('a genuinely missing activation ACK still fails within 90 seconds and releases the client',async()=>{
  const f=fixture();const work=f.client.leave();work.catch(()=>{});
  await f.settle();await f.advance(90000);
  await assert.rejects(work,/资源请求超时.*应用资源/);
  assert.equal(f.packets.length,3);assert.equal(f.client.busy,false);
  assert.equal(f.timers.size,0);
});

test('ordinary timeouts and explicit activation errors keep their original failure behavior',async()=>{
  const f=fixture();let work=f.client.request(1);work.catch(()=>{});
  await f.settle();await f.advance(15000);
  await assert.rejects(work,/资源请求超时/);assert.equal(f.packets.length,3);
  work=f.client.leave();await f.settle();f.reply(1);
  await assert.rejects(work,error=>error.device&&error.code===1);
  work=f.client.request(1);await f.settle();
  assert.equal([...f.timers.values()][0].at,20000);
  f.reply();await work;
});

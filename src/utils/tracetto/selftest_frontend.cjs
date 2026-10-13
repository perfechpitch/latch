// 运行实际 app.js 的事件处理，验证虚拟目录和空行缓存；无需浏览器或第三方依赖。
// node src/utils/tracetto/selftest_frontend.cjs
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

class Element {
  constructor() {
    this.style = {}; this.children = []; this.handlers = {};
    this.clientWidth = 800; this.clientHeight = 600; this.scrollTop = 0;
    this.classList = { add() {} };
  }
  set textContent(value) { this.text = value; this.children = []; }
  get textContent() { return this.text || ''; }
  appendChild(child) { this.children.push(child); }
  addEventListener(name, callback) { this.handlers[name] = callback; }
  setAttribute(name, value) { this[name] = value; }
  getContext() { return new Proxy({}, { get: () => () => {} }); }
}

async function main() {
  const elements = new Map(), frames = [], timers = new Map();
  let nextTimer = 1, requests = 0;
  const taskInfo = {available:true,chip:0,core:0,harvest:0,logical:0,
    report_url:'http://127.0.0.1:8901/report.html',tasks:[
      {fn:'broadcast_reissue_t1',unit:'DTE',native_task_id:1,pcode_task_id:1,
       task_type:1,op:'broadcast',pcode_line:103,timings:{}},
      {fn:'gemv_t2',unit:'MU',native_task_id:2,pcode_task_id:2,op:'gemv',
       pcode_line:139,pcode_call:'gemv(input_data=fc13_in)',data_flow:'MM x CM -> CM',
       timings:{'MU-Core':[[348,357,179,2]],'MU-DSA':[[360,531,179,2]]}},
      {fn:'pcie_sw_data_in',unit:'DTE',native_task_id:63,pcode_task_id:-1,
       datain:true,op:'pcie_sw_data_in',pcode_line:58,
       timings:{'DTE-Core':[[20,40,179,63]],'DTE-DSA':[[44,52,179,63]]}},
    ]};
  const get = (id) => {
    if (!elements.has(id)) elements.set(id, new Element());
    return elements.get(id);
  };
  const sandbox = {
    document: { getElementById: get, createElement: () => new Element() },
    window: { devicePixelRatio: 1, addEventListener() {} },
    requestAnimationFrame: (callback) => { frames.push(callback); return frames.length; },
    setTimeout: (callback) => { const id = nextTimer++; timers.set(id, callback); return id; },
    clearTimeout: (id) => timers.delete(id),
    AbortController, TextDecoder,
    fetch: async (url) => {
      if (url === '/api/init') return new Promise(() => {});
      if (url.startsWith('/api/tasks')) return {ok:true,json:async()=>taskInfo};
      requests++;
      const params = new URL(url, 'http://localhost').searchParams;
      const header = Buffer.from(JSON.stringify({ build: 'test', lanes: [],
        t0: Number(params.get('t0')), t1: Number(params.get('t1')) }));
      const frame = Buffer.alloc(8 + header.length);
      frame.write('TCW1'); frame.writeUInt32LE(header.length, 4); header.copy(frame, 8);
      return { ok: true, status: 200,
        arrayBuffer: async () => frame.buffer.slice(frame.byteOffset, frame.byteOffset + frame.length) };
    },
  };
  const context = vm.createContext(sandbox);
  const run = (code) => vm.runInContext(code, context);
  vm.runInContext(fs.readFileSync(path.join(__dirname, 'web/app.js'), 'utf8'), context);
  run(`S.build = 'test'; S.t_end = 10000; S.view = {t0:0,t1:1000};
    S.rows = Array.from({length:10}, (_,i) => 'row'+i);
    S.row_parts = S.rows.map((_,i) => [{lane:i,color:i,unit:'unit'}]);
    S.chips = Array.from({length:10}, (_,chip) => [chip,
      Array.from({length:10}, (_,core) => [core,1,(chip*10+core)*10])]);
    S.lane_n = Array(1000).fill(1);
    S.expanded = new Set(Array.from({length:100}, (_,i) => i*10));
    buildEntries(); renderTree();`);
  const scroll = get('scroll'), tree = get('side-inner');
  const first = tree.children[0];
  run('scheduleDraw()'); frames.splice(0).forEach((callback) => callback());
  assert.equal(tree.children[0], first, '同一视口重绘不应重建目录 DOM');

  for (const top of [800, 1800, 6000, 18000, 0]) {
    scroll.scrollTop = top; scroll.handlers.scroll();
    frames.splice(0).forEach((callback) => callback());
    const start = Math.max(0, Math.floor(Math.max(0, top - 20) / 20) - 2);
    const end = Math.min(run('S.entries.length'), Math.ceil((Math.max(0, top - 20) + 600) / 20) + 2);
    assert.equal(tree.children[0].style.top, start * 20 + 'px', '滚动必须更新目录起始行');
    assert.equal(tree.children.length, end - start, '目录应覆盖整个可见视口');
    assert.ok(tree.children.length <= 35, '展开 100 个 core 后 DOM 仍只渲染可见条目');
  }
  timers.clear();
  await run('ensureData()');
  const initial = requests;
  await run('ensureData()');
  assert.equal(requests, initial, '空行不应重复请求');
  run('S.view = {t0:5000,t1:6000}');
  await run('ensureData()');
  assert.equal(requests, initial + 1, '换窗口需要一次新请求');
  await run('ensureData()');
  assert.equal(requests, initial + 1, '换窗口后空行覆盖范围必须更新');
  run(`S.rows=['TS-DTE','TS-MU','TS-VU','DTE-Core','DTE-DSA','MU-Core','MU-DSA','VU-Core','VU-DSA-ISQ','VU-DSA'];
    S.chips=[[0,[[0,1,0]]]]; S.expanded=new Set([0]);
    S.row_parts=S.rows.map((name,i)=>[{lane:[0,1,2,3,6,5,8,4,9,7][i],color:i,
      unit:name.startsWith('TS-')?name:(name.includes('Core')?'CORE-':'DSA-')+name.split('-')[0]}]);
    buildEntries();`);
  await run(`showProps({chip:0,core:0,row:6,lane:8,unit:'DSA-MU',seg:{t0:360,t1:531,user:179,task:2}})`);
  const current = get('task-current'), chain = get('task-chain');
  assert.ok(current.children.some(n=>n.textContent.includes('gemv_t2')), '点击 MU 块显示对应函数');
  assert.equal(chain.children[1]['aria-pressed'], 'true', '整条链高亮当前任务');
  assert.ok(chain.children[0].children.at(-1).textContent.includes('条件转发'), '未执行分支需明确标记');
  assert.ok(current.children.at(-1).href.endsWith('#chip=0&core=0&harvest=0&task=2'), '三视图链接定位物理核和 pcode 任务');
  assert.equal(run("labelOf('DSA-MU',{t0:360,t1:531,user:179,task:2},{chip:0,core:0})").split(' · ')[0], 'gemv_t2', '波形块显示任务函数名');
  assert.equal(run("selectedTask({chip:0,core:0},{unit:'CORE-MU'},{user:179,task:2})"), false);
  run('S.sel={lane:8,row:6,seg:{user:179,task:2}}');
  assert.equal(run("selectedTask({chip:0,core:0},{unit:'CORE-MU'},{user:179,task:2})"), true, '同任务跨 Core/DSA 行高亮');
  assert.equal(run("selectedTask({chip:0,core:0},{unit:'CORE-DTE'},{user:179,task:2})"), false, '不能混淆不同执行单元');
  chain.children[0].onclick();
  assert.ok(current.children.some(n=>n.textContent.includes('没有此任务')), '点未执行分支不能显示错误的时间');
  await run(`showProps({chip:0,core:0,row:4,lane:6,unit:'DSA-DTE',seg:{t0:44,t1:52,user:179,task:63}})`);
  assert.ok(current.children.some(n=>n.textContent.includes('task 63 ↔ pcode task -1')), 'DATAIN 63 映射 pcode -1');
  get('task-close').onclick();
  assert.equal(get('task-panel').hidden,true, '任务链可以收起');
  run(`S.view={t0:0,t1:10000}; S.sel=null;
    S.cache.set(3,{kind:'exact',segs:[{t0:307,t1:309,user:179,task:0}]});`);
  assert.equal(run('pick(t2x(307)+0.5,110).seg.task'),0, '一像素时间块按实际标记命中');
  assert.equal(run('pick(t2x(307)+20,110)'),null, '远离时间块的空白不能误选任务');
  console.log('Tracetto 前端自检通过：100 core 滚动、缓存、任务点击、高亮、条件分支、DATAIN、三视图链接');
}
main().catch((error) => { console.error(error); process.exitCode = 1; });

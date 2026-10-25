// selftest.js -- end-to-end UI check against tools/mock_server.py.
//   python3 tools/mock_server.py --port 8099 &
//   "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome" --headless=new \
//       --virtual-time-budget=60000 --dump-dom http://localhost:8099/selftest.html | grep -A40 selftest-result
'use strict';
(() => {
  const errors = [];
  window.addEventListener('error', e => errors.push(String(e.message)));
  window.addEventListener('unhandledrejection', e => errors.push('rejection: ' + (e.reason && e.reason.message)));
  const sleep = ms => new Promise(r => setTimeout(r, ms));
  const results = [];
  const check = (name, ok, info) => results.push({ name, ok: !!ok, info: info === undefined ? '' : String(info).slice(0, 160) });
  const text = s => (document.querySelector(s) || {}).textContent || '';
  async function waitFor(fn, ms = 8000) {
    for (let t = 0; t < ms; t += 100) { if (fn()) return true; await sleep(100); }
    return false;
  }

  async function run() {
    showTab('connect');
    await waitFor(() => /GoPro 5F78/.test(text('#dev-body')));
    check('page booted, no errors', errors.length === 0, errors.join(' | '));
    check('device listed', /GoPro 5F78/.test(text('#dev-body')), text('#dev-body'));
    check('advert decoded (pairing mode)', /pairing mode/.test(text('#dev-body')), text('#dev-body'));
    document.querySelector('#dev-body button.pri').click();
    await waitFor(() => /13 characteristics/.test(text('#gatt-sum')));
    check('GATT loaded after discovery event', /13 characteristics/.test(text('#gatt-sum')), text('#gatt-sum'));
    document.querySelector('#btn-setup').click();
    await waitFor(() => /Done|camera never|not discovered/.test(text('#setup-log')), 30000);
    const setup = text('#setup-log');
    check('setup: HW info success', /Get Hardware Info #1: Get Hardware Info -> Success/.test(setup), setup);
    check('setup: version 2.0', /"major":2,"minor":0/.test(setup), setup);
    check('setup: capabilities protobuf', /supports_2byte_ids/.test(setup), setup);
    check('setup: done', /Done/.test(setup), setup);
    check('HW panel shows model', /HERO12 Black/.test(text('#hw')), text('#hw'));

    showTab('settings'); await sleep(800);
    const srow = [...document.querySelectorAll('#settings-body tr')].find(r => r.cells[0].textContent === '2');
    check('settings: resolution current = 4K', srow && /4K \(1\)/.test(srow.cells[2].textContent), srow && srow.cells[2].textContent);
    check('settings: filtered to HERO12', /HERO12 Black/.test(text('#s-model')), text('#s-model'));
    // change resolution via the UI's own Set button
    srow.querySelector('select').value = '9';
    srow.querySelector('button').click();
    await sleep(1500);
    const srow2 = [...document.querySelectorAll('#settings-body tr')].find(r => r.cells[0].textContent === '2');
    check('settings: set -> push updates current', srow2 && /1080 \(9\)/.test(srow2.cells[2].textContent), srow2 && srow2.cells[2].textContent);

    showTab('statuses'); await sleep(800);
    const st = [...document.querySelectorAll('#status-body tr')].find(r => r.cells[0].textContent === '70');
    check('statuses: battery 85', st && st.cells[2].textContent === '85', st && st.cells[2].textContent);

    showTab('commands');
    selectOp(OPS.find(o => o.n === 'Set Shutter')); await sleep(200);
    check('commands: preview', /GP-0072: 01 01 01/.test(text('#op-prev')), text('#op-prev'));
    document.querySelector('#op-form button.pri').click(); await sleep(1500);
    check('commands: response shown', /Set Shutter -> Success/.test(text('#op-rsp')), text('#op-rsp'));
    selectOp(OPS.find(o => o.n === 'Read WiFi AP SSID')); await sleep(200);
    document.querySelector('#op-form button.pri').click(); await sleep(1500);
    check('commands: GATT read decoded', /GP24500000/.test(text('#op-rsp')), text('#op-rsp'));
    selectOp(OPS.find(o => o.n === 'Get Available Presets')); await sleep(200);
    check('commands: protobuf preview', /GP-0076: f5 72 08 01 08 02 18 01 20 01/.test(text('#op-prev')), text('#op-prev'));

    showTab('log'); await sleep(500);
    check('log has decoded messages', document.querySelectorAll('#log .le').length > 10, document.querySelectorAll('#log .le').length);
    showTab('firmware'); await sleep(800);
    check('firmware: other slot = original', /fc_cam_bridge/.test(text('#boot-other')), text('#boot-other'));
    check('no runtime errors at end', errors.length === 0, errors.join(' | '));

    const pass = results.every(r => r.ok);
    const pre = document.createElement('pre');
    pre.id = 'selftest-result';
    pre.textContent = (pass ? 'SELFTEST PASS' : 'SELFTEST FAIL') + '\n' + results.map(r => `${r.ok ? 'ok  ' : 'FAIL'} ${r.name}${r.ok ? '' : '  :: ' + r.info}`).join('\n');
    document.body.append(pre);
    document.title = pass ? 'SELFTEST PASS' : 'SELFTEST FAIL';
  }
  window.addEventListener('load', () => run().catch(e => {
    const p = document.createElement('pre'); p.id = 'selftest-result';
    p.textContent = 'SELFTEST CRASH ' + e.stack + '\n' + results.map(r => `${r.ok ? 'ok  ' : 'FAIL'} ${r.name}${r.ok ? '' : '  :: ' + r.info}`).join('\n');
    document.body.append(p);
  }));
})();

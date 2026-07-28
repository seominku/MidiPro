#!/usr/bin/env node
// =============================================================
// MidiPro - mcp/test-midipro-mcp.mjs
// MCP 서버를 실제로 띄워 stdio로 대화하며 검증한다.
//
// 왜 프로세스를 진짜 띄우나 (Rule 6):
//   함수만 직접 부르면 JSON-RPC 프레이밍·stdout 오염 같은 진짜 실패
//   모드를 못 잡는다. 클라이언트가 하는 그대로 줄바꿈 JSON을 주고받는다.
//
// 실행: node mcp/test-midipro-mcp.mjs
// =============================================================

import { spawn } from 'node:child_process';
import { mkdtempSync, readFileSync, writeFileSync, existsSync, rmSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { tmpdir } from 'node:os';
import path from 'node:path';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const SERVER = path.join(HERE, 'midipro-mcp.mjs');

let passed = 0, failed = 0;
const check = (cond, label) => {
    if (cond) { passed++; console.log(`  [OK] ${label}`); }
    else { failed++; console.log(`  [FAIL] ${label}`); }
};

// --- 서버와 대화하는 최소 클라이언트 -------------------------------
class Client {
    constructor() {
        this.proc = spawn(process.execPath, [SERVER], { stdio: ['pipe', 'pipe', 'pipe'] });
        this.buf = '';
        this.pending = new Map();
        this.nextId = 1;
        this.stderr = '';
        this.stdoutRaw = '';
        this.proc.stdout.setEncoding('utf8');
        this.proc.stderr.setEncoding('utf8');
        this.proc.stderr.on('data', (d) => { this.stderr += d; });
        this.proc.stdout.on('data', (d) => {
            this.stdoutRaw += d;
            this.buf += d;
            let nl;
            while ((nl = this.buf.indexOf('\n')) >= 0) {
                const line = this.buf.slice(0, nl).trim();
                this.buf = this.buf.slice(nl + 1);
                if (!line) continue;
                const msg = JSON.parse(line); // 파싱 실패 = stdout 오염 = 테스트 실패
                const p = this.pending.get(msg.id);
                if (p) { this.pending.delete(msg.id); p(msg); }
            }
        });
    }
    request(method, params) {
        const id = this.nextId++;
        return new Promise((resolve, reject) => {
            const timer = setTimeout(() => reject(new Error(`시간 초과: ${method}`)), 15000);
            this.pending.set(id, (m) => { clearTimeout(timer); resolve(m); });
            this.proc.stdin.write(JSON.stringify({ jsonrpc: '2.0', id, method, params }) + '\n');
        });
    }
    notify(method, params) {
        this.proc.stdin.write(JSON.stringify({ jsonrpc: '2.0', method, params }) + '\n');
    }
    async call(name, args) {
        const r = await this.request('tools/call', { name, arguments: args });
        return { text: r.result?.content?.[0]?.text ?? '', isError: !!r.result?.isError, raw: r };
    }
    close() { this.proc.stdin.end(); this.proc.kill(); }
}

// ------------------------------------------------------------------
const dir = mkdtempSync(path.join(tmpdir(), 'mcptest-'));
const proj = path.join(dir, 'test.midipro');
const c = new Client();

try {
    console.log('--- 프로토콜 ---');
    const init = await c.request('initialize', {
        protocolVersion: '2025-06-18', capabilities: {}, clientInfo: { name: 'test', version: '1' },
    });
    check(init.result?.serverInfo?.name === 'midipro', 'initialize가 서버 정보를 돌려준다');
    check(init.result?.capabilities?.tools !== undefined, 'tools 기능을 광고한다');
    c.notify('notifications/initialized');

    const pong = await c.request('ping', {});
    check(pong.result !== undefined && !pong.error, 'ping에 응답한다');

    const list = await c.request('tools/list', {});
    const names = (list.result?.tools ?? []).map((t) => t.name);
    check(names.length === 9, `도구 9개를 노출한다 (실제 ${names.length}개)`);
    check(names.every((n) => n.startsWith('midipro_')), '도구 이름이 모두 midipro_ 로 시작한다');
    check((list.result?.tools ?? []).every((t) => t.inputSchema?.type === 'object'),
          '모든 도구가 object 스키마를 갖는다');

    const bad = await c.request('nonexistent/method', {});
    check(bad.error?.code === -32601, '모르는 메서드에 -32601을 준다');

    console.log('--- 프로젝트 만들기 ---');
    let r = await c.call('midipro_create_project', {
        path: proj, bpm: 100, tracks: [
            { name: '피아노' }, { name: '드럼', type: 'drum' }, { name: '기타 연습', type: 'guitar' },
        ],
    });
    check(!r.isError && existsSync(proj), '프로젝트 파일이 만들어진다');
    let text = readFileSync(proj, 'utf8');
    check(text.startsWith('midipro_project 1\n'), '첫 줄이 midipro_project 1');
    check(/^bpm 100$/m.test(text), 'bpm이 기록된다');
    check(/^ppqn 480$/m.test(text), 'ppqn 기본값 480');
    check(/^track 9 0 드럼$/m.test(text), '드럼 트랙이 채널 9(=MIDI 10)로 만들어진다');
    check(/^tgtr 1$/m.test(text), '기타 트랙에 tgtr 1이 붙는다');
    check(text.includes('[end]'), '[end]로 끝난다');
    check(!text.includes('\r'), 'CRLF 없이 LF만 쓴다 (앱과 동일)');

    r = await c.call('midipro_create_project', { path: proj });
    check(r.isError && /이미 있는 파일/.test(r.text), '덮어쓰기 보호가 동작한다');

    console.log('--- 노트 찍기 ---');
    r = await c.call('midipro_add_notes', {
        path: proj, track: '피아노',
        notes: [
            { pitch: 'C4', start: 0, duration: 1 },
            { pitch: 'E4', start: 1, duration: 1, velocity: 90 },
            { pitch: 'G4', start: 2, duration: 2 },
            { pitch: 60, start: 4, duration: 1 },
        ],
    });
    check(!r.isError, '이름으로 트랙을 지정해 노트를 찍는다');
    text = readFileSync(proj, 'utf8');
    check(/^ev 0 144 60 100$/m.test(text), 'C4 노트온이 tick 0, status 144(0x90), 노트 60');
    check(/^ev 480 128 60 0$/m.test(text), '노트오프가 480틱(1박)에 status 128(0x80)');
    check(/^ev 480 144 64 90$/m.test(text), 'E4가 1박에 세기 90으로 들어간다');
    check(existsSync(proj + '.bak'), '편집 시 .bak 백업을 남긴다');

    console.log('--- 코드 ---');
    r = await c.call('midipro_add_chords', {
        path: proj, track: 0, chords: ['C', 'Am', 'F', 'G7'], startBeat: 8, octave: 3,
    });
    check(!r.isError, '코드 진행을 찍는다');
    text = readFileSync(proj, 'utf8');
    // C3 = 48, E3 = 52, G3 = 55 가 8박(=3840틱)에
    check(/^ev 3840 144 48 80$/m.test(text), 'C 코드의 근음 C3(48)이 8박에');
    check(/^ev 3840 144 52 80$/m.test(text), 'C 코드의 3음 E3(52)');
    check(/^ev 3840 144 55 80$/m.test(text), 'C 코드의 5음 G3(55)');
    // G7 = G3 B3 D4 F4 = 55 59 62 65, 20박 = 9600틱
    check(/^ev 9600 144 65 80$/m.test(text), 'G7의 7음 F4(65)가 들어간다');

    r = await c.call('midipro_add_chords', { path: proj, track: 0, chords: ['G7/B'], startBeat: 24 });
    check(!r.isError && /ev 11520 144 47 /.test(readFileSync(proj, 'utf8')),
          '분수코드 G7/B의 베이스 B2(47)가 근음 아래에 붙는다');

    r = await c.call('midipro_add_chords', { path: proj, track: 0, chords: ['Cwat7'] });
    check(r.isError && /모르는 코드 종류/.test(r.text), '모르는 코드는 친절한 오류를 준다');

    console.log('--- 드럼 ---');
    r = await c.call('midipro_add_drums', {
        path: proj, track: '드럼',
        pattern: { kick: 'x---x---', snare: '----x---', hat: 'xxxxxxxx' },
        repeat: 2,
    });
    check(!r.isError, '드럼 패턴을 찍는다');
    text = readFileSync(proj, 'utf8');
    check(/^ev 0 153 36 100$/m.test(text), '킥이 채널 10(status 153=0x99), 노트 36');
    check(/^ev 480 153 38 100$/m.test(text), '스네어가 1박(480틱), 노트 38');
    check(/^ev 120 153 42 100$/m.test(text), '하이햇이 16분음표 간격(120틱)으로');
    check(/^ev 960 153 36 100$/m.test(text), '2회 반복이 이어 붙는다 (2박=960틱에 킥)');

    r = await c.call('midipro_add_drums', { path: proj, track: '드럼', pattern: { kick: 'x-9-' }, startBeat: 16 });
    check(!r.isError && /ev 7920 153 36 127/.test(readFileSync(proj, 'utf8')),
          '숫자 9가 최대 세기(127)로 들어간다');

    r = await c.call('midipro_add_drums', { path: proj, track: '드럼', pattern: { 우주선: 'x---' } });
    check(r.isError && /모르는 드럼 악기/.test(r.text), '모르는 드럼 이름은 오류를 준다');

    console.log('--- 템포 / 읽기 ---');
    r = await c.call('midipro_set_tempo', { path: proj, bpm: 128 });
    check(!r.isError && /^bpm 128$/m.test(readFileSync(proj, 'utf8')), '템포를 바꾼다');

    r = await c.call('midipro_read_project', { path: proj });
    const info = JSON.parse(r.text);
    check(info.bpm === 128 && info.ppqn === 480, '읽기가 템포/ppqn을 돌려준다');
    check(info.trackCount === 3, '트랙 3개를 본다');
    check(info.tracks[1].isDrum === true, '드럼 트랙을 드럼으로 인식한다');
    check(info.tracks[2].isGuitar === true, '기타 트랙을 기타로 인식한다');
    // 노트 4 + C(3) + Am(3) + F(3) + G7(4) + G7/B(4음 + 베이스 1) = 22
    check(info.tracks[0].noteCount === 4 + 3 + 3 + 3 + 4 + 5,
          `피아노 트랙 노트 수가 맞는다 (${info.tracks[0].noteCount}개)`);
    check(info.totalNotes > 0 && info.tracks[1].noteCount > 0, '드럼 노트도 센다');

    r = await c.call('midipro_read_project', { path: proj, includeNotes: true, track: 0 });
    const one = JSON.parse(r.text);
    const first = one.tracks[0].notes?.[0];
    check(first?.pitch === 'C4' && first?.startBeat === 0 && first?.durationBeats === 1,
          '노트 목록이 음이름·박·길이를 되돌려준다');
    check(one.tracks[1].notes === undefined, 'track 지정 시 그 트랙만 나열한다');

    console.log('--- 트랙 추가 ---');
    r = await c.call('midipro_add_track', { path: proj, name: '베이스', channel: 2 });
    check(!r.isError, '트랙을 추가한다');
    r = await c.call('midipro_read_project', { path: proj });
    check(JSON.parse(r.text).trackCount === 4, '트랙이 4개가 된다');

    console.log('--- 모르는 내용 보존 (가장 중요) ---');
    // 이 서버가 이해하지 못하는 줄들을 넣고, 편집 후에도 살아남는지 본다.
    const rich = path.join(dir, 'rich.midipro');
    writeFileSync(rich, [
        'midipro_project 1',
        'mpe 1',
        'vstinst C:\\VST3\\Surge XT.vst3',
        'mlimiter 1 0 -0.3 120',
        'drumsample 36 C:\\samples\\kick.wav',
        '[song]',
        'bpm 90',
        'ppqn 960',
        'master 1 0 1',
        'marker 1920 후렴',
        'track 0 0 Lead',
        'tvol 0.8 -0.2 1 0.3',
        'tinch 0',
        'tplugin 1 1 0 Surge XT\tC:\\VST3\\Surge XT.vst3',
        'taudio track0_0.wav 0 1 0 0 보컬',
        'tgain 0.9',
        'ev 0 144 72 100',
        '[synth]',
        'wave 2',
        '[midimap]',
        'cc 7 volume',
        '[versions]',
        'vercur 0 1',
        '[end]',
    ].join('\n') + '\n', 'utf8');

    r = await c.call('midipro_add_notes', { path: rich, track: 'Lead', notes: [{ pitch: 'A4', start: 2, duration: 1 }] });
    check(!r.isError, '복잡한 프로젝트도 편집된다');
    const after = readFileSync(rich, 'utf8');
    for (const must of ['mpe 1', 'vstinst C:\\VST3\\Surge XT.vst3', 'mlimiter 1 0 -0.3 120',
                        'drumsample 36 C:\\samples\\kick.wav', 'marker 1920 후렴',
                        'tvol 0.8 -0.2 1 0.3', 'tplugin 1 1 0 Surge XT\tC:\\VST3\\Surge XT.vst3',
                        'taudio track0_0.wav 0 1 0 0 보컬', 'tgain 0.9',
                        '[synth]', 'wave 2', '[midimap]', 'cc 7 volume', '[versions]', 'vercur 0 1'])
        check(after.includes(must), `보존됨: ${must.length > 40 ? must.slice(0, 40) + '...' : must}`);
    check(after.indexOf('taudio') < after.indexOf('tgain'), 'taudio -> tgain 순서가 유지된다');
    check(/^ev 1920 144 69 100$/m.test(after), 'ppqn 960을 반영해 2박 = 1920틱에 찍는다');
    check(/^ev 0 144 72 100$/m.test(after), '원래 있던 노트가 남아 있다');

    console.log('--- 오류 처리 ---');
    r = await c.call('midipro_add_notes', { path: proj, track: 0, notes: [{ pitch: 'H9', start: 0 }] });
    check(r.isError && /음이름/.test(r.text), '잘못된 음이름을 잡는다');
    r = await c.call('midipro_add_notes', { path: proj, track: 99, notes: [{ pitch: 'C4', start: 0 }] });
    check(r.isError && /트랙이 없습니다/.test(r.text), '없는 트랙 번호를 잡는다');
    r = await c.call('midipro_read_project', { path: path.join(dir, 'nope.midipro') });
    check(r.isError && /파일이 없습니다/.test(r.text), '없는 파일을 잡는다');
    const notProj = path.join(dir, 'notproj.midipro');
    writeFileSync(notProj, 'hello world\n', 'utf8');
    r = await c.call('midipro_read_project', { path: notProj });
    check(r.isError && /프로젝트 파일이 아닙니다/.test(r.text), '다른 파일을 거부한다');
    r = await c.call('midipro_add_notes', { path: proj, track: 0, notes: [] });
    check(r.isError, '빈 노트 배열을 거부한다');

    console.log('--- 상태 ---');
    r = await c.call('midipro_status', {});
    const st = JSON.parse(r.text);
    check(!r.isError && typeof st.running === 'boolean', '상태를 돌려준다');
    check(Array.isArray(st.plugins) && Array.isArray(st.recentProjects), '플러그인/최근 목록이 배열');

    console.log('--- stdout 청결 ---');
    check(c.stdoutRaw.split('\n').filter(Boolean).every((l) => { try { JSON.parse(l); return true; } catch { return false; } }),
          'stdout에 JSON 외 다른 출력이 섞이지 않는다');
    check(c.stderr.includes('MCP 서버'), '로그는 stderr로 나간다');

} catch (e) {
    failed++;
    console.log(`  [FAIL] 예외: ${e?.stack ?? e}`);
} finally {
    c.close();
    try { rmSync(dir, { recursive: true, force: true }); } catch {}
}

console.log(`\n${passed}개 통과, ${failed}개 실패`);
if (failed > 0) { console.log('[FAIL] mcp tests failed'); process.exit(1); }
console.log('[OK] mcp tests passed');

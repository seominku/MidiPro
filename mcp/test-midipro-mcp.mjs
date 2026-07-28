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
import { mkdtempSync, mkdirSync, readFileSync, writeFileSync, existsSync, rmSync } from 'node:fs';
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
    constructor(env) {
        this.proc = spawn(process.execPath, [SERVER], {
            stdio: ['pipe', 'pipe', 'pipe'],
            env: env ? { ...process.env, ...env } : process.env,
        });
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
    check(names.length === 17, `도구 17개를 노출한다 (실제 ${names.length}개)`);
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

    console.log('--- 드럼 킷 (샘플 자동 배정) ---');
    // 실제 라이브러리에 기대지 않도록, 아카이브 구조를 흉내낸 가짜 폴더를 만들어
    // MIDIPRO_DRUMLIB로 가리킨다 — 분류/배정 로직만 검사한다.
    const lib = path.join(dir, 'DrumLib', 'Archive 1');
    const mk = (kit, folder, names) => {
        const d = path.join(lib, kit, folder);
        mkdirSync(d, { recursive: true });
        for (const n of names) writeFileSync(path.join(d, n), 'RIFF');
    };
    mk('TR-909', 'Bassdrums', ['Bassdrum-01.wav', 'Bassdrum-02.wav']);
    mk('TR-909', 'Snares', ['Snare-01.wav', 'Snare-02.wav']);
    mk('TR-909', 'Hats', ['ClosedHat-01.wav', 'OpenHat-01.wav']);
    mk('TR-909', 'Toms', ['Tom-01.wav', 'Tom-02.wav', 'Tom-03.wav', 'Tom-04.wav']);
    mk('TR-909', 'Cymbals', ['Crash-01.wav', 'Ride-01.wav']);
    mk('TR-909', 'Perc', ['Clap-01.wav']);
    mk('TR-909', 'Perc2', ['Rim-01.wav']);     // 스네어가 있으니 38번엔 안 걸려야 한다
    mk('TR-909', 'Bass', ['Bottoms Up 01.wav']); // "bottoms"가 tom으로 잡히면 안 된다
    mk('Tiny Box', 'Kick', ['Kick-01.wav']);   // 킥만 있는 빈약한 킷
    // 계열이 섞인 킷: Acoustic 계열로 통일되어야 한다
    mk('Producer', 'All', [
        'Acoustic-Kick-Ac2 Kik.wav', 'Acoustic-Snare-Ac2 Snr.wav',
        'Acoustic-Tom-Ac2 Tom 1.wav', 'Acoustic-Tom-Ac2 Tom 2.wav', 'Acoustic-Tom-Ac2 Tom 3.wav',
        'Acoustic-Cymbal-Ac2 Crash.wav', 'Acoustic-Cymbal-Ac2 Ride.wav',
        'Acoustic-Hat-Ac2 Closed.wav', 'Acoustic-Hat-Ac2 Open.wav',
        'Urban-Tom-PD6 Tom.wav', 'HipHop-Bass-MSXII Bottoms Up.wav',
    ]);

    // 서버를 라이브러리 환경변수와 함께 새로 띄운다
    const c2 = new Client({ MIDIPRO_DRUMLIB: path.join(dir, 'DrumLib') });
    await c2.request('initialize', { protocolVersion: '2025-06-18', capabilities: {}, clientInfo: { name: 't', version: '1' } });

    const dkit = path.join(dir, 'kit.midipro');
    await c2.call('midipro_create_project', { path: dkit, tracks: [{ name: '드럼', type: 'drum' }] });
    await c2.call('midipro_add_drums', {
        path: dkit, track: '드럼',
        pattern: { kick: 'x---x---', snare: '----x---', hat: 'x-x-x-x-', openhat: '-------x',
                   crash: 'x-------', ride: '--x-----', tom1: '------x-', tom2: '-----x--', tom3: '----x---' },
    });

    r = await c2.call('midipro_list_drumkits', {});
    const kits = JSON.parse(r.text);
    check(!r.isError && kits.kitCount === 3, `킷 3개를 찾는다 (${kits.kitCount})`);
    check(kits.kits[0].has.includes('킥') && kits.kits[0].has.includes('오픈 햇'),
          '킷이 가진 악기를 분류해 보여준다');

    r = await c2.call('midipro_set_drumkit', { path: dkit, dryRun: true });
    check(!r.isError && /미리보기/.test(r.text), 'dryRun이 동작한다');
    check(!readFileSync(dkit, 'utf8').includes('drumsample'), 'dryRun은 파일을 고치지 않는다');

    // 오탐/우선순위 검사 (자동 배정의 핵심 품질)
    r = await c2.call('midipro_set_drumkit', { path: dkit, kit: 'TR-909', dryRun: true });
    check(/38 스네어 .*-> Snare-01\.wav/.test(r.text),
          '스네어가 있으면 38번에 림이 아니라 스네어가 걸린다');
    check(!/Bottoms Up/.test(r.text),
          '"Bottoms Up"이 탐으로 오탐되지 않는다 (낱말 단위 매칭)');
    r = await c2.call('midipro_set_drumkit', { path: dkit, kit: 'TR-909', notes: [37], dryRun: true });
    check(/37 림 .*-> Rim-01\.wav/.test(r.text), '37번(림)에는 림 샘플이 걸린다');

    // 약어 인식 + 같은 파일 중복 방지: "Rd"(ride)만 있고 "ride"라는 낱말이 없는 킷
    mk('Abbrev', 'Cym', ['Acoustic-Cymbal-Ac2 Crsh 1.wav', 'Acoustic-Cymbal-Ac2 Rd Bow.wav',
                         'Acoustic-Kick-Ac2 Kik.wav', 'Acoustic-Snare-Ac2 Sn.wav']);
    mk('Abbrev', 'Misc', ['Acoustic-Perc-3rd Take Shaker.wav']); // "3rd"가 ride로 잡히면 안 된다
    const c3 = new Client({ MIDIPRO_DRUMLIB: path.join(dir, 'DrumLib') });
    await c3.request('initialize', { protocolVersion: '2025-06-18', capabilities: {}, clientInfo: { name: 't', version: '1' } });
    const abbrev = path.join(dir, 'abbrev.midipro');
    await c3.call('midipro_create_project', { path: abbrev, tracks: [{ name: '드럼', type: 'drum' }] });
    await c3.call('midipro_add_drums', {
        path: abbrev, track: '드럼',
        pattern: { kick: 'x---', snare: '--x-', crash: 'x---', ride: '--x-' },
    });
    r = await c3.call('midipro_set_drumkit', { path: abbrev, kit: 'Abbrev', dryRun: true });
    check(/36 킥 .*Kik\.wav/.test(r.text), '"Kik" 약어를 킥으로 알아본다');
    check(/38 스네어 .*Sn\.wav/.test(r.text), '"Sn" 약어를 스네어로 알아본다');
    check(/51 라이드 .*Rd Bow\.wav/.test(r.text), '"Rd" 약어를 라이드로 알아본다');
    check(/49 크래시 .*Crsh 1\.wav/.test(r.text), '"Crsh" 약어를 크래시로');
    check(!/3rd Take/.test(r.text), '"3rd"가 라이드로 오탐되지 않는다 (숫자 경계)');
    c3.close();

    r = await c2.call('midipro_set_drumkit', { path: dkit, kit: 'Producer', dryRun: true });
    check(/Acoustic 계열/.test(r.text), '계열이 섞인 킷에서 커버리지 넓은 계열을 고른다');
    check(!/Urban-Tom|MSXII/.test(r.text), '다른 계열 샘플이 섞이지 않는다');
    check(/49 크래시 .*Crash/.test(r.text) && /51 라이드 .*Ride/.test(r.text),
          '크래시와 라이드가 서로 다른 샘플로 걸린다 (막연한 cymbal보다 이름이 정확한 쪽 우선)');

    r = await c2.call('midipro_set_drumkit', { path: dkit, kit: 'Producer', family: 'Urban', dryRun: true });
    check(!r.isError && /Urban 계열/.test(r.text), 'family로 계열을 직접 고를 수 있다');
    r = await c2.call('midipro_set_drumkit', { path: dkit, kit: 'Producer', family: '없는계열' });
    check(r.isError && /있는 계열:/.test(r.text), '없는 계열은 있는 목록을 알려준다');

    r = await c2.call('midipro_set_drumkit', { path: dkit, kit: 'TR-909' });
    check(!r.isError, '드럼 킷을 배정한다');
    const kt = readFileSync(dkit, 'utf8');
    check(/^drumsample 36 .*Bassdrum-01\.wav$/m.test(kt), '킥(36)에 킥 샘플이 걸린다');
    check(/^drumsample 38 .*Snare-01\.wav$/m.test(kt), '스네어(38)에 스네어 샘플');
    check(/^drumsample 42 .*ClosedHat-01\.wav$/m.test(kt), '클로즈드 햇(42)에 closed 샘플');
    check(/^drumsample 46 .*OpenHat-01\.wav$/m.test(kt), '오픈 햇(46)에 open 샘플');
    check(/^drumsample 49 .*Crash-01\.wav$/m.test(kt), '크래시(49)에 크래시');
    check(/^drumsample 51 .*Ride-01\.wav$/m.test(kt), '라이드(51)에 라이드');
    // 탐 3개(45/47/50)는 서로 다른 파일이어야 한다
    const toms = [...kt.matchAll(/^drumsample (4[57]|50) .*(Tom-\d+)\.wav$/gm)].map((m) => m[2]);
    check(toms.length === 3 && new Set(toms).size === 3,
          `탐 3개에 서로 다른 샘플이 걸린다 (${toms.join(', ')})`);
    check(kt.indexOf('drumsample') < kt.indexOf('[song]'), 'drumsample 줄이 헤더(=[song] 앞)에 들어간다');

    r = await c2.call('midipro_set_drumkit', { path: dkit, kit: 'Tiny' });
    check(!r.isError && /킥/.test(r.text), '킷을 지정할 수 있다');
    check(/못 찾아 내장 신디로 남는 노트/.test(r.text), '없는 악기는 내장 신디로 남는다고 알려준다');
    const kt2 = readFileSync(dkit, 'utf8');
    check((kt2.match(/^drumsample /gm) ?? []).length === 1, '다시 배정하면 이전 배정을 걷어낸다');

    r = await c2.call('midipro_set_drumkit', { path: dkit, kit: '없는킷999' });
    check(r.isError && /맞는 드럼머신이 없습니다/.test(r.text), '없는 킷 이름을 잡는다');
    const nodrum = path.join(dir, 'nodrum.midipro');
    await c2.call('midipro_create_project', { path: nodrum, tracks: [{ name: '피아노' }] });
    r = await c2.call('midipro_set_drumkit', { path: nodrum });
    check(r.isError && /드럼 트랙/.test(r.text), '드럼 노트가 없으면 알려준다');
    c2.close();

    console.log('--- 플러그인 (악기/이펙트) ---');
    // 내장 이펙트는 스캔된 VST가 없어도 항상 되므로 그걸로 검사한다.
    r = await c.call('midipro_add_effect', { path: proj, track: '피아노', effect: 'delay' });
    check(!r.isError, '내장 이펙트를 추가한다');
    text = readFileSync(proj, 'utf8');
    check(/^tplugin 0 1 -1 딜레이\tbuiltin:delay$/m.test(text),
          '내장 이펙트가 앱과 같은 형식으로 기록된다 (classIndex -1, builtin: 경로)');
    r = await c.call('midipro_add_effect', { path: proj, track: '피아노', effect: '리버브' });
    check(!r.isError && /딜레이 -> 리버브/.test(r.text), '체인 뒤에 순서대로 붙는다');
    r = await c.call('midipro_add_effect', { path: proj, track: '피아노', effect: '컴프' });
    check(!r.isError && /^tplugin 0 1 -1 컴프레서\tbuiltin:comp$/m.test(readFileSync(proj, 'utf8')),
          '한글 별칭(컴프)도 받는다');

    r = await c.call('midipro_add_effect', { path: proj, track: '피아노', effect: '없는이펙트123' });
    check(r.isError, '모르는 이펙트는 오류를 준다');
    r = await c.call('midipro_set_instrument', { path: proj, track: 0, plugin: 'C:\\없는\\경로.vst3' });
    check(r.isError && /찾을 수 없습니다/.test(r.text), '없는 .vst3 경로를 잡는다');

    // 악기: 가짜 .vst3 경로를 만들어 형식만 검사한다 (진짜 로드는 앱이 한다)
    const fakeVst = path.join(dir, 'FakeSynth.vst3');
    writeFileSync(fakeVst, 'x');
    r = await c.call('midipro_set_instrument', { path: proj, track: '피아노', plugin: fakeVst });
    check(!r.isError, '악기를 얹는다');
    text = readFileSync(proj, 'utf8');
    check(new RegExp(`^tplugin 1 1 -1 FakeSynth\t${fakeVst.replace(/[\\.]/g, '\\$&')}$`, 'm').test(text),
          '악기가 tplugin 1 ... 형식으로 기록된다');
    const fake2 = path.join(dir, 'Other.vst3');
    writeFileSync(fake2, 'x');
    r = await c.call('midipro_set_instrument', { path: proj, track: '피아노', plugin: fake2 });
    check(!r.isError && /기존 악기 교체/.test(r.text), '악기는 트랙당 1개라 교체된다');
    text = readFileSync(proj, 'utf8');
    check((text.match(/^tplugin 1 /gm) ?? []).length === 1, '악기 줄이 하나만 남는다');
    check((text.match(/^tplugin 0 /gm) ?? []).length === 3, '이펙트 3개는 그대로 남는다');

    // tplugin 줄이 taudio 블록 앞에 들어가는지 (tgain이 taudio에 붙어 있어야 한다)
    const withAudio = path.join(dir, 'audio.midipro');
    writeFileSync(withAudio, [
        'midipro_project 1', '[song]', 'bpm 120', 'ppqn 480',
        'track 0 0 Aud', 'tvol 1 0 1 0', 'tinch 0',
        'taudio track0_0.wav 0 1 0 0 보컬', 'tgain 0.9', '[end]',
    ].join('\n') + '\n', 'utf8');
    r = await c.call('midipro_add_effect', { path: withAudio, track: 0, effect: 'eq' });
    const al = readFileSync(withAudio, 'utf8').split('\n');
    check(al.indexOf('tplugin 0 1 -1 EQ\tbuiltin:eq') < al.findIndex((L) => L.startsWith('taudio ')),
          'tplugin이 taudio 앞에 들어간다');
    check(al.findIndex((L) => L.startsWith('tgain ')) - al.findIndex((L) => L.startsWith('taudio ')) === 1,
          'taudio 바로 뒤에 tgain이 붙어 있다 (오디오 클립 정보 보존)');

    r = await c.call('midipro_read_project', { path: proj });
    const pj = JSON.parse(r.text);
    check(pj.tracks[0].plugins.length === 4, '읽기가 플러그인 4개를 보고한다');
    check(pj.tracks[0].plugins.filter((p) => p.kind === 'instrument').length === 1,
          '그중 악기가 1개');

    console.log('--- MIDI 내보내기/가져오기 ---');
    const mid = path.join(dir, 'out.mid');
    r = await c.call('midipro_export_midi', { path: proj, midiPath: mid });
    check(!r.isError && existsSync(mid), '.mid 파일을 내보낸다');
    const mb = readFileSync(mid);
    check(mb.toString('ascii', 0, 4) === 'MThd', 'MThd 헤더로 시작한다');
    check(mb.readUInt32BE(4) === 6, '헤더 길이가 6');
    check(mb.readUInt16BE(8) === 1, '포맷 1로 쓴다');
    check(mb.readUInt16BE(12) === 480, 'division이 프로젝트 ppqn(480)과 같다');
    check(mb.readUInt16BE(10) === 5, '트랙 청크 5개 (템포 + 트랙 4개)');
    check(mb.toString('ascii', 14, 18) === 'MTrk', '첫 청크가 MTrk');
    check(mb.includes(Buffer.from([0xff, 0x2f, 0x00])), 'End of Track 메타가 들어 있다');

    r = await c.call('midipro_export_midi', { path: proj, midiPath: mid });
    check(r.isError && /이미 있는 파일/.test(r.text), '.mid 덮어쓰기 보호가 동작한다');

    // 왕복: 내보낸 .mid를 새 프로젝트로 다시 가져와 노트가 살아있는지 본다
    const rt = path.join(dir, 'roundtrip.midipro');
    await c.call('midipro_create_project', { path: rt, bpm: 120 });
    r = await c.call('midipro_import_midi', { path: rt, midiPath: mid });
    check(!r.isError, '내보낸 .mid를 다시 가져온다');
    const back = JSON.parse((await c.call('midipro_read_project', { path: rt })).text);
    const origInfo = JSON.parse((await c.call('midipro_read_project', { path: proj })).text);
    check(back.totalNotes === origInfo.totalNotes,
          `왕복 후 노트 수가 같다 (원본 ${origInfo.totalNotes} / 왕복 ${back.totalNotes})`);
    check(back.bpm === 128, '가져오기가 MIDI의 템포(128)를 적용한다');
    check(back.tracks.some((t) => t.channel === 9), '드럼 채널(10)이 유지된다');
    check(back.tracks.some((t) => t.name === '피아노'), '트랙 이름 메타가 왕복한다');

    // 노트 하나의 위치·길이가 정확히 살아남는가
    const rtPiano = back.tracks.find((t) => t.name === '피아노');
    const one2 = JSON.parse((await c.call('midipro_read_project',
        { path: rt, includeNotes: true, track: rtPiano.index })).text);
    const n0 = one2.tracks[rtPiano.index].notes[0];
    check(n0.pitch === 'C4' && n0.startBeat === 0 && n0.durationBeats === 1,
          '왕복 후 첫 노트가 C4 / 0박 / 1박 그대로');

    // 한 트랙으로 합치기
    const merge = path.join(dir, 'merge.midipro');
    await c.call('midipro_create_project', { path: merge, tracks: [{ name: '한트랙' }] });
    r = await c.call('midipro_import_midi', { path: merge, midiPath: mid, track: '한트랙', startBeat: 4 });
    check(!r.isError, 'track 지정 시 한 트랙으로 합친다');
    const mg = JSON.parse((await c.call('midipro_read_project', { path: merge })).text);
    check(mg.trackCount === 1, '트랙이 늘지 않는다');
    check(mg.tracks[0].noteCount === origInfo.totalNotes, '모든 노트가 그 트랙에 들어간다');
    check(mg.tracks[0].startBeat === 4, 'startBeat=4 만큼 밀려서 들어간다');

    // 다른 ppqn으로 가져오면 틱이 환산되는가
    const scaled = path.join(dir, 'scaled.midipro');
    await c.call('midipro_create_project', { path: scaled, ppqn: 960 });
    r = await c.call('midipro_import_midi', { path: scaled, midiPath: mid });
    const sc = JSON.parse((await c.call('midipro_read_project', { path: scaled })).text);
    check(sc.ppqn === 960 && sc.totalNotes === origInfo.totalNotes,
          'ppqn 960 프로젝트로 가져와도 노트 수가 같다');
    const scPiano = sc.tracks.find((t) => t.name === '피아노');
    check(scPiano && scPiano.startBeat === 0 && Math.abs(scPiano.endBeat - origInfo.tracks[0].endBeat) < 0.01,
          'ppqn이 달라도 박 위치가 보존된다 (틱 환산)');

    // 러닝 스테이터스: 실제 MIDI 파일이 흔히 쓰지만 앱의 writer는 안 만드는 경로라
    // 손으로 바이트를 짜서 검사한다. 상태 바이트를 생략하고 데이터만 이어 붙인 형태.
    const runmid = path.join(dir, 'running.mid');
    writeFileSync(runmid, Buffer.from([
        0x4d, 0x54, 0x68, 0x64, 0, 0, 0, 6, 0, 0, 0, 1, 0x01, 0xe0, // MThd 포맷0 1트랙 division 480
        0x4d, 0x54, 0x72, 0x6b, 0, 0, 0, 0x11,                       // MTrk 길이 17
        0x00, 0x90, 0x3c, 0x64,   // delta 0: Note On C4 vel100 (상태 바이트 있음)
        0x60, 0x3e, 0x64,         // delta 96: 러닝 스테이터스 -> Note On D4 vel100
        0x00, 0x3c, 0x00,         // delta 0: 러닝 -> C4 vel0 (= Note Off 관례)
        0x60, 0x3e, 0x00,         // delta 96: 러닝 -> D4 vel0
        0x00, 0xff, 0x2f, 0x00,   // End of Track
    ]));
    const runproj = path.join(dir, 'running.midipro');
    await c.call('midipro_create_project', { path: runproj });
    r = await c.call('midipro_import_midi', { path: runproj, midiPath: runmid });
    check(!r.isError, '러닝 스테이터스 MIDI를 가져온다');
    const rn = JSON.parse((await c.call('midipro_read_project',
        { path: runproj, includeNotes: true })).text);
    check(rn.totalNotes === 2, `러닝 스테이터스로 이어 쓴 노트 2개를 읽는다 (${rn.totalNotes}개)`);
    const rnotes = rn.tracks[0].notes;
    check(rnotes?.[0]?.pitch === 'C4' && rnotes[0].startBeat === 0,
          '첫 노트 C4가 0박에');
    check(rnotes?.[1]?.pitch === 'D4' && Math.abs(rnotes[1].startBeat - 0.2) < 0.001,
          '둘째 노트 D4가 96틱(0.2박)에');
    check(Math.abs(rnotes[0].durationBeats - 0.2) < 0.001,
          'velocity 0인 Note On을 Note Off로 인정해 길이가 나온다');

    r = await c.call('midipro_import_midi', { path: rt, midiPath: path.join(dir, 'none.mid') });
    check(r.isError && /MIDI 파일이 없습니다/.test(r.text), '없는 .mid를 잡는다');
    const fake = path.join(dir, 'fake.mid');
    writeFileSync(fake, 'not a midi file at all, really');
    r = await c.call('midipro_import_midi', { path: rt, midiPath: fake });
    check(r.isError && /MIDI 파일이 아닙니다/.test(r.text), 'MThd 없는 파일을 거부한다');

    console.log('--- 앱이 열고 있는 프로젝트 보호 ---');
    // 데이터 폴더를 가짜로 잡아 session.lock / recent.txt 를 직접 만든다.
    // (실제 MidiPro 실행 여부에 좌우되지 않게 — 테스트는 결정적이어야 한다)
    const fakeData = path.join(dir, 'AppData');
    mkdirSync(fakeData, { recursive: true });
    const gproj = path.join(dir, 'guard.midipro');

    const cg = new Client({ MIDIPRO_DATA_DIR: fakeData });
    await cg.request('initialize', { protocolVersion: '2025-06-18', capabilities: {}, clientInfo: { name: 'g', version: '1' } });
    await cg.call('midipro_create_project', { path: gproj, tracks: [{ name: 'T' }] });

    // 세션 없음 -> 아무 경고 없이 그냥 된다
    r = await cg.call('midipro_add_notes', { path: gproj, track: 0, notes: [{ pitch: 'C4', start: 0 }] });
    check(!r.isError && !/경고|주의/.test(r.text), '세션이 없으면 조용히 편집된다');

    // 세션 있음 + 최근 목록 맨 위가 다른 파일 -> 경고만
    writeFileSync(path.join(fakeData, 'session.lock'), 'running');
    writeFileSync(path.join(fakeData, 'recent.txt'), path.join(dir, 'other.midipro') + '\n', 'utf8');
    r = await cg.call('midipro_add_notes', { path: gproj, track: 0, notes: [{ pitch: 'D4', start: 1 }] });
    check(!r.isError && /\[경고\] MidiPro가 실행 중/.test(r.text),
          '앱이 켜져 있고 다른 프로젝트면 경고만 붙고 편집은 된다');
    check(/노트 1개를 찍었습니다/.test(r.text), '경고와 함께 원래 결과도 온다');

    // 세션 있음 + 최근 목록 맨 위가 이 파일 -> 차단
    writeFileSync(path.join(fakeData, 'recent.txt'), gproj + '\n', 'utf8');
    const before2 = readFileSync(gproj, 'utf8');
    r = await cg.call('midipro_add_notes', { path: gproj, track: 0, notes: [{ pitch: 'E4', start: 2 }] });
    check(r.isError && /열고 있는 것 같습니다/.test(r.text), '앱이 그 프로젝트를 열고 있으면 막는다');
    check(readFileSync(gproj, 'utf8') === before2, '막혔을 때 파일이 그대로다');
    check(/force: true/.test(r.text), '넘기는 방법을 알려준다');

    // force로 강행
    r = await cg.call('midipro_add_notes', { path: gproj, track: 0, notes: [{ pitch: 'E4', start: 2 }], force: true });
    check(!r.isError && /\[주의\]/.test(r.text), 'force면 주의 문구와 함께 진행한다');
    check(readFileSync(gproj, 'utf8') !== before2, 'force면 실제로 고쳐진다');

    // 다른 변경 도구도 같은 보호를 받는다 (한 곳에서 거르는지 확인)
    for (const [tool, extra] of [
        ['midipro_set_tempo', { bpm: 100 }],
        ['midipro_add_track', { name: '새트랙' }],
        ['midipro_add_chords', { track: 0, chords: ['C'] }],
        ['midipro_add_drums', { track: 0, pattern: { kick: 'x' } }],
    ]) {
        r = await cg.call(tool, { path: gproj, ...extra });
        check(r.isError && /열고 있는 것 같습니다/.test(r.text), `${tool}도 보호된다`);
    }
    // 읽기 도구는 막히지 않는다
    r = await cg.call('midipro_read_project', { path: gproj });
    check(!r.isError, '읽기는 세션이 있어도 된다');

    r = await cg.call('midipro_status', {});
    const stg = JSON.parse(r.text);
    check(stg.sessionLive === true && stg.likelyOpenProject === gproj,
          'status가 열려 있는 프로젝트를 알려준다');

    // 스키마에 force가 붙어 있는지
    const list2 = await cg.request('tools/list', {});
    const mut = (list2.result?.tools ?? []).filter((t) =>
        ['midipro_add_notes', 'midipro_set_drumkit', 'midipro_add_effect', 'midipro_import_midi'].includes(t.name));
    check(mut.length === 4 && mut.every((t) => t.inputSchema.properties.force),
          '변경 도구 스키마에 force가 붙는다');
    const ro = (list2.result?.tools ?? []).find((t) => t.name === 'midipro_read_project');
    check(!ro.inputSchema.properties.force, '읽기 도구엔 force가 없다');

    // 환경변수로 끌 수 있다
    const cOff = new Client({ MIDIPRO_DATA_DIR: fakeData, MIDIPRO_OPEN_GUARD: 'off' });
    await cOff.request('initialize', { protocolVersion: '2025-06-18', capabilities: {}, clientInfo: { name: 'o', version: '1' } });
    r = await cOff.call('midipro_set_tempo', { path: gproj, bpm: 111 });
    check(!r.isError && !/경고|주의/.test(r.text), 'MIDIPRO_OPEN_GUARD=off로 끌 수 있다');
    cOff.close();
    cg.close();

    console.log('--- 앱 제어 (파이프 없을 때) ---');
    // 실제 앱이 떠 있는지에 좌우되지 않게, 파이프가 없을 때의 동작만 검사한다.
    // (있을 때의 동작은 앱을 실제로 띄워 따로 확인했다)
    r = await c.call('midipro_transport', { action: 'status' });
    if (r.isError) {
        check(/실행 중인 MidiPro를 찾지 못했습니다|제어 통로/.test(r.text),
              '앱이 없으면 이유를 알려준다');
    } else {
        check(/상태: (재생 중|정지)/.test(r.text), '앱이 떠 있으면 상태를 돌려준다');
    }
    r = await c.call('midipro_transport', { action: 'seek' });
    check(r.isError && /beat/.test(r.text), 'seek에 beat가 없으면 잡는다');
    r = await c.call('midipro_transport', { action: 'tempo', bpm: 999 });
    check(r.isError && /20~400|bpm/.test(r.text), 'tempo 범위를 잡는다');
    r = await c.call('midipro_transport', { action: 'open', path: path.join(dir, 'nope.midipro') });
    check(r.isError && /파일이 없습니다/.test(r.text), 'open에 없는 파일을 잡는다');

    // 프리셋: 파일/인자 검사는 앱 없이도 확인된다
    const cp = new Client({ MIDIPRO_DATA_DIR: path.join(dir, 'PresetData') });
    await cp.request('initialize', { protocolVersion: '2025-06-18', capabilities: {}, clientInfo: { name: 'p', version: '1' } });
    r = await cp.call('midipro_preset', { action: 'list' });
    const pl = JSON.parse(r.text);
    check(!r.isError && Array.isArray(pl.saved) && pl.saved.length === 0,
          '보관된 음색이 없으면 빈 목록');
    check(/보관된 음색이 없습니다/.test(pl.note ?? ''), '비어 있으면 어떻게 하는지 알려준다');
    r = await cp.call('midipro_preset', { action: 'save', track: 0 });
    check(r.isError && /name 또는 file/.test(r.text), 'save에 이름이 없으면 잡는다');
    r = await cp.call('midipro_preset', { action: 'load', track: 0, name: '없는음색' });
    check(r.isError && /음색 파일이 없습니다/.test(r.text), 'load에 없는 음색을 잡는다');
    r = await cp.call('midipro_preset', { action: 'save', name: 'x' });
    check(r.isError && /track/.test(r.text), 'track이 없으면 잡는다');
    cp.close();

    const tt = (list.result?.tools ?? []).find((t) => t.name === 'midipro_transport');
    check(tt && tt.inputSchema.properties.action.enum.includes('reload'),
          'transport 스키마에 reload가 있다');
    check(tt && !tt.inputSchema.properties.force, '앱 제어 도구엔 force가 없다 (파일을 안 고친다)');

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

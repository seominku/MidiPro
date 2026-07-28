#!/usr/bin/env node
// =============================================================
// MidiPro - mcp/midipro-mcp.mjs
// MidiPro용 MCP(Model Context Protocol) 서버.
//
// 무엇을 하나:
//   AI 어시스턴트가 .midipro 프로젝트 파일을 직접 만들고 편집할 수 있게
//   도구(tool) 몇 개를 노출한다. 트랙 추가, 노트/코드/드럼 패턴 찍기,
//   템포 변경, 그리고 만든 프로젝트를 MidiPro로 바로 열기까지.
//
// 왜 파일을 직접 다루나 (Rule 1):
//   MidiProConsole은 대화형 메뉴라 CLI 진입점이 없고, GUI 앱에는 아직
//   외부 제어 통로가 없다. .midipro는 사람이 읽을 수 있는 텍스트 포맷이라
//   앱을 전혀 건드리지 않고도 안전하게 왕복할 수 있다. 나중에 실행 중인
//   앱을 실시간 제어하려면 앱 쪽에 IPC를 추가하면 된다(2단계).
//
// 왜 외부 의존성이 없나:
//   MCP stdio 전송은 "줄바꿈으로 구분된 JSON-RPC 2.0"이라 SDK 없이도
//   구현이 짧다. npm 설치 없이 node 하나로 돌아가야 배포·유지가 쉽다.
//
// 중요: stdout은 JSON-RPC 전용이다. 로그는 반드시 stderr로 내보낸다.
// =============================================================

import { existsSync, readFileSync, writeFileSync, renameSync, copyFileSync,
         mkdirSync, readdirSync, openSync, readSync, writeSync, closeSync } from 'node:fs';
import { execFileSync, spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import path from 'node:path';

const SERVER_NAME = 'midipro';
const SERVER_VERSION = '1.0.0';
const HERE = path.dirname(fileURLToPath(import.meta.url));

// ------------------------------------------------------------------
// 작은 유틸
// ------------------------------------------------------------------
const log = (...a) => process.stderr.write(a.join(' ') + '\n');
const clamp = (v, lo, hi) => Math.min(hi, Math.max(lo, v));

function requireNum(v, name, { min = -Infinity, max = Infinity } = {}) {
    const n = Number(v);
    if (!Number.isFinite(n)) throw new Error(`${name}: 숫자가 필요합니다 (받은 값: ${JSON.stringify(v)})`);
    if (n < min || n > max) throw new Error(`${name}: ${min}~${max} 범위여야 합니다 (받은 값: ${n})`);
    return n;
}

// "C4", "F#3", "Bb5" 또는 숫자(0~127) -> MIDI 노트 번호.
// 기준은 MidiPro 피아노 롤과 같은 C4=60 (A0=21, C8=108).
const NOTE_LETTER = { c: 0, d: 2, e: 4, f: 5, g: 7, a: 9, b: 11 };
function noteToNumber(v) {
    if (typeof v === 'number' || /^\d+$/.test(String(v).trim()))
        return clamp(Math.round(Number(v)), 0, 127);
    const m = /^([A-Ga-g])([#b♯♭]*)(-?\d+)$/.exec(String(v).trim());
    if (!m) throw new Error(`음이름을 알 수 없습니다: ${JSON.stringify(v)} (예: C4, F#3, Bb5, 또는 0~127 숫자)`);
    let semi = NOTE_LETTER[m[1].toLowerCase()];
    for (const ch of m[2]) semi += (ch === '#' || ch === '♯') ? 1 : -1;
    const n = (parseInt(m[3], 10) + 1) * 12 + semi;
    if (n < 0 || n > 127) throw new Error(`음이 MIDI 범위를 벗어납니다: ${v} -> ${n}`);
    return n;
}

const PITCH_NAMES = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];
const numberToNote = (n) => `${PITCH_NAMES[n % 12]}${Math.floor(n / 12) - 1}`;

// ------------------------------------------------------------------
// .midipro 파싱/직렬화
//
// 파일 구조:
//   midipro_project 1        <- 1행 고정
//   <헤더 줄들>               <- vstinst, metrovol, mlimiter ...
//   [song] <곡 본문>
//   [synth] / [midimap] / [versions] / [end]
//
// 우리가 이해하지 못하는 줄은 전부 원문 그대로 보존한다 — 이 서버가
// 모르는 기능(오디오 클립, 버전 트리, VST 설정)을 지우지 않기 위해서다.
// ------------------------------------------------------------------
function parseProject(text) {
    const lines = text.split(/\r?\n/);
    if (!/^midipro_project\b/.test(lines[0] ?? ''))
        throw new Error('MidiPro 프로젝트 파일이 아닙니다 (첫 줄이 "midipro_project"가 아님)');

    const header = [lines[0]];
    let i = 1;
    for (; i < lines.length && !lines[i].startsWith('['); i++)
        if (lines[i] !== '') header.push(lines[i]);

    const sections = [];
    let cur = null;
    for (; i < lines.length; i++) {
        const L = lines[i];
        if (L.startsWith('[')) {
            const end = L.indexOf(']');
            cur = { name: end < 0 ? L.slice(1) : L.slice(1, end), lines: [] };
            sections.push(cur);
        } else if (cur && L !== '') {
            cur.lines.push(L);
        }
    }
    const songSec = sections.find((s) => s.name === 'song');
    if (!songSec) throw new Error('[song] 섹션이 없습니다');
    return { header, sections, song: parseSong(songSec.lines) };
}

// [song] 본문 -> { globals, tracks }.
// 트랙 블록은 "track " 줄에서 시작해 다음 "track " 줄 전까지다.
function parseSong(lines) {
    const globals = [];
    const tracks = [];
    let cur = null;
    for (const L of lines) {
        if (L.startsWith('track ')) {
            const m = /^(\d+)\s+(\d+)\s?(.*)$/.exec(L.slice(6));
            if (!m) throw new Error(`track 줄을 해석할 수 없습니다: ${L}`);
            cur = { channel: +m[1], muted: +m[2], name: m[3] ?? '', lines: [], evs: [] };
            tracks.push(cur);
        } else if (cur && L.startsWith('ev ')) {
            const p = L.slice(3).trim().split(/\s+/);
            cur.evs.push({ tick: +p[0], status: +p[1], d1: +p[2], d2: +p[3] });
        } else if (cur) {
            cur.lines.push(L);
        } else {
            globals.push(L);
        }
    }
    return { globals, tracks };
}

// 같은 틱에서는 Note Off를 Note On보다 먼저 — 같은 음을 연달아 찍을 때
// 앞 음의 오프가 뒤 음을 꺼버리지 않게 한다.
const evRank = (e) => ((e.status & 0xf0) === 0x80 ? 0 : 1);

function songToLines({ globals, tracks }) {
    const out = [...globals];
    for (const t of tracks) {
        out.push(`track ${t.channel} ${t.muted} ${t.name}`);
        out.push(...t.lines);
        const evs = [...t.evs].sort((a, b) => a.tick - b.tick || evRank(a) - evRank(b));
        for (const e of evs) out.push(`ev ${e.tick} ${e.status} ${e.d1} ${e.d2}`);
    }
    return out;
}

function serializeProject(proj) {
    const out = [...proj.header];
    for (const sec of proj.sections) {
        out.push(`[${sec.name}]`);
        out.push(...(sec.name === 'song' ? songToLines(proj.song) : sec.lines));
    }
    if (!proj.sections.some((s) => s.name === 'end')) out.push('[end]');
    return out.join('\n') + '\n';
}

const getGlobal = (globals, key, def) => {
    const hit = globals.find((L) => L.startsWith(key + ' '));
    return hit === undefined ? def : hit.slice(key.length + 1).trim();
};
function setGlobal(globals, key, value) {
    const idx = globals.findIndex((L) => L.startsWith(key + ' '));
    if (idx >= 0) globals[idx] = `${key} ${value}`;
    else globals.unshift(`${key} ${value}`);
}
const projectPpqn = (proj) => {
    const v = parseInt(getGlobal(proj.song.globals, 'ppqn', '480'), 10);
    return Number.isFinite(v) && v > 0 ? v : 480;
};

// 트랙 지정: 인덱스(0부터) 또는 이름(대소문자 무시).
function resolveTrack(song, ref) {
    if (ref === undefined || ref === null) throw new Error('track: 트랙 번호(0부터) 또는 이름이 필요합니다');
    if (typeof ref === 'number' || /^\d+$/.test(String(ref))) {
        const i = Math.round(Number(ref));
        if (i < 0 || i >= song.tracks.length)
            throw new Error(`track ${i}: 트랙이 없습니다 (현재 ${song.tracks.length}개)`);
        return i;
    }
    const want = String(ref).trim().toLowerCase();
    const i = song.tracks.findIndex((t) => t.name.trim().toLowerCase() === want);
    if (i < 0)
        throw new Error(`track "${ref}": 그런 이름의 트랙이 없습니다 (있는 트랙: ${song.tracks.map((t) => t.name).join(', ') || '없음'})`);
    return i;
}

// ------------------------------------------------------------------
// 파일 읽기/쓰기 (원자적 교체 + 자동 백업)
// ------------------------------------------------------------------
function loadProjectFile(file) {
    if (!existsSync(file)) throw new Error(`파일이 없습니다: ${file}`);
    return parseProject(readFileSync(file, 'utf8'));
}

// 왜 백업을 기본으로 하나: 이 서버는 사용자의 작업 파일을 직접 덮어쓴다.
// 되돌릴 방법이 없으면 사고 한 번이 곡 하나를 날린다.
function saveProjectFile(file, proj, { backup = true } = {}) {
    const text = serializeProject(proj);
    if (backup && existsSync(file)) copyFileSync(file, file + '.bak');
    const tmp = file + '.tmp';
    writeFileSync(tmp, text, 'utf8');
    renameSync(tmp, file); // 윈도우에서도 기존 파일을 덮어쓴다
    return text.length;
}

// ------------------------------------------------------------------
// 화음 / 드럼 사전
// ------------------------------------------------------------------
const CHORD_QUALITIES = {
    '': [0, 4, 7], 'maj': [0, 4, 7], 'M': [0, 4, 7], 'major': [0, 4, 7],
    'm': [0, 3, 7], 'min': [0, 3, 7], '-': [0, 3, 7], 'minor': [0, 3, 7],
    '5': [0, 7], 'power': [0, 7],
    'dim': [0, 3, 6], 'o': [0, 3, 6],
    'aug': [0, 4, 8], '+': [0, 4, 8],
    'sus2': [0, 2, 7], 'sus4': [0, 5, 7], 'sus': [0, 5, 7],
    '6': [0, 4, 7, 9], 'm6': [0, 3, 7, 9], 'min6': [0, 3, 7, 9],
    '7': [0, 4, 7, 10], 'dom7': [0, 4, 7, 10], '7sus4': [0, 5, 7, 10],
    'maj7': [0, 4, 7, 11], 'M7': [0, 4, 7, 11], 'Maj7': [0, 4, 7, 11],
    'm7': [0, 3, 7, 10], 'min7': [0, 3, 7, 10],
    'mmaj7': [0, 3, 7, 11], 'mM7': [0, 3, 7, 11],
    'dim7': [0, 3, 6, 9], 'm7b5': [0, 3, 6, 10], 'halfdim': [0, 3, 6, 10],
    '9': [0, 4, 7, 10, 14], 'maj9': [0, 4, 7, 11, 14], 'm9': [0, 3, 7, 10, 14],
    'add9': [0, 4, 7, 14], 'madd9': [0, 3, 7, 14],
    '11': [0, 4, 7, 10, 14, 17], 'm11': [0, 3, 7, 10, 14, 17],
    '13': [0, 4, 7, 10, 14, 21], 'maj13': [0, 4, 7, 11, 14, 21],
};

// "Cmaj7", "F#m7", "G7/B" -> { root, intervals, bass }
function parseChord(symbol) {
    const s = String(symbol).trim();
    const m = /^([A-Ga-g][#b♯♭]?)(.*?)(?:\/([A-Ga-g][#b♯♭]?))?$/.exec(s);
    if (!m) throw new Error(`코드를 해석할 수 없습니다: ${symbol}`);
    const pc = (name) => {
        let v = NOTE_LETTER[name[0].toLowerCase()];
        for (const ch of name.slice(1)) v += (ch === '#' || ch === '♯') ? 1 : -1;
        return ((v % 12) + 12) % 12;
    };
    const quality = m[2] ?? '';
    const iv = CHORD_QUALITIES[quality] ?? CHORD_QUALITIES[quality.toLowerCase()];
    if (!iv)
        throw new Error(`모르는 코드 종류: "${quality}" (${symbol}) — 쓸 수 있는 예: maj, m, 7, maj7, m7, dim, aug, sus4, add9, m7b5`);
    return { rootPc: pc(m[1]), intervals: iv, bassPc: m[3] ? pc(m[3]) : null };
}

// GM 드럼 맵(채널 10). 이름은 관용적으로 여러 개 받는다.
const GM_DRUMS = {
    kick: 36, bd: 36, bassdrum: 36, kick2: 35,
    snare: 38, sd: 38, rim: 37, rimshot: 37, clap: 39, snare2: 40,
    hat: 42, hh: 42, closedhat: 42, hihat: 42,
    pedalhat: 44, openhat: 46, oh: 46,
    crash: 49, crash2: 57, ride: 51, ridebell: 53, splash: 55, china: 52,
    tom1: 50, hightom: 50, tom2: 47, midtom: 47, tom3: 45, lowtom: 45,
    floortom: 41, tom4: 41,
    cowbell: 56, tambourine: 54, tamb: 54, shaker: 82, clave: 75,
};

// ------------------------------------------------------------------
// SMF(.mid) 읽기/쓰기
//
// 앱의 sequencer/SmfFile.cpp와 같은 범위를 맞춘다: 포맷 0/1, PPQN 분해능
// (SMPTE는 거부), 러닝 스테이터스, 템포·트랙 이름 메타, 채널 보이스 메시지.
// 쓰기는 포맷 1(트랙 0 = 템포 트랙)이고 러닝 스테이터스를 쓰지 않는다 —
// 몇 바이트 아끼자고 호환성 위험을 지지 않는다.
// ------------------------------------------------------------------
function readVlq(buf, pos) {
    let value = 0, b, n = 0;
    do {
        if (pos >= buf.length) throw new Error('MIDI 파일이 중간에 끊겼습니다 (VLQ)');
        b = buf[pos++];
        value = (value << 7) | (b & 0x7f);
    } while ((b & 0x80) && ++n < 4);
    return [value, pos];
}

function writeVlq(out, value) {
    let v = value >>> 0;
    let buffer = v & 0x7f;
    while ((v >>>= 7) > 0) {
        buffer = ((buffer << 8) | 0x80 | (v & 0x7f)) >>> 0;
    }
    for (;;) {
        out.push(buffer & 0xff);
        if (buffer & 0x80) buffer >>>= 8;
        else break;
    }
}

function parseSmfTrack(buf, pos, end) {
    let tick = 0, running = 0, name = '';
    const events = [], tempos = [];
    while (pos < end) {
        let dt;
        [dt, pos] = readVlq(buf, pos);
        tick += dt;
        if (pos >= end) break;
        let status = buf[pos];
        if (status & 0x80) { pos++; running = status; }
        else { status = running; if (!status) break; } // 러닝 스테이터스
        if (status === 0xff) {
            const type = buf[pos++];
            let len;
            [len, pos] = readVlq(buf, pos);
            const data = buf.subarray(pos, pos + len);
            pos += len;
            if (type === 0x03 && !name) name = data.toString('utf8').replace(/\0/g, '').trim();
            else if (type === 0x51 && len === 3) tempos.push({ tick, us: (data[0] << 16) | (data[1] << 8) | data[2] });
        } else if (status === 0xf0 || status === 0xf7) {
            let len;
            [len, pos] = readVlq(buf, pos);
            pos += len; // SysEx는 버린다 (앱도 곡 데이터로 쓰지 않는다)
        } else {
            const hi = status & 0xf0;
            const d1 = buf[pos++];
            const d2 = (hi === 0xc0 || hi === 0xd0) ? 0 : buf[pos++];
            events.push({ tick, status, d1, d2 });
        }
    }
    return { name, events, tempos };
}

function parseSmf(buf) {
    if (buf.length < 14 || buf.toString('ascii', 0, 4) !== 'MThd')
        throw new Error('MIDI 파일이 아닙니다 (MThd 헤더 없음)');
    const headLen = buf.readUInt32BE(4);
    const format = buf.readUInt16BE(8);
    const division = buf.readInt16BE(12);
    if (division <= 0)
        throw new Error('SMPTE 타임코드 MIDI는 지원하지 않습니다 (PPQN 분해능만 가능) — 앱도 같습니다');
    let pos = 8 + headLen;
    const tracks = [];
    while (pos + 8 <= buf.length) {
        const id = buf.toString('ascii', pos, pos + 4);
        const len = buf.readUInt32BE(pos + 4);
        pos += 8;
        const end = Math.min(pos + len, buf.length);
        if (id === 'MTrk') tracks.push(parseSmfTrack(buf, pos, end));
        pos = end;
    }
    if (!tracks.length) throw new Error('MIDI 파일에 트랙(MTrk)이 없습니다');
    return { format, division, tracks };
}

// events: {tick, status, d1, d2} 또는 {tick, meta, data}
function buildSmfTrack(events, name) {
    const body = [];
    if (name) {
        const nb = Buffer.from(name, 'utf8');
        writeVlq(body, 0);
        body.push(0xff, 0x03);
        writeVlq(body, nb.length);
        for (const b of nb) body.push(b);
    }
    let prev = 0;
    for (const e of events) {
        writeVlq(body, Math.max(0, e.tick - prev));
        prev = e.tick;
        if (e.meta !== undefined) {
            body.push(0xff, e.meta);
            writeVlq(body, e.data.length);
            for (const b of e.data) body.push(b);
        } else {
            const hi = e.status & 0xf0;
            body.push(e.status & 0xff, e.d1 & 0x7f);
            if (hi !== 0xc0 && hi !== 0xd0) body.push(e.d2 & 0x7f);
        }
    }
    writeVlq(body, 0);
    body.push(0xff, 0x2f, 0x00); // End of Track
    const head = Buffer.alloc(8);
    head.write('MTrk', 0, 'ascii');
    head.writeUInt32BE(body.length, 4);
    return Buffer.concat([head, Buffer.from(body)]);
}

function projectToSmf(proj, which) {
    const ppqn = projectPpqn(proj);
    const bpm = Number(getGlobal(proj.song.globals, 'bpm', '120')) || 120;
    const us = Math.round(60000000 / bpm);
    const chunks = [buildSmfTrack([
        { tick: 0, meta: 0x51, data: [(us >> 16) & 0xff, (us >> 8) & 0xff, us & 0xff] },
        { tick: 0, meta: 0x58, data: [4, 2, 24, 8] },
    ], 'Tempo')];
    let notes = 0;
    proj.song.tracks.forEach((t, i) => {
        if (which && !which.includes(i)) return;
        const evs = [...t.evs].sort((a, b) => a.tick - b.tick || evRank(a) - evRank(b));
        notes += evs.filter((e) => (e.status & 0xf0) === 0x90 && e.d2 > 0).length;
        chunks.push(buildSmfTrack(evs, t.name));
    });
    const head = Buffer.alloc(14);
    head.write('MThd', 0, 'ascii');
    head.writeUInt32BE(6, 4);
    head.writeUInt16BE(1, 8);              // 포맷 1
    head.writeUInt16BE(chunks.length, 10);
    head.writeUInt16BE(ppqn, 12);
    return { buffer: Buffer.concat([head, ...chunks]), notes, trackChunks: chunks.length };
}

// ------------------------------------------------------------------
// 플러그인 (tplugin 줄)
//
// 앱은 프로젝트를 열 때 tplugin 줄을 보고 VST3를 실제로 로드한다
// (gui/App.cpp). 로드에 실패하면 그 줄만 조용히 버리므로, 잘못 써도
// 프로젝트가 깨지지는 않는다.
//
// 형식: tplugin <악기?1:0> <켜짐?1:0> <classIndex> <이름>\t<경로>
// classIndex = -1 은 "번들에서 클래스 자동 선택" — 앱 UI가 쓰는 값과 같다.
// 내장 이펙트는 경로가 "builtin:<토큰>" 이고 파라미터는 기본값을 쓴다.
// ------------------------------------------------------------------
const BUILTIN_FX = {
    eq: 'EQ', delay: '딜레이', reverb: '리버브', limiter: '리미터', comp: '컴프레서',
};
const BUILTIN_ALIAS = {
    eq: 'eq', 이퀄라이저: 'eq',
    delay: 'delay', 딜레이: 'delay', echo: 'delay',
    reverb: 'reverb', 리버브: 'reverb',
    limiter: 'limiter', 리미터: 'limiter',
    comp: 'comp', compressor: 'comp', 컴프레서: 'comp', 컴프: 'comp',
};

function scannedPlugins() {
    const f = path.join(dataDir(), 'plugincache.ini');
    if (!existsSync(f)) return [];
    return readFileSync(f, 'utf8').split(/\r?\n/).slice(1).map((L) => {
        const m = /^(\d+)\s+(\d+)\s+(\d)\s+(\d)\s+(.+)$/.exec(L);
        if (!m) return null;
        return {
            path: m[5], name: path.basename(m[5], '.vst3'),
            instrument: m[3] === '1', effect: m[4] === '1',
        };
    }).filter(Boolean);
}

// "Surge XT" 같은 이름이나 .vst3 전체 경로를 받아 {name, path}로 바꾼다.
function resolveVst(spec, wantInstrument) {
    const s = String(spec).trim();
    const kind = wantInstrument ? '악기' : '이펙트';
    if (/[\\/]/.test(s) || /\.vst3$/i.test(s)) {
        if (!existsSync(s)) throw new Error(`VST3를 찾을 수 없습니다: ${s}`);
        return { name: path.basename(s, '.vst3'), path: path.resolve(s) };
    }
    const all = scannedPlugins();
    if (!all.length)
        throw new Error('스캔된 VST3가 없습니다. 앱의 VST3 창에서 "플러그인 검색"을 먼저 하세요.');
    const usable = all.filter((p) => (wantInstrument ? p.instrument : p.effect));
    const low = s.toLowerCase();
    const hit = usable.find((p) => p.name.toLowerCase() === low)
             ?? usable.find((p) => p.name.toLowerCase().includes(low));
    if (!hit) {
        const other = all.find((p) => p.name.toLowerCase().includes(low));
        if (other)
            throw new Error(`"${other.name}"은(는) ${kind}로 쓸 수 없습니다 ` +
                            `(악기=${other.instrument ? 'O' : 'X'}, 이펙트=${other.effect ? 'O' : 'X'})`);
        throw new Error(`${kind}로 쓸 수 있는 "${s}"을(를) 못 찾았습니다. ` +
                        `쓸 수 있는 것: ${usable.map((p) => p.name).join(', ') || '없음'}`);
    }
    return { name: hit.name, path: hit.path };
}

// tplugin 줄은 taudio 블록(taudio+tgain/tfade가 붙어 다닌다) 앞에 넣는다.
function insertPluginLine(track, line) {
    const at = track.lines.findIndex((L) => L.startsWith('taudio '));
    if (at < 0) track.lines.push(line);
    else track.lines.splice(at, 0, line);
}

const pluginLine = (isInst, name, p) => `tplugin ${isInst ? 1 : 0} 1 -1 ${name}\t${p}`;

// ------------------------------------------------------------------
// 드럼 샘플 라이브러리 (drumsample 줄)
//
// 프로젝트 헤더의 "drumsample <노트> <경로>" 는 그 노트를 내장 신디 대신
// WAV로 소리내게 한다. 앱은 파일 경로의 낱말로 킥/스네어/햇을 자동 분류하는데
// (gui/PanelsDrums.cpp classifyDrumPath), 여기서 같은 규칙을 그대로 옮겨
// 어떤 샘플이 어느 드럼인지 알아낸 뒤 자동 배정한다.
//
// 라이브러리 위치도 앱과 같은 순서로 찾는다: <저장소>/src/Drum ->
// %LOCALAPPDATA%\MidiPro\Drum.
// ------------------------------------------------------------------
function drumLibRoot() {
    const cands = [
        process.env.MIDIPRO_DRUMLIB,
        path.join(HERE, '..', 'src', 'Drum'),
        path.join(dataDir(), 'Drum'),
    ].filter(Boolean);
    for (const c of cands) if (existsSync(c)) return path.resolve(c);
    return null;
}

// 앱의 classifyDrumPath(gui/PanelsDrums.cpp)를 옮기되 두 가지를 고쳤다:
//
//  1) 낱말 단위로 본다. 단순 부분 문자열이면 "Bottoms Up"이 "tom"으로,
//     "Bell"이 "bd"로 잡힌다. 사람이 목록에서 고를 때는 눈으로 걸러지지만
//     자동 배정은 그대로 집어가므로 오탐이 치명적이다.
//  2) 0/1이 아니라 점수를 준다. "snare"라고 적힌 파일이 "rim"보다,
//     "crash"가 막연한 "cymbal"보다 먼저 뽑히게 하려는 것.
const wordIn = (s, w) => new RegExp(`(^|[^a-z])${w}s?([^a-z]|$)`).test(s);
// 약어("rd", "bd", "hh")는 숫자까지 경계로 본다 — 그래야 "3rd"가 ride로 잡히지 않는다
const abbrIn = (s, w) => new RegExp(`(^|[^a-z0-9])${w}s?([^a-z0-9]|$)`).test(s);

function scoreDrum(rel) {
    const s = rel.toLowerCase();
    const has = (k) => s.includes(k);
    const w = (k) => wordIn(s, k);
    const ab = (k) => abbrIn(s, k);
    const m = new Map();
    const put = (b, sc) => m.set(b, Math.max(m.get(b) ?? 0, sc));

    if (w('clap')) put('clap', 2);
    if (w('snare') || ab('snr') || ab('sn')) put('snare', 2);
    if (w('rim')) { put('rim', 2); put('snare', 1); } // 스네어가 없을 때만 대타
    if (w('kick') || ab('kik') || has('bassdrum') || has('bass drum') ||
        has('bass-drum') || ab('bd')) put('kick', 2);
    if (w('hat') || has('hihat') || has('hi-hat') || ab('hh') || ab('chh') || ab('ohh')) {
        if (has('open') || ab('ohh')) put('hatopen', 2);
        else if (has('close') || ab('cls') || ab('chh')) put('hatclosed', 2);
        else { put('hatclosed', 1); put('hatopen', 1); } // 모호하면 둘 다 약하게
    }
    if (w('tom')) put('tom', 2);
    if (w('crash') || ab('crsh') || ab('crs')) put('crash', 2);
    if (w('ride') || ab('rd')) put('ride', 2);
    if (w('cymbal') || ab('cym')) { put('crash', 1); put('ride', 1); }
    if (m.size === 0) m.set('etc', 1);
    return m;
}

// GM 드럼 노트 -> 어느 분류에서 고를지 (앞이 없으면 뒤로 대체)
const NOTE_BUCKET = {
    35: 'kick', 36: 'kick',
    37: 'rim', 38: 'snare', 40: 'snare',
    39: 'clap',
    42: 'hatclosed', 44: 'hatclosed',
    46: 'hatopen',
    41: 'tom', 43: 'tom', 45: 'tom', 47: 'tom', 48: 'tom', 50: 'tom',
    49: 'crash', 52: 'crash', 55: 'crash', 57: 'crash',
    51: 'ride', 53: 'ride', 59: 'ride',
};
const BUCKET_FALLBACK = {
    rim: ['rim', 'snare'], snare: ['snare', 'rim'],
    hatopen: ['hatopen', 'hatclosed'], hatclosed: ['hatclosed', 'hatopen'],
    crash: ['crash', 'ride'], ride: ['ride', 'crash'],
    clap: ['clap', 'snare'],
};
const BUCKET_LABEL = {
    kick: '킥', snare: '스네어', clap: '클랩', hatclosed: '클로즈드 햇',
    hatopen: '오픈 햇', tom: '탐', crash: '크래시', ride: '라이드', etc: '기타',
};
const DRUM_NOTE_NAME = {
    35: '킥2', 36: '킥', 37: '림', 38: '스네어', 39: '클랩', 40: '스네어2',
    41: '플로어 탐', 42: '클로즈드 햇', 43: '로우 탐', 44: '페달 햇', 45: '로우 탐',
    46: '오픈 햇', 47: '미드 탐', 48: '하이 탐', 49: '크래시', 50: '하이 탐',
    51: '라이드', 52: '차이나', 53: '라이드 벨', 54: '탬버린', 55: '스플래시',
    56: '카우벨', 57: '크래시2', 59: '라이드2', 75: '클라베', 82: '셰이커',
};

// 라이브러리 스캔은 파일이 수천 개라 프로세스당 한 번만 한다.
let g_drumLib = null;
function scanDrumLib() {
    if (g_drumLib) return g_drumLib;
    const root = drumLibRoot();
    g_drumLib = { root, files: [], kits: new Map() };
    if (!root) return g_drumLib;
    const walk = (dir) => {
        let ents;
        try { ents = readdirSync(dir, { withFileTypes: true }); } catch { return; }
        for (const e of ents) {
            const full = path.join(dir, e.name);
            if (e.isDirectory()) walk(full);
            else if (/\.wav$/i.test(e.name)) {
                const rel = path.relative(root, full);
                const segs = rel.split(/[\\/]/);
                // 구조: <아카이브>/<드럼머신>/<악기폴더>/<파일>. 앱 표시도 첫 칸을 뺀다.
                const kit = segs.length >= 3 ? segs[1] : segs[0];
                // 같은 킷 안의 "계열" — "Acoustic-Kick-...", "Urban-Tom-..." 처럼
                // 파일 이름이 <계열>-<악기>-<이름> 꼴이면 계열을 뽑아 둔다.
                // 계열을 맞춰 고르면 킥만 어쿠스틱, 탐만 힙합인 잡탕을 피한다.
                const base = e.name.replace(/\.wav$/i, '');
                const parts = base.split('-');
                const family = parts.length >= 3 ? parts[0].trim() : null;
                const f = { full, rel, kit, family, scores: scoreDrum(rel) };
                g_drumLib.files.push(f);
                if (!g_drumLib.kits.has(kit)) g_drumLib.kits.set(kit, []);
                g_drumLib.kits.get(kit).push(f);
            }
        }
    };
    walk(root);
    g_drumLib.files.sort((a, b) => a.rel.localeCompare(b.rel));
    for (const list of g_drumLib.kits.values()) list.sort((a, b) => a.rel.localeCompare(b.rel));
    return g_drumLib;
}

const hasBucket = (f, b) => (f.scores.get(b) ?? 0) > 0;
const kitCoverage = (files, buckets) =>
    buckets.filter((b) => files.some((f) => hasBucket(f, b))).length;

// 한 분류의 후보들: 점수 높은 것 먼저, 같으면 이름순. 없으면 대체 분류로.
function candidatesFor(files, bucket) {
    for (const b of BUCKET_FALLBACK[bucket] ?? [bucket]) {
        const hit = files.filter((f) => hasBucket(f, b));
        if (!hit.length) continue;
        const top = Math.max(...hit.map((f) => f.scores.get(b)));
        const best = hit.filter((f) => f.scores.get(b) === top);
        best.sort((a, c) => a.rel.localeCompare(c.rel));
        return best;
    }
    return [];
}

// 한 킷 안에서 노트들에 샘플을 배정한다.
// 계열(Acoustic/Urban 등)이 있으면 필요한 악기를 가장 많이 갖춘 계열로 통일하고,
// 그 계열에 없는 악기만 킷 전체에서 가져온다.
// 같은 분류에 여러 노트가 걸리면(탐 3개 등) 후보 목록을 고르게 훑어 서로 다른
// 샘플이 걸리게 한다 — 보통 이름 순서가 낮은 음~높은 음이라 자연스럽다.
function assignKit(files, notes, wantFamily) {
    const grouped = new Map();
    for (const n of notes) {
        const b = NOTE_BUCKET[n] ?? 'etc';
        if (!grouped.has(b)) grouped.set(b, []);
        grouped.get(b).push(n);
    }
    const wanted = [...grouped.keys()];

    const fams = new Map();
    for (const f of files) {
        if (!f.family) continue;
        if (!fams.has(f.family)) fams.set(f.family, []);
        fams.get(f.family).push(f);
    }
    let famName = null, famFiles = null;
    if (wantFamily) {
        const low = String(wantFamily).toLowerCase();
        const hit = [...fams.keys()].find((k) => k.toLowerCase() === low)
                 ?? [...fams.keys()].find((k) => k.toLowerCase().includes(low));
        if (!hit)
            throw new Error(`"${wantFamily}" 계열이 이 킷에 없습니다. ` +
                            `있는 계열: ${[...fams.keys()].sort().join(', ') || '(계열 구분 없음)'}`);
        famName = hit;
        famFiles = fams.get(hit);
    } else {
        for (const [name, fs2] of fams) {
            const cov = kitCoverage(fs2, wanted);
            if (!famFiles || cov > kitCoverage(famFiles, wanted) ||
                (cov === kitCoverage(famFiles, wanted) && fs2.length > famFiles.length)) {
                famName = name; famFiles = fs2;
            }
        }
    }

    const out = [], missing = [];
    const used = new Set(); // 같은 파일이 두 드럼에 걸리지 않게
    for (const [b, ns] of grouped) {
        let cands = famFiles ? candidatesFor(famFiles, b) : [];
        let fromFamily = cands.length > 0;
        if (!cands.length) cands = candidatesFor(files, b);
        if (!cands.length) { missing.push(...ns); continue; }
        ns.sort((x, y) => x - y);
        ns.forEach((n, i) => {
            const idx = ns.length === 1 ? 0
                      : Math.round((i * (cands.length - 1)) / (ns.length - 1));
            let pick = cands[Math.min(idx, cands.length - 1)];
            // 이미 쓴 샘플이면 가까운 미사용 후보로 비킨다 (크래시/라이드가
            // 같은 "cymbal" 후보군을 나눠 쓸 때 똑같은 파일이 걸리는 걸 막는다)
            if (used.has(pick.full)) {
                const free = cands.filter((c) => !used.has(c.full));
                if (free.length) pick = free[Math.min(idx, free.length - 1)];
            }
            used.add(pick.full);
            out.push({ note: n, bucket: b, fromFamily, file: pick });
        });
    }
    out.sort((a, b2) => a.note - b2.note);
    return { assigned: out, missing, family: famName };
}

// 프로젝트의 드럼 트랙(채널 9)이 실제로 쓰는 노트들
function usedDrumNotes(song) {
    const s = new Set();
    for (const t of song.tracks) {
        if ((t.channel & 0x0f) !== 9) continue;
        for (const e of t.evs)
            if ((e.status & 0xf0) === 0x90 && e.d2 > 0) s.add(e.d1);
    }
    return [...s].sort((a, b) => a - b);
}

// ------------------------------------------------------------------
// 실행 중인 앱 제어 (네임드 파이프)
//
// 앱이 \\.\pipe\MidiPro.Control 을 열어 둔다(gui/ControlServer.cpp).
// 요청은 "명령 인자..." 한 줄, 응답은 JSON 한 줄.
//
// 파이프는 윈도우에서 그냥 파일처럼 열린다 — 별도 라이브러리가 필요 없다.
// 다만 노드의 파일 스트림으로는 "쓰고 나서 읽기"가 매끄럽지 않아, 열고 쓰고
// 읽는 동안만 동기 I/O로 처리한다(명령이 드물고 응답이 짧아 문제되지 않는다).
// ------------------------------------------------------------------
const CONTROL_PIPE = '\\\\.\\pipe\\MidiPro.Control';

function controlSend(command, { timeoutMs = 7000 } = {}) {
    let fd;
    try {
        fd = openSync(CONTROL_PIPE, 'r+');
    } catch (e) {
        if (e.code === 'ENOENT')
            throw new Error('실행 중인 MidiPro를 찾지 못했습니다 — 앱이 꺼져 있거나 ' +
                            '개인설정에서 외부 제어를 꺼 두었을 수 있습니다 (settings.ini의 control_pipe). ' +
                            'midipro_open으로 앱을 먼저 켜세요.');
        throw new Error(`제어 통로를 열지 못했습니다: ${e.message}`);
    }
    try {
        const out = Buffer.from(command + '\n', 'utf8');
        writeSync(fd, out, 0, out.length);
        const deadline = Date.now() + timeoutMs;
        const chunk = Buffer.alloc(64 * 1024);
        let acc = '';
        for (;;) {
            let n = 0;
            try {
                n = readSync(fd, chunk, 0, chunk.length, null);
            } catch (e) {
                if (e.code === 'EAGAIN') { n = 0; } else throw e;
            }
            if (n > 0) {
                acc += chunk.subarray(0, n).toString('utf8');
                const nl = acc.indexOf('\n');
                if (nl >= 0) {
                    const line = acc.slice(0, nl).trim();
                    try {
                        return JSON.parse(line);
                    } catch {
                        throw new Error(`앱의 응답을 해석하지 못했습니다: ${line.slice(0, 200)}`);
                    }
                }
            }
            if (Date.now() > deadline)
                throw new Error('앱이 제한 시간 안에 응답하지 않았습니다 ' +
                                '(대화상자가 열려 있거나 바쁜 상태일 수 있습니다)');
        }
    } finally {
        try { closeSync(fd); } catch {}
    }
}

// 앱이 살아 있는지 (제어 통로가 열려 있는지)
function controlAlive() {
    try {
        return controlSend('ping', { timeoutMs: 1500 })?.ok === true;
    } catch { return false; }
}

// ------------------------------------------------------------------
// MidiPro 실행 파일 찾기
// ------------------------------------------------------------------
function findMidiProExe() {
    const cands = [
        process.env.MIDIPRO_EXE,
        path.join(HERE, '..', 'build', 'MidiPro.exe'),      // 개발 트리
        'C:\\Program Files\\MidiPro\\MidiPro.exe',           // 설치본(모든 사용자)
        'C:\\Program Files (x86)\\MidiPro\\MidiPro.exe',
    ].filter(Boolean);
    for (const c of cands) if (existsSync(c)) return path.resolve(c);
    return null;
}

function isMidiProRunning() {
    try {
        const out = execFileSync('tasklist', ['/FI', 'IMAGENAME eq MidiPro.exe', '/NH'],
                                 { encoding: 'utf8', timeout: 5000, windowsHide: true });
        return /MidiPro\.exe/i.test(out);
    } catch { return false; }
}

const localAppData = () => process.env.LOCALAPPDATA || path.join(process.env.USERPROFILE || '', 'AppData', 'Local');
// 앱의 사용자 데이터 폴더 (autosave/settings/recent/plugincache/session.lock)
const dataDir = () => process.env.MIDIPRO_DATA_DIR
    ? path.resolve(process.env.MIDIPRO_DATA_DIR)
    : path.join(localAppData(), 'MidiPro');

// ------------------------------------------------------------------
// "앱이 이 프로젝트를 열고 있는가" 안전장치
//
// 왜 필요한가: 이 서버는 파일을 직접 고치는데, 앱은 열어 둔 프로젝트를 주기적으로
// 자동 저장한다. 앱이 그 프로젝트를 열고 있는 동안 고치면 편집이 통째로 사라진다.
// (개발 중에 실제로 여러 번 겪었다.)
//
// 판별: session.lock 은 앱이 시작할 때 만들고 정상 종료 때 지운다 = 세션 진행 중.
// recent.txt 맨 윗줄 = 앱이 가장 최근에 연 프로젝트. 둘이 겹치면 열려 있다고 본다.
// 확실한 판정은 아니다(앱에서 새 곡을 만들었을 수도 있다) — 그래서 force로 넘길 수 있다.
// ------------------------------------------------------------------
function openProjectState(file) {
    const dir = dataDir();
    const live = existsSync(path.join(dir, 'session.lock'));
    let top = null;
    const rf = path.join(dir, 'recent.txt');
    if (existsSync(rf)) {
        const lines = readFileSync(rf, 'utf8').split(/\r?\n/).filter(Boolean);
        top = lines[0] ?? null;
    }
    const same = !!top && path.resolve(top).toLowerCase() === path.resolve(file).toLowerCase();
    return { live, top, same };
}

// 막아야 하면 예외를 던지고, 그냥 조심시킬 상황이면 경고 문자열을 돌려준다.
function guardProjectOpen(file, force) {
    if (process.env.MIDIPRO_OPEN_GUARD === 'off') return null;
    const s = openProjectState(file);
    if (!s.live) return null; // 세션 없음 = 안전
    if (s.same && !force)
        throw new Error(
            `MidiPro가 이 프로젝트를 열고 있는 것 같습니다 — 지금 고치면 앱이 자동 저장할 때 편집이 사라집니다.\n` +
            `  1) force: true 로 고친 뒤 곧바로 midipro_transport(action="reload")를 부르세요 (앱을 닫지 않아도 됩니다)\n` +
            `  2) 또는 앱에서 저장하고 닫은 뒤 다시 하세요\n` +
            `  3) 앱이 이미 꺼져 있는데 이 메시지가 나오면 비정상 종료로 남은 세션 표시입니다 — ` +
            `앱을 한 번 켰다 정상 종료하거나 force: true 로 넘기세요\n` +
            `파일: ${file}`);
    if (s.same) return '[주의] 앱이 이 프로젝트를 열고 있는데 force로 강행했습니다 — 앱에서 저장하면 이 편집이 덮어써집니다.';
    return '[경고] MidiPro가 실행 중입니다. 이 프로젝트를 앱에서도 열고 있다면 앱의 자동 저장이 이 편집을 덮어씁니다.';
}

// 프로젝트 파일을 고치는 도구들 — 위 안전장치를 거친다
const MUTATING_TOOLS = new Set([
    'midipro_create_project', 'midipro_add_track', 'midipro_add_notes', 'midipro_add_chords',
    'midipro_add_drums', 'midipro_set_tempo', 'midipro_import_midi', 'midipro_set_drumkit',
    'midipro_set_instrument', 'midipro_add_effect',
]);

// ------------------------------------------------------------------
// 도구 구현
// ------------------------------------------------------------------
const MIN_PROJECT_HEADER = ['midipro_project 1'];

function makeTrackBlock({ name, type = 'midi', channel }) {
    const t = { channel: 0, muted: 0, name: name ?? 'Track', lines: [], evs: [] };
    if (type === 'drum') t.channel = 9;                      // GM 드럼 채널 10
    else if (channel !== undefined) t.channel = clamp(Math.round(Number(channel)), 0, 15);
    t.lines.push('tvol 1 0 1 0', 'tinch 0');
    if (type === 'guitar') t.lines.push('tgtr 1');
    return t;
}

const TOOLS = {
    // --- 프로젝트 만들기 -------------------------------------------------
    midipro_create_project: {
        description:
            '새 .midipro 프로젝트 파일을 만든다. 트랙 목록을 함께 주면 빈 트랙까지 만들어 둔다. ' +
            '이미 있는 파일은 overwrite=true 없이는 덮어쓰지 않는다.',
        inputSchema: {
            type: 'object',
            properties: {
                path: { type: 'string', description: '만들 .midipro 파일의 전체 경로' },
                bpm: { type: 'number', description: '템포 (기본 120)' },
                ppqn: { type: 'integer', description: '4분음표당 틱 (기본 480)' },
                tracks: {
                    type: 'array',
                    description: '함께 만들 트랙들',
                    items: {
                        type: 'object',
                        properties: {
                            name: { type: 'string' },
                            type: { type: 'string', enum: ['midi', 'drum', 'guitar'], description: 'drum이면 채널 10 고정' },
                            channel: { type: 'integer', description: 'MIDI 채널 0~15 (drum은 무시)' },
                        },
                        required: ['name'],
                    },
                },
                overwrite: { type: 'boolean', description: '기존 파일 덮어쓰기 (기본 false)' },
            },
            required: ['path'],
        },
        run(args) {
            const file = path.resolve(String(args.path));
            if (existsSync(file) && !args.overwrite)
                throw new Error(`이미 있는 파일입니다: ${file} (덮어쓰려면 overwrite=true)`);
            const bpm = args.bpm === undefined ? 120 : requireNum(args.bpm, 'bpm', { min: 20, max: 400 });
            const ppqn = args.ppqn === undefined ? 480 : requireNum(args.ppqn, 'ppqn', { min: 24, max: 3840 });
            const proj = {
                header: [...MIN_PROJECT_HEADER],
                sections: [{ name: 'song', lines: [] }, { name: 'end', lines: [] }],
                song: { globals: [`bpm ${bpm}`, `ppqn ${ppqn}`, 'master 1 0 1'], tracks: [] },
            };
            for (const t of args.tracks ?? []) proj.song.tracks.push(makeTrackBlock(t));
            mkdirSync(path.dirname(file), { recursive: true });
            saveProjectFile(file, proj, { backup: false });
            const names = proj.song.tracks.map((t, i) => `  ${i}: ${t.name}${t.channel === 9 ? ' (드럼)' : ''}`);
            return `프로젝트를 만들었습니다: ${file}\n템포 ${bpm} BPM, ppqn ${ppqn}, 트랙 ${proj.song.tracks.length}개` +
                   (names.length ? '\n' + names.join('\n') : '');
        },
    },

    // --- 프로젝트 읽기 ---------------------------------------------------
    midipro_read_project: {
        description:
            '.midipro 파일을 읽어 구조를 JSON으로 돌려준다 (템포, 트랙, 트랙별 노트 수와 길이, 로드된 VST 플러그인, 마커).',
        inputSchema: {
            type: 'object',
            properties: {
                path: { type: 'string', description: '읽을 .midipro 파일 경로' },
                includeNotes: { type: 'boolean', description: '트랙별 노트를 모두 나열 (기본 false — 큰 곡은 응답이 매우 길어짐)' },
                track: { description: 'includeNotes일 때 이 트랙만 (번호 또는 이름)' },
            },
            required: ['path'],
        },
        run(args) {
            const file = path.resolve(String(args.path));
            const proj = loadProjectFile(file);
            const ppqn = projectPpqn(proj);
            const g = proj.song.globals;

            const only = args.track === undefined ? -1 : resolveTrack(proj.song, args.track);
            const tracks = proj.song.tracks.map((t, i) => {
                const ons = t.evs.filter((e) => (e.status & 0xf0) === 0x90 && e.d2 > 0);
                const ticks = t.evs.map((e) => e.tick);
                const info = {
                    index: i,
                    name: t.name,
                    channel: t.channel,
                    midiChannelDisplay: t.channel + 1,
                    muted: !!t.muted,
                    isDrum: t.channel === 9,
                    isGuitar: t.lines.some((L) => L.startsWith('tgtr ')),
                    frozen: t.lines.some((L) => L.startsWith('tfrz ')),
                    noteCount: ons.length,
                    startBeat: ticks.length ? Math.min(...ticks) / ppqn : null,
                    endBeat: ticks.length ? Math.max(...ticks) / ppqn : null,
                    plugins: t.lines.filter((L) => L.startsWith('tplugin ')).map((L) => {
                        const p = L.slice(8).split(/\s+/);
                        const rest = L.slice(8).split('\t');
                        return {
                            kind: p[0] === '1' ? 'instrument' : 'effect',
                            enabled: p[1] === '1',
                            name: (rest[0] ?? '').split(/\s+/).slice(3).join(' '),
                            path: rest[1] ?? '',
                        };
                    }),
                    audioClips: t.lines.filter((L) => L.startsWith('taudio ')).length,
                };
                if (args.includeNotes && (only < 0 || only === i)) {
                    info.notes = ons.map((e) => {
                        const off = t.evs.find((x) => x.tick > e.tick && x.d1 === e.d1 &&
                            ((x.status & 0xf0) === 0x80 || ((x.status & 0xf0) === 0x90 && x.d2 === 0)));
                        return {
                            pitch: numberToNote(e.d1), midi: e.d1, velocity: e.d2,
                            startBeat: e.tick / ppqn,
                            durationBeats: off ? (off.tick - e.tick) / ppqn : null,
                        };
                    });
                }
                return info;
            });

            return JSON.stringify({
                path: file,
                bpm: Number(getGlobal(g, 'bpm', '120')),
                ppqn,
                trackCount: tracks.length,
                totalNotes: tracks.reduce((s, t) => s + t.noteCount, 0),
                markers: g.filter((L) => L.startsWith('marker ')).map((L) => {
                    const p = L.slice(7).split(/\s(.+)/);
                    return { beat: Number(p[0]) / ppqn, name: p[1] ?? '' };
                }),
                tempoChanges: g.filter((L) => L.startsWith('tempo ')).map((L) => {
                    const p = L.slice(6).split(/\s+/);
                    return { beat: Number(p[0]) / ppqn, bpm: Number(p[1]) };
                }),
                hasVersionTree: proj.sections.some((s) => s.name === 'versions'),
                tracks,
            }, null, 2);
        },
    },

    // --- 트랙 추가 -------------------------------------------------------
    midipro_add_track: {
        description: '기존 프로젝트에 빈 트랙을 추가한다. type=drum이면 GM 드럼 채널(10)로 만든다.',
        inputSchema: {
            type: 'object',
            properties: {
                path: { type: 'string' },
                name: { type: 'string', description: '트랙 이름' },
                type: { type: 'string', enum: ['midi', 'drum', 'guitar'], description: '기본 midi' },
                channel: { type: 'integer', description: 'MIDI 채널 0~15 (기본: 트랙 순번, drum은 9 고정)' },
            },
            required: ['path', 'name'],
        },
        run(args) {
            const file = path.resolve(String(args.path));
            const proj = loadProjectFile(file);
            const type = args.type ?? 'midi';
            const ch = args.channel !== undefined ? args.channel
                     : (type === 'drum' ? 9 : proj.song.tracks.length % 16);
            const t = makeTrackBlock({ name: args.name, type, channel: ch });
            proj.song.tracks.push(t);
            saveProjectFile(file, proj);
            return `트랙 추가: [${proj.song.tracks.length - 1}] ${t.name} (MIDI 채널 ${t.channel + 1}${t.channel === 9 ? ', 드럼' : ''})\n` +
                   `총 트랙 ${proj.song.tracks.length}개 — ${file}`;
        },
    },

    // --- 노트 찍기 -------------------------------------------------------
    midipro_add_notes: {
        description:
            '트랙에 노트를 찍는다. 위치와 길이는 모두 박(beat=4분음표) 단위이고 0박이 곡의 시작이다. ' +
            '4/4에서 한 마디는 4박이므로 2마디 첫 박은 start=4. 음높이는 "C4" 같은 이름이나 0~127 숫자.',
        inputSchema: {
            type: 'object',
            properties: {
                path: { type: 'string' },
                track: { description: '트랙 번호(0부터) 또는 이름' },
                notes: {
                    type: 'array',
                    description: '찍을 노트들',
                    items: {
                        type: 'object',
                        properties: {
                            pitch: { description: '"C4", "F#3" 또는 0~127' },
                            start: { type: 'number', description: '시작 위치 (박)' },
                            duration: { type: 'number', description: '길이 (박, 기본 1)' },
                            velocity: { type: 'integer', description: '세기 1~127 (기본 100)' },
                        },
                        required: ['pitch', 'start'],
                    },
                },
                replace: { type: 'boolean', description: 'true면 트랙의 기존 노트를 모두 지우고 새로 찍는다 (기본 false)' },
            },
            required: ['path', 'track', 'notes'],
        },
        run(args) {
            if (!Array.isArray(args.notes) || args.notes.length === 0)
                throw new Error('notes: 노트를 하나 이상 넣어주세요');
            const file = path.resolve(String(args.path));
            const proj = loadProjectFile(file);
            const ppqn = projectPpqn(proj);
            const ti = resolveTrack(proj.song, args.track);
            const t = proj.song.tracks[ti];
            const before = t.evs.filter((e) => (e.status & 0xf0) === 0x90 && e.d2 > 0).length;
            if (args.replace) t.evs = t.evs.filter((e) => (e.status & 0xf0) !== 0x90 && (e.status & 0xf0) !== 0x80);

            const ch = t.channel & 0x0f;
            let lastBeat = 0;
            for (const n of args.notes) {
                const pitch = noteToNumber(n.pitch);
                const start = requireNum(n.start, 'start', { min: 0 });
                const dur = n.duration === undefined ? 1 : requireNum(n.duration, 'duration', { min: 0.001 });
                const vel = n.velocity === undefined ? 100 : clamp(Math.round(requireNum(n.velocity, 'velocity')), 1, 127);
                const tick = Math.round(start * ppqn);
                t.evs.push({ tick, status: 0x90 | ch, d1: pitch, d2: vel });
                t.evs.push({ tick: tick + Math.max(1, Math.round(dur * ppqn)), status: 0x80 | ch, d1: pitch, d2: 0 });
                lastBeat = Math.max(lastBeat, start + dur);
            }
            saveProjectFile(file, proj);
            const after = t.evs.filter((e) => (e.status & 0xf0) === 0x90 && e.d2 > 0).length;
            return `"${t.name}" 트랙에 노트 ${args.notes.length}개를 찍었습니다` +
                   `${args.replace ? ` (기존 ${before}개 삭제)` : ''}.\n` +
                   `트랙 노트 수 ${after}개, 마지막 노트 끝 ${lastBeat}박 (약 ${(lastBeat / 4 + 1).toFixed(2)}마디, 4/4 기준) — ${file}`;
        },
    },

    // --- 코드 진행 -------------------------------------------------------
    midipro_add_chords: {
        description:
            '코드 심볼로 화음을 찍는다. 예: ["C", "Am", "F", "G7"]. 기본으로 코드당 4박(4/4 한 마디)씩 이어 붙인다. ' +
            '쓸 수 있는 종류: maj, m, 7, maj7, m7, m7b5, dim, dim7, aug, sus2, sus4, 6, 9, add9, 11, 13, 그리고 "G7/B" 같은 분수코드.',
        inputSchema: {
            type: 'object',
            properties: {
                path: { type: 'string' },
                track: { description: '트랙 번호 또는 이름' },
                chords: {
                    type: 'array',
                    description: '코드 심볼 문자열 배열, 또는 {symbol,start,duration} 객체 배열',
                    items: {},
                },
                startBeat: { type: 'number', description: '첫 코드 시작 위치 (박, 기본 0)' },
                beatsPerChord: { type: 'number', description: '코드 하나의 길이 (박, 기본 4)' },
                octave: { type: 'integer', description: '근음 옥타브 (기본 3 = C3)' },
                velocity: { type: 'integer', description: '세기 (기본 80)' },
                replace: { type: 'boolean', description: '트랙의 기존 노트를 지우고 새로 찍기' },
            },
            required: ['path', 'track', 'chords'],
        },
        run(args) {
            if (!Array.isArray(args.chords) || args.chords.length === 0)
                throw new Error('chords: 코드를 하나 이상 넣어주세요');
            const file = path.resolve(String(args.path));
            const proj = loadProjectFile(file);
            const ppqn = projectPpqn(proj);
            const ti = resolveTrack(proj.song, args.track);
            const t = proj.song.tracks[ti];
            if (args.replace) t.evs = t.evs.filter((e) => (e.status & 0xf0) !== 0x90 && (e.status & 0xf0) !== 0x80);

            const per = args.beatsPerChord === undefined ? 4 : requireNum(args.beatsPerChord, 'beatsPerChord', { min: 0.25 });
            const oct = args.octave === undefined ? 3 : requireNum(args.octave, 'octave', { min: -1, max: 8 });
            const vel = args.velocity === undefined ? 80 : clamp(Math.round(requireNum(args.velocity, 'velocity')), 1, 127);
            const ch = t.channel & 0x0f;
            let cursor = args.startBeat === undefined ? 0 : requireNum(args.startBeat, 'startBeat', { min: 0 });

            const written = [];
            for (const item of args.chords) {
                const symbol = typeof item === 'string' ? item : item.symbol;
                if (!symbol) throw new Error(`코드 항목에 symbol이 없습니다: ${JSON.stringify(item)}`);
                const start = (typeof item === 'object' && item.start !== undefined)
                    ? requireNum(item.start, 'start', { min: 0 }) : cursor;
                const dur = (typeof item === 'object' && item.duration !== undefined)
                    ? requireNum(item.duration, 'duration', { min: 0.001 }) : per;

                const { rootPc, intervals, bassPc } = parseChord(symbol);
                const rootMidi = (oct + 1) * 12 + rootPc;
                const pitches = intervals.map((iv) => rootMidi + iv);
                if (bassPc !== null) {
                    let bass = (oct + 1) * 12 + bassPc;
                    while (bass >= rootMidi) bass -= 12;   // 근음보다 아래로
                    pitches.unshift(bass);
                }
                const tick = Math.round(start * ppqn);
                const offTick = tick + Math.max(1, Math.round(dur * ppqn));
                for (const p of pitches) {
                    if (p < 0 || p > 127) continue;
                    t.evs.push({ tick, status: 0x90 | ch, d1: p, d2: vel });
                    t.evs.push({ tick: offTick, status: 0x80 | ch, d1: p, d2: 0 });
                }
                written.push(`${symbol} (${start}박, ${pitches.filter((p) => p >= 0 && p <= 127).map(numberToNote).join(' ')})`);
                cursor = start + dur;
            }
            saveProjectFile(file, proj);
            return `"${t.name}" 트랙에 코드 ${written.length}개를 찍었습니다 (${cursor}박까지):\n  ` +
                   written.join('\n  ') + `\n— ${file}`;
        },
    },

    // --- 드럼 패턴 -------------------------------------------------------
    midipro_add_drums: {
        description:
            '스텝 문자열로 드럼 패턴을 찍는다. 예: {"kick":"x---x---","snare":"----x---","hat":"xxxxxxxx"}. ' +
            'x/o = 치기, -/./공백 = 쉼, 1~9 = 세기(9가 가장 셈). 기본은 한 칸이 16분음표(stepsPerBeat=4). ' +
            '악기 이름: kick, snare, hat, openhat, rim, clap, crash, ride, tom1~tom4, cowbell, tambourine, shaker 등.',
        inputSchema: {
            type: 'object',
            properties: {
                path: { type: 'string' },
                track: { description: '드럼 트랙 번호 또는 이름 (채널 10 트랙 권장)' },
                pattern: { type: 'object', description: '악기 이름 -> 스텝 문자열', additionalProperties: { type: 'string' } },
                stepsPerBeat: { type: 'integer', description: '한 박에 들어가는 칸 수 (기본 4 = 16분음표)' },
                startBeat: { type: 'number', description: '시작 위치 (박, 기본 0)' },
                repeat: { type: 'integer', description: '패턴 반복 횟수 (기본 1)' },
                velocity: { type: 'integer', description: 'x로 찍을 때의 기본 세기 (기본 100)' },
                replace: { type: 'boolean', description: '트랙의 기존 노트를 지우고 새로 찍기' },
            },
            required: ['path', 'track', 'pattern'],
        },
        run(args) {
            const pat = args.pattern;
            if (!pat || typeof pat !== 'object' || Object.keys(pat).length === 0)
                throw new Error('pattern: {"kick":"x---x---"} 형태로 하나 이상 넣어주세요');
            const file = path.resolve(String(args.path));
            const proj = loadProjectFile(file);
            const ppqn = projectPpqn(proj);
            const ti = resolveTrack(proj.song, args.track);
            const t = proj.song.tracks[ti];
            if (args.replace) t.evs = t.evs.filter((e) => (e.status & 0xf0) !== 0x90 && (e.status & 0xf0) !== 0x80);

            const spb = args.stepsPerBeat === undefined ? 4 : requireNum(args.stepsPerBeat, 'stepsPerBeat', { min: 1, max: 16 });
            const reps = args.repeat === undefined ? 1 : requireNum(args.repeat, 'repeat', { min: 1, max: 256 });
            const baseVel = args.velocity === undefined ? 100 : clamp(Math.round(requireNum(args.velocity, 'velocity')), 1, 127);
            const start0 = args.startBeat === undefined ? 0 : requireNum(args.startBeat, 'startBeat', { min: 0 });
            const stepTicks = ppqn / spb;
            const ch = t.channel & 0x0f;
            if (ch !== 9)
                log(`[경고] "${t.name}"은 드럼 채널(10)이 아닙니다 — GM 드럼 소리가 안 날 수 있습니다`);

            let steps = 0, hits = 0;
            const used = [];
            for (const [rawName, rawPat] of Object.entries(pat)) {
                const key = String(rawName).toLowerCase().replace(/[\s_-]/g, '');
                const note = GM_DRUMS[key];
                if (note === undefined)
                    throw new Error(`모르는 드럼 악기: "${rawName}" — 쓸 수 있는 이름: ${Object.keys(GM_DRUMS).join(', ')}`);
                const cells = String(rawPat).replace(/\|/g, '').split('');
                steps = Math.max(steps, cells.length);
                let n = 0;
                for (let r = 0; r < reps; r++) {
                    for (let s = 0; s < cells.length; s++) {
                        const c = cells[s];
                        if (c === '-' || c === '.' || c === ' ' || c === '0') continue;
                        let vel = baseVel;
                        if (/[1-9]/.test(c)) vel = clamp(Math.round((Number(c) / 9) * 127), 1, 127);
                        else if (!/[xXoO]/.test(c))
                            throw new Error(`패턴에 쓸 수 없는 글자 "${c}" (${rawName}) — x, o, -, ., 1~9만 됩니다`);
                        const tick = Math.round((start0 * ppqn) + (r * cells.length + s) * stepTicks);
                        t.evs.push({ tick, status: 0x90 | ch, d1: note, d2: vel });
                        t.evs.push({ tick: tick + Math.max(1, Math.round(stepTicks / 2)), status: 0x80 | ch, d1: note, d2: 0 });
                        n++; hits++;
                    }
                }
                used.push(`${rawName}(${numberToNote(note)}) ${n}회`);
            }
            saveProjectFile(file, proj);
            const lenBeats = (steps * reps) / spb;
            return `"${t.name}" 트랙에 드럼 ${hits}타를 찍었습니다 — ${used.join(', ')}\n` +
                   `길이 ${lenBeats}박 (약 ${(lenBeats / 4).toFixed(2)}마디), ${reps}회 반복 — ${file}`;
        },
    },

    // --- 템포 -----------------------------------------------------------
    midipro_set_tempo: {
        description: '프로젝트의 기본 템포(BPM)를 바꾼다.',
        inputSchema: {
            type: 'object',
            properties: {
                path: { type: 'string' },
                bpm: { type: 'number', description: '20~400' },
            },
            required: ['path', 'bpm'],
        },
        run(args) {
            const file = path.resolve(String(args.path));
            const proj = loadProjectFile(file);
            const old = getGlobal(proj.song.globals, 'bpm', '120');
            const bpm = requireNum(args.bpm, 'bpm', { min: 20, max: 400 });
            setGlobal(proj.song.globals, 'bpm', bpm);
            saveProjectFile(file, proj);
            return `템포를 ${old} -> ${bpm} BPM으로 바꿨습니다 — ${file}`;
        },
    },

    // --- 드럼 킷 (샘플 자동 배정) ------------------------------------------
    midipro_set_drumkit: {
        description:
            '드럼을 내장 신디 대신 실제 WAV 샘플로 소리나게 한다. 프로젝트의 드럼 트랙이 쓰는 노트를 찾아 ' +
            '드럼 라이브러리에서 킥/스네어/햇/탐/심벌을 자동으로 골라 배정한다. ' +
            'kit을 주면 그 드럼머신에서만 고르고, 안 주면 필요한 악기를 가장 많이 갖춘 킷을 자동으로 고른다. ' +
            '사용 가능한 킷은 midipro_list_drumkits로 확인.',
        inputSchema: {
            type: 'object',
            properties: {
                path: { type: 'string', description: '.midipro 파일' },
                kit: { type: 'string', description: '드럼머신 이름 일부 (예: "909", "Akai"). 생략하면 자동 선택' },
                family: {
                    type: 'string',
                    description: '킷 안의 음색 계열 (예: "Acoustic", "Trap", "Urban"). ' +
                                 '생략하면 필요한 악기를 가장 많이 갖춘 계열. 어쿠스틱 드럼을 원하면 "Acoustic".',
                },
                notes: {
                    type: 'array', items: { type: 'integer' },
                    description: '배정할 노트 번호들 (생략하면 드럼 트랙이 실제로 쓰는 노트 전부)',
                },
                dryRun: { type: 'boolean', description: 'true면 파일을 고치지 않고 어떤 샘플이 걸릴지만 보여준다' },
            },
            required: ['path'],
        },
        run(args) {
            const file = path.resolve(String(args.path));
            const proj = loadProjectFile(file);
            const lib = scanDrumLib();
            if (!lib.root)
                throw new Error('드럼 샘플 라이브러리를 찾지 못했습니다 ' +
                                '(<저장소>\\src\\Drum 또는 %LOCALAPPDATA%\\MidiPro\\Drum, 환경변수 MIDIPRO_DRUMLIB)');
            if (!lib.files.length) throw new Error(`라이브러리에 WAV가 없습니다: ${lib.root}`);

            const notes = Array.isArray(args.notes) && args.notes.length
                ? args.notes.map((n) => clamp(Math.round(Number(n)), 0, 127))
                : usedDrumNotes(proj.song);
            if (!notes.length)
                throw new Error('드럼 트랙(채널 10)에 노트가 없습니다. 먼저 midipro_add_drums로 패턴을 찍으세요.');

            const wanted = [...new Set(notes.map((n) => NOTE_BUCKET[n] ?? 'etc'))];
            let kitName, files;
            if (args.kit) {
                const low = String(args.kit).toLowerCase();
                const hit = [...lib.kits.keys()].find((k) => k.toLowerCase() === low)
                         ?? [...lib.kits.keys()].find((k) => k.toLowerCase().includes(low));
                if (!hit) throw new Error(`"${args.kit}"에 맞는 드럼머신이 없습니다 (킷 ${lib.kits.size}개) — midipro_list_drumkits로 확인하세요`);
                kitName = hit;
                files = lib.kits.get(hit);
            } else {
                // 필요한 악기를 가장 많이 갖춘 킷 (동점이면 샘플이 많은 쪽)
                let best = null;
                for (const [k, fs2] of lib.kits) {
                    const cov = kitCoverage(fs2, wanted);
                    if (!best || cov > best.cov || (cov === best.cov && fs2.length > best.files.length))
                        best = { k, cov, files: fs2 };
                }
                kitName = best.k;
                files = best.files;
            }

            const { assigned, missing, family } = assignKit(files, notes, args.family);
            if (!assigned.length) throw new Error(`"${kitName}" 킷에서 쓸 수 있는 샘플을 못 찾았습니다`);

            const lines = assigned.map((a) =>
                `  ${a.note} ${DRUM_NOTE_NAME[a.note] ?? ''} (${BUCKET_LABEL[a.bucket]}) -> ${path.basename(a.file.full)}` +
                (family && !a.fromFamily ? ' [계열 밖]' : ''));
            const head = `드럼 킷: ${kitName}${family ? ` / ${family} 계열` : ''}\n` +
                         `라이브러리: ${lib.root} (WAV ${lib.files.length}개, 킷 ${lib.kits.size}개)\n` +
                         `배정 ${assigned.length}개:\n${lines.join('\n')}` +
                         (missing.length ? `\n※ 샘플을 못 찾아 내장 신디로 남는 노트: ${missing.join(', ')}` : '');
            if (args.dryRun) return `[미리보기 — 파일은 고치지 않았습니다]\n${head}`;

            // 기존 drumsample 줄을 걷어내고 새로 쓴다 (헤더는 [song] 앞)
            proj.header = proj.header.filter((L) => !L.startsWith('drumsample '));
            for (const a of assigned) proj.header.push(`drumsample ${a.note} ${a.file.full}`);
            saveProjectFile(file, proj);
            return `${head}\n앱에서 프로젝트를 열면 이 샘플로 소리납니다 — ${file}`;
        },
    },

    midipro_list_drumkits: {
        description:
            '드럼 샘플 라이브러리에 있는 드럼머신(킷) 목록을 보여준다. 각 킷이 어떤 악기를 갖췄는지도 함께.',
        inputSchema: {
            type: 'object',
            properties: {
                filter: { type: 'string', description: '이름에 이 낱말이 든 킷만 (예: "roland", "909")' },
                limit: { type: 'integer', description: '최대 개수 (기본 40)' },
            },
        },
        run(args) {
            const lib = scanDrumLib();
            if (!lib.root) throw new Error('드럼 샘플 라이브러리를 찾지 못했습니다');
            const want = ['kick', 'snare', 'hatclosed', 'hatopen', 'tom', 'crash', 'ride', 'clap'];
            const low = args.filter ? String(args.filter).toLowerCase() : null;
            const rows = [...lib.kits.entries()]
                .filter(([k]) => !low || k.toLowerCase().includes(low))
                .map(([k, fs2]) => ({
                    kit: k, samples: fs2.length,
                    has: want.filter((b) => fs2.some((f) => hasBucket(f, b))).map((b) => BUCKET_LABEL[b]),
                    families: [...new Set(fs2.map((f) => f.family).filter(Boolean))].sort(),
                }))
                .sort((a, b) => b.has.length - a.has.length || b.samples - a.samples);
            const limit = args.limit === undefined ? 40 : requireNum(args.limit, 'limit', { min: 1, max: 500 });
            return JSON.stringify({
                root: lib.root, totalWav: lib.files.length, kitCount: lib.kits.size,
                shown: Math.min(limit, rows.length),
                kits: rows.slice(0, limit),
            }, null, 2);
        },
    },

    // --- 트랙 악기 (VSTi) -------------------------------------------------
    midipro_set_instrument: {
        description:
            '트랙에 VST3 악기(VSTi)를 얹는다. 프로젝트를 열면 앱이 실제로 로드한다. ' +
            '플러그인은 이름("Surge XT")이나 .vst3 전체 경로로 지정한다. 트랙당 악기는 하나라 기존 악기는 교체된다. ' +
            '쓸 수 있는 목록은 midipro_status로 확인.',
        inputSchema: {
            type: 'object',
            properties: {
                path: { type: 'string', description: '.midipro 파일' },
                track: { description: '트랙 번호 또는 이름' },
                plugin: { type: 'string', description: '악기 이름 또는 .vst3 경로 (예: "Surge XT")' },
            },
            required: ['path', 'track', 'plugin'],
        },
        run(args) {
            const file = path.resolve(String(args.path));
            const proj = loadProjectFile(file);
            const ti = resolveTrack(proj.song, args.track);
            const t = proj.song.tracks[ti];
            const vst = resolveVst(args.plugin, true);
            const had = t.lines.filter((L) => L.startsWith('tplugin 1 ')).length;
            t.lines = t.lines.filter((L) => !L.startsWith('tplugin 1 ')); // 악기는 트랙당 1개
            insertPluginLine(t, pluginLine(true, vst.name, vst.path));
            saveProjectFile(file, proj);
            return `"${t.name}" 트랙 악기: ${vst.name}${had ? ' (기존 악기 교체)' : ''}\n` +
                   `${vst.path}\n앱에서 프로젝트를 열면 로드됩니다 — ${file}`;
        },
    },

    // --- 트랙 이펙트 ------------------------------------------------------
    midipro_add_effect: {
        description:
            '트랙 FX 체인에 이펙트를 추가한다(맨 뒤에 붙는다). VST3 이펙트는 이름이나 .vst3 경로로, ' +
            '내장 이펙트는 eq / delay / reverb / limiter / comp 로 지정한다. 프로젝트를 열면 앱이 실제로 로드한다.',
        inputSchema: {
            type: 'object',
            properties: {
                path: { type: 'string', description: '.midipro 파일' },
                track: { description: '트랙 번호 또는 이름' },
                effect: { type: 'string', description: 'VST3 이름/경로, 또는 내장: eq, delay, reverb, limiter, comp' },
            },
            required: ['path', 'track', 'effect'],
        },
        run(args) {
            const file = path.resolve(String(args.path));
            const proj = loadProjectFile(file);
            const ti = resolveTrack(proj.song, args.track);
            const t = proj.song.tracks[ti];
            const key = String(args.effect).trim().toLowerCase();
            const builtin = BUILTIN_ALIAS[key];
            let label;
            if (builtin) {
                label = BUILTIN_FX[builtin];
                insertPluginLine(t, pluginLine(false, label, `builtin:${builtin}`));
            } else {
                const vst = resolveVst(args.effect, false);
                label = vst.name;
                insertPluginLine(t, pluginLine(false, vst.name, vst.path));
            }
            saveProjectFile(file, proj);
            const chain = t.lines.filter((L) => L.startsWith('tplugin 0 '))
                                 .map((L) => L.split('\t')[0].split(/\s+/).slice(4).join(' '));
            return `"${t.name}" 트랙에 이펙트 추가: ${label}\n` +
                   `FX 체인: ${chain.join(' -> ')} — ${file}`;
        },
    },

    // --- MIDI 파일 가져오기 ----------------------------------------------
    midipro_import_midi: {
        description:
            '.mid 파일을 프로젝트로 가져온다. track을 주면 그 트랙에 합치고, 안 주면 MIDI의 트랙/채널마다 ' +
            '새 트랙을 만든다. 노트 외의 채널 메시지(CC, 피치벤드, 프로그램 체인지)도 함께 가져온다.',
        inputSchema: {
            type: 'object',
            properties: {
                path: { type: 'string', description: '가져올 대상 .midipro 파일' },
                midiPath: { type: 'string', description: '읽을 .mid 파일' },
                track: { description: '이 트랙에 합치기 (번호 또는 이름). 생략하면 새 트랙들을 만든다' },
                startBeat: { type: 'number', description: '이 위치부터 놓기 (박, 기본 0)' },
                applyTempo: { type: 'boolean', description: 'MIDI의 첫 템포를 프로젝트 템포로 적용 (기본 true)' },
            },
            required: ['path', 'midiPath'],
        },
        run(args) {
            const midiFile = path.resolve(String(args.midiPath));
            if (!existsSync(midiFile)) throw new Error(`MIDI 파일이 없습니다: ${midiFile}`);
            const file = path.resolve(String(args.path));
            const proj = loadProjectFile(file);
            const ppqn = projectPpqn(proj);
            const smf = parseSmf(readFileSync(midiFile));
            const scale = ppqn / smf.division;
            const offset = Math.round((args.startBeat === undefined ? 0 : requireNum(args.startBeat, 'startBeat', { min: 0 })) * ppqn);

            // 소스 트랙 + 채널로 묶는다 — 포맷 0은 한 트랙에 여러 채널이 섞여 있다.
            const groups = new Map();
            for (const [si, st] of smf.tracks.entries())
                for (const e of st.events) {
                    const ch = e.status & 0x0f;
                    const key = `${si}:${ch}`;
                    if (!groups.has(key)) groups.set(key, { srcName: st.name, channel: ch, events: [] });
                    groups.get(key).events.push(e);
                }
            if (groups.size === 0) throw new Error('MIDI 파일에 연주 데이터(채널 메시지)가 없습니다');

            const rescale = (e, ch) => ({
                tick: Math.round(e.tick * scale) + offset,
                status: (e.status & 0xf0) | (ch & 0x0f), d1: e.d1, d2: e.d2,
            });
            const countNotes = (evs) => evs.filter((e) => (e.status & 0xf0) === 0x90 && e.d2 > 0).length;

            const report = [];
            let total = 0;
            if (args.track !== undefined && args.track !== null) {
                const ti = resolveTrack(proj.song, args.track);
                const t = proj.song.tracks[ti];
                for (const g of groups.values())
                    for (const e of g.events) t.evs.push(rescale(e, t.channel));
                total = [...groups.values()].reduce((s, g) => s + countNotes(g.events), 0);
                report.push(`"${t.name}" 트랙에 합침`);
            } else {
                // 같은 소스 트랙이 여러 채널을 쓰면 이름 뒤에 채널을 붙여 구분한다
                const perSrc = new Map();
                for (const [key, g] of groups) {
                    const si = key.split(':')[0];
                    perSrc.set(si, (perSrc.get(si) ?? 0) + 1);
                }
                let n = 0;
                for (const [key, g] of groups) {
                    const si = key.split(':')[0];
                    const base = g.srcName || `MIDI ${++n}`;
                    const name = perSrc.get(si) > 1 ? `${base} ch${g.channel + 1}` : base;
                    const t = makeTrackBlock({ name, channel: g.channel });
                    t.channel = g.channel; // 드럼 채널(9)도 그대로 살린다
                    for (const e of g.events) t.evs.push(rescale(e, g.channel));
                    proj.song.tracks.push(t);
                    const c = countNotes(g.events);
                    total += c;
                    report.push(`[${proj.song.tracks.length - 1}] ${name} — 채널 ${g.channel + 1}, 노트 ${c}개`);
                }
            }

            const tempos = smf.tracks.flatMap((t) => t.tempos).sort((a, b) => a.tick - b.tick);
            let tempoMsg = '';
            if ((args.applyTempo ?? true) && tempos.length) {
                const bpm = Math.round((60000000 / tempos[0].us) * 1000) / 1000;
                const old = getGlobal(proj.song.globals, 'bpm', '120');
                setGlobal(proj.song.globals, 'bpm', bpm);
                tempoMsg = `\n템포: ${old} -> ${bpm} BPM (MIDI 파일 기준)`;
            }
            saveProjectFile(file, proj);
            return `MIDI를 가져왔습니다: ${path.basename(midiFile)} ` +
                   `(포맷 ${smf.format}, ppqn ${smf.division} -> ${ppqn}, 노트 ${total}개)\n` +
                   report.join('\n') + tempoMsg + `\n— ${file}`;
        },
    },

    // --- MIDI 파일 내보내기 ----------------------------------------------
    midipro_export_midi: {
        description:
            '프로젝트를 표준 .mid 파일(포맷 1)로 내보낸다. 다른 DAW로 옮기거나 공유할 때 쓴다. ' +
            '오디오 클립·VST 설정은 담기지 않는다 (MIDI 포맷에 그런 개념이 없다).',
        inputSchema: {
            type: 'object',
            properties: {
                path: { type: 'string', description: '읽을 .midipro 파일' },
                midiPath: { type: 'string', description: '만들 .mid 파일 경로' },
                tracks: { type: 'array', items: { type: 'integer' }, description: '내보낼 트랙 번호들 (생략하면 전부)' },
                overwrite: { type: 'boolean', description: '기존 .mid 덮어쓰기 (기본 false)' },
            },
            required: ['path', 'midiPath'],
        },
        run(args) {
            const file = path.resolve(String(args.path));
            const out = path.resolve(String(args.midiPath));
            if (existsSync(out) && !args.overwrite)
                throw new Error(`이미 있는 파일입니다: ${out} (덮어쓰려면 overwrite=true)`);
            const proj = loadProjectFile(file);
            const which = Array.isArray(args.tracks) && args.tracks.length
                ? args.tracks.map((i) => resolveTrack(proj.song, i)) : null;
            const { buffer, notes, trackChunks } = projectToSmf(proj, which);
            mkdirSync(path.dirname(out), { recursive: true });
            writeFileSync(out, buffer);
            return `MIDI로 내보냈습니다: ${out}\n` +
                   `포맷 1, ppqn ${projectPpqn(proj)}, 템포 ${getGlobal(proj.song.globals, 'bpm', '120')} BPM, ` +
                   `트랙 ${trackChunks}개(템포 트랙 포함), 노트 ${notes}개, ${buffer.length}바이트`;
        },
    },

    // --- 실행 중인 앱 제어 -------------------------------------------------
    midipro_transport: {
        description:
            '실행 중인 MidiPro를 그 자리에서 조종한다 (앱을 닫지 않아도 된다). ' +
            'play/stop/toggle/rewind = 재생 제어, seek = 위치 이동(박), tempo = 템포 변경, ' +
            'status = 지금 상태(재생 여부·위치·트랙), save = 열려 있는 파일에 저장, ' +
            'reload = 디스크에서 다시 불러오기. ' +
            '**MCP로 파일을 고친 뒤 reload를 부르면 앱을 껐다 켜지 않고 바로 반영된다.**',
        inputSchema: {
            type: 'object',
            properties: {
                action: {
                    type: 'string',
                    enum: ['status', 'play', 'stop', 'toggle', 'rewind', 'seek', 'tempo', 'save', 'reload', 'open'],
                    description: '무엇을 할지',
                },
                beat: { type: 'number', description: 'seek일 때 이동할 위치 (박, 0이 곡 시작)' },
                bpm: { type: 'number', description: 'tempo일 때 새 템포 (20~400)' },
                path: { type: 'string', description: 'open일 때 열 .midipro 경로' },
            },
            required: ['action'],
        },
        run(args) {
            const a = String(args.action);
            let cmd = a;
            if (a === 'seek') {
                const b = requireNum(args.beat, 'beat', { min: 0 });
                cmd = `seek ${b}`;
            } else if (a === 'tempo') {
                const b = requireNum(args.bpm, 'bpm', { min: 20, max: 400 });
                cmd = `tempo ${b}`;
            } else if (a === 'open') {
                if (!args.path) throw new Error('open: path가 필요합니다');
                const p = path.resolve(String(args.path));
                if (!existsSync(p)) throw new Error(`파일이 없습니다: ${p}`);
                cmd = `open ${p}`;
            }
            const r = controlSend(cmd);
            if (!r.ok) throw new Error(r.error ?? '앱이 명령을 거부했습니다');
            if (a !== 'status') return r.message ?? '완료';

            const bar = (r.positionBeat / 4) + 1; // 4/4 기준 표시용
            const lines = (r.tracks ?? []).map((t) =>
                `  [${t.index}] ${t.name} — 채널 ${t.channel + 1}${t.muted ? ', 뮤트' : ''}, 노트 ${t.notes}개`);
            return `상태: ${r.playing ? '재생 중' : '정지'}${r.recording ? ' (녹음)' : ''}\n` +
                   `위치: ${r.positionBeat.toFixed(3)}박 (약 ${bar.toFixed(2)}마디, 4/4) / ${r.positionTick}틱\n` +
                   `템포: ${r.bpm} BPM, ppqn ${r.ppqn}\n` +
                   `프로젝트: ${r.projectPath || '(저장한 적 없는 새 곡)'}\n` +
                   `트랙 ${r.trackCount}개:\n${lines.join('\n')}`;
        },
    },

    // --- 플러그인 음색(프리셋) ---------------------------------------------
    midipro_preset: {
        description:
            '트랙에 얹은 VST 플러그인의 음색(프리셋)을 저장하거나 적용한다. 실행 중인 앱이 필요하다.\n' +
            'save = 지금 그 플러그인의 음색을 이름 붙여 보관 (앱에서 손으로 만든 톤을 재사용할 수 있다)\n' +
            'load = 보관한 음색이나 .vstpreset 파일을 그 트랙에 적용\n' +
            'list = 보관된 음색 목록\n' +
            '※ MCP가 음색을 "만들어" 내지는 못한다 — 플러그인 내부 데이터라 한 번은 앱에서 손으로 잡아야 한다.',
        inputSchema: {
            type: 'object',
            properties: {
                action: { type: 'string', enum: ['save', 'load', 'list'] },
                track: { type: 'integer', description: '트랙 번호 (0부터). save/load에 필요' },
                slot: {
                    type: 'integer',
                    description: '-1 = 트랙 악기(기본), 0 이상 = 그 번호의 트랙 이펙트',
                },
                name: { type: 'string', description: '보관할/불러올 음색 이름 (예: "메탈 리드")' },
                file: { type: 'string', description: 'name 대신 파일 경로를 직접 (.mppreset / .vstpreset)' },
            },
            required: ['action'],
        },
        run(args) {
            const dir = path.join(dataDir(), 'presets');
            const a = String(args.action);
            if (a === 'list') {
                mkdirSync(dir, { recursive: true });
                const mine = readdirSync(dir, { withFileTypes: true })
                    .filter((e) => e.isFile() && /\.mppreset$/i.test(e.name))
                    .map((e) => ({ name: e.name.replace(/\.mppreset$/i, ''), path: path.join(dir, e.name) }));
                // 표준 .vstpreset 위치도 훑는다 (다른 프로그램에서 내보낸 것)
                const std = [];
                for (const root of [
                    path.join(process.env.USERPROFILE ?? '', 'Documents', 'VST3 Presets'),
                    path.join(process.env.PROGRAMDATA ?? '', 'VST3 Presets'),
                    path.join(process.env.APPDATA ?? '', 'VST3 Presets'),
                ]) {
                    if (!root || !existsSync(root)) continue;
                    const walk = (d, depth) => {
                        if (depth > 4) return;
                        let ents;
                        try { ents = readdirSync(d, { withFileTypes: true }); } catch { return; }
                        for (const e of ents) {
                            const full = path.join(d, e.name);
                            if (e.isDirectory()) walk(full, depth + 1);
                            else if (/\.vstpreset$/i.test(e.name))
                                std.push({ name: path.basename(e.name, path.extname(e.name)), path: full });
                        }
                    };
                    walk(root, 0);
                }
                return JSON.stringify({
                    presetDir: dir,
                    saved: mine,
                    vstPresets: std.slice(0, 200),
                    note: mine.length || std.length ? undefined
                        : '보관된 음색이 없습니다. 앱에서 플러그인 음색을 잡은 뒤 action="save"로 보관하세요.',
                }, null, 2);
            }

            const ti = requireNum(args.track, 'track', { min: 0 });
            const slot = args.slot === undefined ? -1 : requireNum(args.slot, 'slot', { min: -1 });
            let file = args.file ? path.resolve(String(args.file)) : null;
            if (!file) {
                if (!args.name) throw new Error('name 또는 file 중 하나가 필요합니다');
                const safe = String(args.name).replace(/[\\/:*?"<>|]/g, '_');
                file = path.join(dir, safe + '.mppreset');
            }
            if (a === 'save') mkdirSync(path.dirname(file), { recursive: true });
            else if (!existsSync(file)) throw new Error(`음색 파일이 없습니다: ${file}`);

            const r = controlSend(`${a === 'save' ? 'presetsave' : 'presetload'} ${ti} ${slot} ${file}`);
            if (!r.ok) throw new Error(r.error ?? '앱이 명령을 거부했습니다');
            return (r.message ?? '완료') +
                   (a === 'save' ? '\n※ 이 음색은 다른 프로젝트에서도 load로 쓸 수 있습니다.'
                                 : '\n※ 프로젝트에 남기려면 midipro_transport(action="save")로 저장하세요.');
        },
    },

    // --- 앱으로 열기 -----------------------------------------------------
    midipro_open: {
        description:
            '만든 프로젝트를 MidiPro 앱으로 연다. MidiPro가 이미 실행 중이면 새 창이 하나 더 뜨므로, ' +
            '먼저 열려 있는 창을 닫는 편이 좋다 (열려 있는 프로젝트가 저장되면서 파일이 덮어써질 수 있음).',
        inputSchema: {
            type: 'object',
            properties: {
                path: { type: 'string', description: '열 .midipro 파일 경로 (생략하면 앱만 실행)' },
            },
        },
        run(args) {
            const exe = findMidiProExe();
            if (!exe)
                throw new Error('MidiPro.exe를 찾지 못했습니다. 환경변수 MIDIPRO_EXE에 전체 경로를 넣어주세요.');
            const file = args.path ? path.resolve(String(args.path)) : null;
            if (file && !existsSync(file)) throw new Error(`파일이 없습니다: ${file}`);
            const warn = isMidiProRunning()
                ? '\n[주의] MidiPro가 이미 실행 중입니다 — 창이 하나 더 뜹니다. 기존 창이 저장하면 이 파일을 덮어쓸 수 있습니다.'
                : '';
            const child = spawn(exe, file ? [file] : [], {
                detached: true, stdio: 'ignore', cwd: path.dirname(exe), windowsHide: false,
            });
            child.unref();
            return `MidiPro를 실행했습니다: ${exe}${file ? `\n프로젝트: ${file}` : ''}${warn}`;
        },
    },

    // --- 환경 정보 -------------------------------------------------------
    midipro_status: {
        description:
            'MidiPro 설치/실행 상태, 스캔된 VST3 플러그인 목록, 최근 연 프로젝트 목록을 알려준다.',
        inputSchema: { type: 'object', properties: {} },
        run() {
            const exe = findMidiProExe();
            const ad = dataDir();
            const readLines = (f) => existsSync(f) ? readFileSync(f, 'utf8').split(/\r?\n/).filter(Boolean) : [];

            const plugins = readLines(path.join(ad, 'plugincache.ini')).slice(1).map((L) => {
                const m = /^(\d+)\s+(\d+)\s+(\d)\s+(\d)\s+(.+)$/.exec(L);
                if (!m) return null;
                return {
                    path: m[5], name: path.basename(m[5], '.vst3'),
                    instrument: m[3] === '1', effect: m[4] === '1',
                };
            }).filter(Boolean);

            const sess = openProjectState(ad); // 열려 있는 프로젝트 추정용
            return JSON.stringify({
                exe: exe ?? '(찾지 못함 — 환경변수 MIDIPRO_EXE로 지정하세요)',
                running: isMidiProRunning(),
                controlChannel: controlAlive(), // true면 midipro_transport로 조종할 수 있다
                sessionLive: sess.live,
                likelyOpenProject: sess.live ? sess.top : null,
                userDataDir: ad,
                crashLog: existsSync(path.join(ad, 'crash.log')) ? path.join(ad, 'crash.log') : null,
                plugins,
                recentProjects: readLines(path.join(ad, 'recent.txt')),
            }, null, 2);
        },
    },
};

// 프로젝트를 고치는 도구 전부에 force를 달아 준다 (스키마를 열 번 손으로
// 적으면 새 도구를 추가할 때 빠뜨리기 쉽다).
for (const n of MUTATING_TOOLS) {
    const t = TOOLS[n];
    if (!t) throw new Error(`MUTATING_TOOLS에 없는 도구: ${n}`); // 오타 방지
    t.inputSchema.properties.force = {
        type: 'boolean',
        description: '앱이 이 프로젝트를 열고 있어도 강행한다 (앱이 저장하면 편집이 덮어써진다)',
    };
}

// ------------------------------------------------------------------
// MCP (JSON-RPC 2.0 over stdio)
// ------------------------------------------------------------------
const send = (msg) => process.stdout.write(JSON.stringify(msg) + '\n');
const ok = (id, result) => send({ jsonrpc: '2.0', id, result });
const fail = (id, code, message) => send({ jsonrpc: '2.0', id, error: { code, message } });

function handle(msg) {
    const { id, method, params } = msg;
    const isNotification = id === undefined || id === null;

    switch (method) {
        case 'initialize':
            return ok(id, {
                // 클라이언트가 요청한 버전을 그대로 받아준다 (모르는 버전이면 우리 기준으로)
                protocolVersion: params?.protocolVersion ?? '2025-06-18',
                capabilities: { tools: { listChanged: false } },
                serverInfo: { name: SERVER_NAME, version: SERVER_VERSION },
            });

        case 'notifications/initialized':
        case 'notifications/cancelled':
            return; // 알림에는 응답하지 않는다

        case 'ping':
            return ok(id, {});

        case 'tools/list':
            return ok(id, {
                tools: Object.entries(TOOLS).map(([name, t]) => ({
                    name, description: t.description, inputSchema: t.inputSchema,
                })),
            });

        case 'tools/call': {
            const name = params?.name;
            const tool = TOOLS[name];
            if (!tool)
                return ok(id, { content: [{ type: 'text', text: `모르는 도구입니다: ${name}` }], isError: true });
            try {
                const a = params?.arguments ?? {};
                // 프로젝트를 고치는 도구는 "앱이 열고 있는지" 먼저 확인한다.
                // 한 곳에서 걸러야 도구가 늘어도 빠뜨리지 않는다.
                let warn = null;
                if (MUTATING_TOOLS.has(name) && a.path)
                    warn = guardProjectOpen(path.resolve(String(a.path)), a.force);
                const text = tool.run(a);
                return ok(id, { content: [{ type: 'text', text: warn ? `${warn}\n${text}` : String(text) }] });
            } catch (e) {
                // 도구 실패는 프로토콜 오류가 아니라 결과에 담아 돌려준다 —
                // 그래야 모델이 메시지를 읽고 스스로 고쳐 다시 시도할 수 있다.
                log(`[tool:${name}] ${e?.stack ?? e}`);
                return ok(id, { content: [{ type: 'text', text: `오류: ${e?.message ?? e}` }], isError: true });
            }
        }

        // 우리가 제공하지 않는 기능이라도 빈 목록으로 답해 클라이언트 로그를 조용하게 둔다
        case 'resources/list': return ok(id, { resources: [] });
        case 'resources/templates/list': return ok(id, { resourceTemplates: [] });
        case 'prompts/list': return ok(id, { prompts: [] });

        default:
            if (!isNotification) fail(id, -32601, `지원하지 않는 메서드: ${method}`);
    }
}

let buf = '';
process.stdin.setEncoding('utf8');
process.stdin.on('data', (chunk) => {
    buf += chunk;
    let nl;
    while ((nl = buf.indexOf('\n')) >= 0) {
        const line = buf.slice(0, nl).trim();
        buf = buf.slice(nl + 1);
        if (!line) continue;
        let msg;
        try {
            msg = JSON.parse(line);
        } catch {
            fail(null, -32700, 'JSON 파싱 실패');
            continue;
        }
        try {
            handle(msg);
        } catch (e) {
            log(`[handle] ${e?.stack ?? e}`);
            if (msg?.id !== undefined && msg?.id !== null) fail(msg.id, -32603, String(e?.message ?? e));
        }
    }
});
process.stdin.on('end', () => process.exit(0));
log(`${SERVER_NAME} MCP 서버 ${SERVER_VERSION} 준비됨 (도구 ${Object.keys(TOOLS).length}개)`);

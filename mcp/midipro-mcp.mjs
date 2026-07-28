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
         mkdirSync } from 'node:fs';
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
            const ad = path.join(localAppData(), 'MidiPro');
            const readLines = (f) => existsSync(f) ? readFileSync(f, 'utf8').split(/\r?\n/).filter(Boolean) : [];

            const plugins = readLines(path.join(ad, 'plugincache.ini')).slice(1).map((L) => {
                const m = /^(\d+)\s+(\d+)\s+(\d)\s+(\d)\s+(.+)$/.exec(L);
                if (!m) return null;
                return {
                    path: m[5], name: path.basename(m[5], '.vst3'),
                    instrument: m[3] === '1', effect: m[4] === '1',
                };
            }).filter(Boolean);

            return JSON.stringify({
                exe: exe ?? '(찾지 못함 — 환경변수 MIDIPRO_EXE로 지정하세요)',
                running: isMidiProRunning(),
                userDataDir: ad,
                crashLog: existsSync(path.join(ad, 'crash.log')) ? path.join(ad, 'crash.log') : null,
                plugins,
                recentProjects: readLines(path.join(ad, 'recent.txt')),
            }, null, 2);
        },
    },
};

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
                const text = tool.run(params?.arguments ?? {});
                return ok(id, { content: [{ type: 'text', text: String(text) }] });
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

// =============================================================
// MidiPro - pdf/PdfTab.cpp
// 조판 PDF -> ASCII 타브 복원. 설계는 PdfTab.h 참고.
//
// 처리 흐름:
//   파일 로드 -> "N G obj" 전수 스캔(깨진 xref에도 강함) -> 트레일러/암호화 정보
//   -> RC4 키 유도(빈 비밀번호) -> 페이지 트리 -> 콘텐츠 스트림 해석
//   -> 가로줄 6개 그룹 = 타브 보표 -> 숫자 글리프를 현에 배정 -> ASCII 생성
// =============================================================

#include "pdf/PdfTab.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "miniz.h"

namespace midipro::pdf {
namespace {

// ───────────────────────── MD5 ─────────────────────────
struct Md5 {
    uint32_t a = 0x67452301, b = 0xefcdab89, c = 0x98badcfe, d = 0x10325476;
    uint64_t len = 0;
    uint8_t buf[64];
    std::size_t bufLen = 0;

    static uint32_t rol(uint32_t x, int s) { return (x << s) | (x >> (32 - s)); }

    void block(const uint8_t* p) {
        static const uint32_t K[64] = {
            0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a,
            0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
            0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340,
            0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
            0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8,
            0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
            0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa,
            0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
            0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92,
            0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
            0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
        static const int S[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                                  5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                                  4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                                  6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
        uint32_t M[16];
        for (int i = 0; i < 16; ++i)
            M[i] = (uint32_t)p[i * 4] | ((uint32_t)p[i * 4 + 1] << 8) |
                   ((uint32_t)p[i * 4 + 2] << 16) | ((uint32_t)p[i * 4 + 3] << 24);
        uint32_t A = a, B = b, C = c, D = d;
        for (int i = 0; i < 64; ++i) {
            uint32_t F;
            int g;
            if (i < 16) {
                F = (B & C) | (~B & D);
                g = i;
            } else if (i < 32) {
                F = (D & B) | (~D & C);
                g = (5 * i + 1) & 15;
            } else if (i < 48) {
                F = B ^ C ^ D;
                g = (3 * i + 5) & 15;
            } else {
                F = C ^ (B | ~D);
                g = (7 * i) & 15;
            }
            F += A + K[i] + M[g];
            A = D;
            D = C;
            C = B;
            B += rol(F, S[i]);
        }
        a += A;
        b += B;
        c += C;
        d += D;
    }

    void update(const void* data, std::size_t n) {
        const uint8_t* p = (const uint8_t*)data;
        len += n;
        while (n) {
            const std::size_t take = std::min(n, 64 - bufLen);
            std::memcpy(buf + bufLen, p, take);
            bufLen += take;
            p += take;
            n -= take;
            if (bufLen == 64) {
                block(buf);
                bufLen = 0;
            }
        }
    }

    void final(uint8_t out[16]) {
        const uint64_t bits = len * 8;
        const uint8_t pad = 0x80;
        update(&pad, 1);
        const uint8_t zero = 0;
        while (bufLen != 56) update(&zero, 1);
        uint8_t lb[8];
        for (int i = 0; i < 8; ++i) lb[i] = (uint8_t)(bits >> (8 * i));
        std::memcpy(buf + bufLen, lb, 8);
        block(buf);
        bufLen = 0;
        const uint32_t v[4] = {a, b, c, d};
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j) out[i * 4 + j] = (uint8_t)(v[i] >> (8 * j));
    }
};

std::string md5(const std::string& in) {
    Md5 h;
    h.update(in.data(), in.size());
    uint8_t out[16];
    h.final(out);
    return std::string((char*)out, 16);
}

// ───────────────────────── RC4 ─────────────────────────
std::string rc4(const std::string& key, const std::string& data) {
    uint8_t S[256];
    for (int i = 0; i < 256; ++i) S[i] = (uint8_t)i;
    int j = 0;
    for (int i = 0; i < 256; ++i) {
        j = (j + S[i] + (uint8_t)key[i % key.size()]) & 255;
        std::swap(S[i], S[j]);
    }
    std::string out(data.size(), '\0');
    int i = 0;
    j = 0;
    for (std::size_t k = 0; k < data.size(); ++k) {
        i = (i + 1) & 255;
        j = (j + S[i]) & 255;
        std::swap(S[i], S[j]);
        out[k] = (char)(data[k] ^ S[(S[i] + S[j]) & 255]);
    }
    return out;
}

// ───────────────────────── Flate ─────────────────────────
bool inflateZlib(const std::string& in, std::string& out) {
    if (in.empty()) return false;
    std::size_t outLen = 0;
    void* p = tinfl_decompress_mem_to_heap(in.data(), in.size(), &outLen,
                                           TINFL_FLAG_PARSE_ZLIB_HEADER);
    if (!p) {
        // zlib 헤더 없이 raw deflate인 경우도 있다
        p = tinfl_decompress_mem_to_heap(in.data(), in.size(), &outLen, 0);
        if (!p) return false;
    }
    out.assign((const char*)p, outLen);
    mz_free(p);
    return true;
}

// ───────────────────────── PDF 객체 ─────────────────────────
struct Obj;
using ObjPtr = std::shared_ptr<Obj>;

struct Obj {
    enum Type { Null, Bool, Num, Str, Name, Arr, Dict, Ref, Keyword } type = Null;
    bool bval = false;
    double num = 0;
    std::string str;                  // Str / Name / Keyword
    std::vector<ObjPtr> arr;          // Arr
    std::map<std::string, ObjPtr> dict; // Dict
    std::string stream;               // Dict에 딸린 원본 스트림 (미복호/미압축)
    bool hasStream = false;
    int refNum = 0, refGen = 0;       // Ref

    bool isNum() const { return type == Num; }
    bool isDict() const { return type == Dict; }
};

ObjPtr mk(Obj::Type t) {
    auto o = std::make_shared<Obj>();
    o->type = t;
    return o;
}

bool isWs(int c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == 0;
}
bool isDelim(int c) {
    return c == '(' || c == ')' || c == '<' || c == '>' || c == '[' || c == ']' ||
           c == '{' || c == '}' || c == '/' || c == '%';
}

struct Lexer {
    const uint8_t* d;
    std::size_t n, p = 0;

    Lexer(const uint8_t* data, std::size_t len, std::size_t pos = 0) : d(data), n(len), p(pos) {}

    void skipWs() {
        while (p < n) {
            if (isWs(d[p])) {
                ++p;
            } else if (d[p] == '%') {
                while (p < n && d[p] != '\n' && d[p] != '\r') ++p;
            } else {
                break;
            }
        }
    }

    static int hexVal(int c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

    ObjPtr parse() {
        skipWs();
        if (p >= n) return nullptr;
        const uint8_t c = d[p];

        if (c == '/') { // 이름
            ++p;
            auto o = mk(Obj::Name);
            while (p < n && !isWs(d[p]) && !isDelim(d[p])) {
                if (d[p] == '#' && p + 2 < n) {
                    const int h = hexVal(d[p + 1]), l = hexVal(d[p + 2]);
                    if (h >= 0 && l >= 0) {
                        o->str += (char)(h * 16 + l);
                        p += 3;
                        continue;
                    }
                }
                o->str += (char)d[p++];
            }
            return o;
        }
        if (c == '(') { // 리터럴 문자열
            ++p;
            auto o = mk(Obj::Str);
            int depth = 1;
            while (p < n) {
                const uint8_t ch = d[p++];
                if (ch == '\\') {
                    if (p >= n) break;
                    const uint8_t e = d[p++];
                    switch (e) {
                    case 'n': o->str += '\n'; break;
                    case 'r': o->str += '\r'; break;
                    case 't': o->str += '\t'; break;
                    case 'b': o->str += '\b'; break;
                    case 'f': o->str += '\f'; break;
                    case '\r':
                        if (p < n && d[p] == '\n') ++p;
                        break;
                    case '\n': break;
                    default:
                        if (e >= '0' && e <= '7') {
                            int v = e - '0';
                            for (int k = 0; k < 2 && p < n && d[p] >= '0' && d[p] <= '7'; ++k)
                                v = v * 8 + (d[p++] - '0');
                            o->str += (char)(v & 255);
                        } else {
                            o->str += (char)e;
                        }
                    }
                } else if (ch == '(') {
                    ++depth;
                    o->str += '(';
                } else if (ch == ')') {
                    if (--depth == 0) break;
                    o->str += ')';
                } else {
                    o->str += (char)ch;
                }
            }
            return o;
        }
        if (c == '<') {
            if (p + 1 < n && d[p + 1] == '<') { // 딕셔너리
                p += 2;
                auto o = mk(Obj::Dict);
                while (true) {
                    skipWs();
                    if (p + 1 < n && d[p] == '>' && d[p + 1] == '>') {
                        p += 2;
                        break;
                    }
                    if (p >= n) break;
                    auto key = parse();
                    if (!key) break;
                    if (key->type != Obj::Name) continue; // 이상한 토큰은 건너뜀
                    auto val = parseValue();
                    if (!val) break;
                    o->dict[key->str] = val;
                }
                // 스트림?
                const std::size_t save = p;
                skipWs();
                if (p + 6 <= n && std::memcmp(d + p, "stream", 6) == 0) {
                    p += 6;
                    if (p < n && d[p] == '\r') ++p;
                    if (p < n && d[p] == '\n') ++p;
                    const std::size_t start = p;
                    // /Length 우선, 없거나 틀리면 endstream 검색
                    std::size_t len = 0;
                    bool haveLen = false;
                    auto it = o->dict.find("Length");
                    if (it != o->dict.end() && it->second->isNum()) {
                        len = (std::size_t)it->second->num;
                        haveLen = true;
                    }
                    bool ok = false;
                    if (haveLen && start + len <= n) {
                        Lexer probe(d, n, start + len);
                        probe.skipWs();
                        if (probe.p + 9 <= n && std::memcmp(d + probe.p, "endstream", 9) == 0)
                            ok = true;
                    }
                    if (!ok) {
                        const uint8_t* found = nullptr;
                        for (std::size_t q = start; q + 9 <= n; ++q)
                            if (d[q] == 'e' && std::memcmp(d + q, "endstream", 9) == 0) {
                                found = d + q;
                                break;
                            }
                        if (!found) return o;
                        len = (std::size_t)(found - (d + start));
                        while (len > 0 && (d[start + len - 1] == '\n' || d[start + len - 1] == '\r'))
                            --len;
                    }
                    o->stream.assign((const char*)d + start, len);
                    o->hasStream = true;
                    p = start + len;
                    // endstream 지나가기
                    for (std::size_t q = p; q + 9 <= n && q < p + 40; ++q)
                        if (std::memcmp(d + q, "endstream", 9) == 0) {
                            p = q + 9;
                            break;
                        }
                } else {
                    p = save;
                }
                return o;
            }
            // 16진 문자열
            ++p;
            auto o = mk(Obj::Str);
            int hi = -1;
            while (p < n && d[p] != '>') {
                const int v = hexVal(d[p++]);
                if (v < 0) continue;
                if (hi < 0) {
                    hi = v;
                } else {
                    o->str += (char)(hi * 16 + v);
                    hi = -1;
                }
            }
            if (hi >= 0) o->str += (char)(hi * 16);
            if (p < n) ++p; // '>'
            return o;
        }
        if (c == '[') {
            ++p;
            auto o = mk(Obj::Arr);
            while (true) {
                skipWs();
                if (p < n && d[p] == ']') {
                    ++p;
                    break;
                }
                if (p >= n) break;
                auto v = parseValue();
                if (!v) break;
                o->arr.push_back(v);
            }
            return o;
        }
        if (c == ']' || c == '>' || c == '}') { // 예상 밖 종료 토큰
            ++p;
            auto o = mk(Obj::Keyword);
            o->str = std::string(1, (char)c);
            return o;
        }
        if (c == '{') {
            ++p;
            auto o = mk(Obj::Keyword);
            o->str = "{";
            return o;
        }
        if ((c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.') {
            std::string s;
            while (p < n && !isWs(d[p]) && !isDelim(d[p])) s += (char)d[p++];
            auto o = mk(Obj::Num);
            o->num = std::atof(s.c_str());
            return o;
        }
        // 키워드 (true/false/null/R/obj/BT/Tj/...)
        {
            std::string s;
            while (p < n && !isWs(d[p]) && !isDelim(d[p])) s += (char)d[p++];
            if (s.empty()) { // 알 수 없는 구분자 — 한 글자 소비
                ++p;
                return parse();
            }
            if (s == "true" || s == "false") {
                auto o = mk(Obj::Bool);
                o->bval = (s == "true");
                return o;
            }
            if (s == "null") return mk(Obj::Null);
            auto o = mk(Obj::Keyword);
            o->str = s;
            return o;
        }
    }

    // 값 위치에서 파싱 — "3 0 R" 참조를 합쳐준다
    ObjPtr parseValue() {
        auto v = parse();
        if (!v || !v->isNum()) return v;
        const std::size_t save = p;
        auto second = parse();
        if (second && second->isNum()) {
            const std::size_t save2 = p;
            auto third = parse();
            if (third && third->type == Obj::Keyword && third->str == "R") {
                auto r = mk(Obj::Ref);
                r->refNum = (int)v->num;
                r->refGen = (int)second->num;
                return r;
            }
            p = save2;
        }
        p = save;
        return v;
    }
};

// ───────────────────────── 문서 ─────────────────────────
struct Doc {
    std::string bytes;
    std::map<int, std::size_t> offsets;   // 객체번호 -> 파일 오프셋 ("N G obj" 뒤)
    std::map<int, ObjPtr> cache;
    std::map<int, ObjPtr> embedded;       // ObjStm 안에서 꺼낸 객체
    ObjPtr trailer;
    bool encrypted = false;
    std::string fileKey;
    int keyLen = 5;
    int encV = 0, encR = 0;

    ObjPtr get(int num) {
        auto c = cache.find(num);
        if (c != cache.end()) return c->second;
        auto e = embedded.find(num);
        if (e != embedded.end()) return e->second;
        auto it = offsets.find(num);
        if (it == offsets.end()) return nullptr;
        Lexer lx((const uint8_t*)bytes.data(), bytes.size(), it->second);
        auto o = lx.parseValue();
        cache[num] = o;
        return o;
    }

    ObjPtr res(const ObjPtr& o) { // 참조 해제
        if (!o) return nullptr;
        if (o->type != Obj::Ref) return o;
        return get(o->refNum);
    }

    ObjPtr dget(const ObjPtr& dictObj, const std::string& key) {
        if (!dictObj || !dictObj->isDict()) return nullptr;
        auto it = dictObj->dict.find(key);
        if (it == dictObj->dict.end()) return nullptr;
        return res(it->second);
    }

    std::string objKey(int num, int gen) const {
        std::string k = fileKey;
        k += (char)(num & 255);
        k += (char)((num >> 8) & 255);
        k += (char)((num >> 16) & 255);
        k += (char)(gen & 255);
        k += (char)((gen >> 8) & 255);
        std::string h = md5(k);
        const std::size_t len = std::min<std::size_t>(fileKey.size() + 5, 16);
        return h.substr(0, len);
    }

    // 스트림 데이터 얻기 (복호 + 압축 해제)
    std::string streamData(const ObjPtr& o, int objNum) {
        if (!o || !o->hasStream) return {};
        std::string raw = o->stream;
        if (encrypted && objNum > 0) raw = rc4(objKey(objNum, 0), raw);

        // 필터 적용
        std::vector<std::string> filters;
        auto f = dget(o, "Filter");
        if (f) {
            if (f->type == Obj::Name) {
                filters.push_back(f->str);
            } else if (f->type == Obj::Arr) {
                for (auto& x : f->arr) {
                    auto rx = res(x);
                    if (rx && rx->type == Obj::Name) filters.push_back(rx->str);
                }
            }
        }
        for (const auto& name : filters) {
            if (name == "FlateDecode") {
                std::string out;
                if (!inflateZlib(raw, out)) return {};
                raw = out;
            } else {
                return {}; // 미지원 필터 (DCTDecode 등 = 이미지)
            }
        }
        return raw;
    }
};

const uint8_t kPad[32] = {0x28, 0xBF, 0x4E, 0x5E, 0x4E, 0x75, 0x8A, 0x41, 0x64, 0x00, 0x4E,
                          0x56, 0xFF, 0xFA, 0x01, 0x08, 0x2E, 0x2E, 0x00, 0xB6, 0xD0, 0x68,
                          0x3E, 0x80, 0x2F, 0x0C, 0xA9, 0xFE, 0x64, 0x53, 0x69, 0x7A};

// "N G obj" 전수 스캔
void scanObjects(Doc& doc) {
    const std::string& b = doc.bytes;
    for (std::size_t i = 0; i + 3 < b.size(); ++i) {
        if (b[i] != 'o' || b[i + 1] != 'b' || b[i + 2] != 'j') continue;
        if (i + 3 < b.size() && !isWs((uint8_t)b[i + 3]) && !isDelim((uint8_t)b[i + 3])) continue;
        // 뒤로 걸어가며 "N G " 읽기
        std::size_t j = i;
        while (j > 0 && isWs((uint8_t)b[j - 1])) --j;
        const std::size_t genEnd = j;
        while (j > 0 && std::isdigit((uint8_t)b[j - 1])) --j;
        if (j == genEnd) continue;
        const std::size_t genStart = j;
        while (j > 0 && isWs((uint8_t)b[j - 1])) --j;
        if (j == genStart) continue;
        const std::size_t numEnd = j;
        while (j > 0 && std::isdigit((uint8_t)b[j - 1])) --j;
        if (j == numEnd) continue;
        const int num = std::atoi(b.substr(j, numEnd - j).c_str());
        if (num <= 0) continue;
        doc.offsets[num] = i + 3; // 나중 정의가 이긴다 (증분 업데이트)
    }
}

// 트레일러: classic trailer 또는 XRef 스트림 딕셔너리
void findTrailer(Doc& doc) {
    const std::string& b = doc.bytes;
    // classic
    std::size_t pos = b.rfind("trailer");
    while (pos != std::string::npos) {
        Lexer lx((const uint8_t*)b.data(), b.size(), pos + 7);
        auto t = lx.parse();
        if (t && t->isDict() && t->dict.count("Root")) {
            doc.trailer = t;
            return;
        }
        pos = pos ? b.rfind("trailer", pos - 1) : std::string::npos;
    }
    // XRef 스트림
    for (auto& kv : doc.offsets) {
        auto o = doc.get(kv.first);
        if (!o || !o->isDict()) continue;
        auto t = o->dict.find("Type");
        if (t == o->dict.end() || t->second->type != Obj::Name || t->second->str != "XRef")
            continue;
        if (o->dict.count("Root")) {
            doc.trailer = o;
            return;
        }
    }
}

bool setupDecryption(Doc& doc, std::string& err) {
    if (!doc.trailer) return true;
    auto encRef = doc.trailer->dict.find("Encrypt");
    if (encRef == doc.trailer->dict.end()) return true;
    auto enc = doc.res(encRef->second);
    if (!enc || !enc->isDict()) return true;

    auto filt = doc.dget(enc, "Filter");
    if (filt && filt->type == Obj::Name && filt->str != "Standard") {
        err = "지원하지 않는 암호화 방식(" + filt->str + ")입니다.";
        return false;
    }
    auto vO = doc.dget(enc, "V");
    auto rO = doc.dget(enc, "R");
    doc.encV = vO && vO->isNum() ? (int)vO->num : 0;
    doc.encR = rO && rO->isNum() ? (int)rO->num : 2;
    if (doc.encV >= 4) {
        err = "AES로 암호화된 PDF는 지원하지 않습니다. (인쇄 허용본으로 다시 받아보세요)";
        return false;
    }
    auto oO = doc.dget(enc, "O");
    auto pO = doc.dget(enc, "P");
    auto lenO = doc.dget(enc, "Length");
    if (!oO || oO->type != Obj::Str || !pO || !pO->isNum()) {
        err = "암호화 정보를 읽지 못했습니다.";
        return false;
    }
    int bits = lenO && lenO->isNum() ? (int)lenO->num : 40;
    if (doc.encR == 2) bits = 40;
    const int nKey = std::max(5, std::min(16, bits / 8));

    std::string idFirst;
    auto idIt = doc.trailer->dict.find("ID");
    if (idIt != doc.trailer->dict.end()) {
        auto id = doc.res(idIt->second);
        if (id && id->type == Obj::Arr && !id->arr.empty()) {
            auto f = doc.res(id->arr[0]);
            if (f && f->type == Obj::Str) idFirst = f->str;
        }
    }

    // 알고리즘 2 (빈 사용자 비밀번호)
    std::string in((const char*)kPad, 32);
    in += oO->str.substr(0, 32);
    const int32_t P = (int32_t)pO->num;
    const uint32_t up = (uint32_t)P;
    for (int i = 0; i < 4; ++i) in += (char)((up >> (8 * i)) & 255);
    in += idFirst;
    auto encMeta = doc.dget(enc, "EncryptMetadata");
    if (doc.encR >= 4 && encMeta && encMeta->type == Obj::Bool && !encMeta->bval)
        in += "\xFF\xFF\xFF\xFF";

    std::string key = md5(in);
    if (doc.encR >= 3)
        for (int i = 0; i < 50; ++i) key = md5(key.substr(0, (std::size_t)nKey));
    doc.fileKey = key.substr(0, (std::size_t)nKey);
    doc.keyLen = nKey;
    doc.encrypted = true;
    return true;
}

// ObjStm 안의 객체 펼치기
void expandObjectStreams(Doc& doc) {
    std::vector<int> nums;
    for (auto& kv : doc.offsets) nums.push_back(kv.first);
    for (int num : nums) {
        auto o = doc.get(num);
        if (!o || !o->isDict() || !o->hasStream) continue;
        auto t = o->dict.find("Type");
        if (t == o->dict.end() || t->second->type != Obj::Name || t->second->str != "ObjStm")
            continue;
        const std::string data = doc.streamData(o, num);
        if (data.empty()) continue;
        auto nO = doc.dget(o, "N");
        auto firstO = doc.dget(o, "First");
        if (!nO || !firstO) continue;
        const int cnt = (int)nO->num;
        const std::size_t first = (std::size_t)firstO->num;
        Lexer hdr((const uint8_t*)data.data(), data.size(), 0);
        std::vector<std::pair<int, std::size_t>> ents;
        for (int i = 0; i < cnt; ++i) {
            auto a = hdr.parse();
            auto b = hdr.parse();
            if (!a || !b || !a->isNum() || !b->isNum()) break;
            ents.push_back({(int)a->num, (std::size_t)b->num});
        }
        for (auto& e : ents) {
            if (doc.offsets.count(e.first)) continue; // 파일 본문 정의 우선
            if (first + e.second >= data.size()) continue;
            Lexer lx((const uint8_t*)data.data(), data.size(), first + e.second);
            doc.embedded[e.first] = lx.parseValue();
        }
    }
}

// ───────────────────────── 콘텐츠 해석 ─────────────────────────
struct Mat {
    double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
};
Mat mul(const Mat& m, const Mat& n) { // m 다음 n
    Mat r;
    r.a = m.a * n.a + m.b * n.c;
    r.b = m.a * n.b + m.b * n.d;
    r.c = m.c * n.a + m.d * n.c;
    r.d = m.c * n.b + m.d * n.d;
    r.e = m.e * n.a + m.f * n.c + n.e;
    r.f = m.e * n.b + m.f * n.d + n.f;
    return r;
}
void apply(const Mat& m, double x, double y, double& ox, double& oy) {
    ox = m.a * x + m.c * y + m.e;
    oy = m.b * x + m.d * y + m.f;
}

struct HLine {
    double y, x0, x1;
};
struct VLine {        // 기둥(stem) / 마디선 후보
    double x, y0, y1; // y0 = 위, y1 = 아래 (top-down)
    double w = 1.0;   // 선 굵기 — 도돌이표의 굵은 세로줄을 가려내는 열쇠
};
struct Rect { // 빔 후보 (채운 사각형)
    double x0, y0, x1, y1; // top-down
};
struct Arc { // 이음줄(타이/슬러) 곡선 또는 슬라이드 사선 — 두 음을 잇는다
    double x0, y0, x1, y1; // top-down
};
struct TextItem {
    double x, y;        // y = 글자 중심 (top-down)
    double size = 0;    // 글자 크기 (pt) — 프렛 숫자와 박자표 숫자를 가르는 열쇠
    int code = -1;      // 폰트 안의 원본 글리프 번호 (마디 반복 기호를 알아보는 데 쓴다)
    std::string text;
};

struct FontInfo {
    bool twoByte = false;
    std::map<int, int> toUni; // 코드 -> 유니코드
    bool hasMap = false;
};

// ToUnicode CMap에서 bfchar/bfrange 뽑기
void parseCMap(const std::string& cmap, FontInfo& fi) {
    auto hexAt = [&](std::size_t& i) -> std::string {
        while (i < cmap.size() && cmap[i] != '<' && cmap[i] != '[' && cmap[i] != 'e') ++i;
        if (i >= cmap.size() || cmap[i] != '<') return {};
        ++i;
        std::string h;
        while (i < cmap.size() && cmap[i] != '>') h += cmap[i++];
        if (i < cmap.size()) ++i;
        return h;
    };
    auto hexToInt = [](const std::string& h) {
        int v = 0;
        for (char ch : h) {
            const int d = Lexer::hexVal((uint8_t)ch);
            if (d < 0) continue;
            v = v * 16 + d;
        }
        return v;
    };
    auto firstUnit = [](const std::string& h) { // UTF-16BE 첫 코드유닛
        return h.size() >= 4 ? h.substr(0, 4) : h;
    };

    std::size_t pos = 0;
    while ((pos = cmap.find("beginbfchar", pos)) != std::string::npos) {
        const std::size_t end = cmap.find("endbfchar", pos);
        if (end == std::string::npos) break;
        std::size_t i = pos + 11;
        while (i < end) {
            const std::string src = hexAt(i);
            if (src.empty() || i >= end) break;
            const std::string dst = hexAt(i);
            if (dst.empty()) break;
            fi.toUni[hexToInt(src)] = hexToInt(firstUnit(dst));
        }
        pos = end + 9;
    }
    pos = 0;
    while ((pos = cmap.find("beginbfrange", pos)) != std::string::npos) {
        const std::size_t end = cmap.find("endbfrange", pos);
        if (end == std::string::npos) break;
        std::size_t i = pos + 12;
        while (i < end) {
            // <lo> <hi> <dst>  또는  <lo> <hi> [<d1> <d2> ...]
            while (i < end && cmap[i] != '<' && cmap[i] != '[') ++i;
            if (i >= end || cmap[i] != '<') break;
            const std::string lo = hexAt(i);
            if (lo.empty()) break;
            while (i < end && cmap[i] != '<' && cmap[i] != '[') ++i;
            if (i >= end || cmap[i] != '<') break;
            const std::string hi = hexAt(i);
            if (hi.empty()) break;
            while (i < end && cmap[i] != '<' && cmap[i] != '[') ++i;
            if (i >= end) break;
            const int a = hexToInt(lo), b = hexToInt(hi);
            if (cmap[i] == '[') {
                ++i;
                int code = a;
                while (i < end && cmap[i] != ']') {
                    const std::string dst = hexAt(i);
                    if (dst.empty()) break;
                    if (code <= b) fi.toUni[code++] = hexToInt(firstUnit(dst));
                    while (i < end && (cmap[i] == ' ' || cmap[i] == '\n' || cmap[i] == '\r')) ++i;
                }
                if (i < end && cmap[i] == ']') ++i;
            } else {
                const std::string dst = hexAt(i);
                if (dst.empty()) break;
                const int base = hexToInt(firstUnit(dst));
                for (int code = a; code <= b && code - a < 65536; ++code)
                    fi.toUni[code] = base + (code - a);
            }
        }
        pos = end + 10;
    }
    fi.hasMap = !fi.toUni.empty();
}

void buildFonts(Doc& doc, const ObjPtr& resources, std::map<std::string, FontInfo>& fonts) {
    auto fdict = doc.dget(resources, "Font");
    if (!fdict || !fdict->isDict()) return;
    for (auto& kv : fdict->dict) {
        auto fObjRef = kv.second;
        auto fObj = doc.res(fObjRef);
        if (!fObj || !fObj->isDict()) continue;
        FontInfo fi;
        auto sub = doc.dget(fObj, "Subtype");
        fi.twoByte = sub && sub->type == Obj::Name && sub->str == "Type0";
        auto tuRef = fObj->dict.find("ToUnicode");
        if (tuRef != fObj->dict.end()) {
            const int tuNum = tuRef->second->type == Obj::Ref ? tuRef->second->refNum : 0;
            auto tu = doc.res(tuRef->second);
            if (tu && tu->hasStream) {
                const std::string cm = doc.streamData(tu, tuNum);
                if (!cm.empty()) parseCMap(cm, fi);
            }
        }
        fonts[kv.first] = fi;
    }
}

std::string decodeShow(const std::string& raw, const FontInfo* fi) {
    std::string out;
    if (fi && fi->twoByte) {
        for (std::size_t i = 0; i + 1 < raw.size(); i += 2) {
            const int code = ((uint8_t)raw[i] << 8) | (uint8_t)raw[i + 1];
            int uni = code;
            if (fi->hasMap) {
                auto it = fi->toUni.find(code);
                if (it == fi->toUni.end()) continue;
                uni = it->second;
            } else {
                continue; // 매핑 없으면 해독 불가
            }
            if (uni >= 32 && uni < 127) out += (char)uni;
        }
        return out;
    }
    for (char ch : raw) {
        int uni = (uint8_t)ch;
        if (fi && fi->hasMap) {
            auto it = fi->toUni.find(uni);
            if (it != fi->toUni.end()) uni = it->second;
        }
        if (uni >= 32 && uni < 127) out += (char)uni;
    }
    return out;
}

struct PageContent {
    std::vector<HLine> lines;
    std::vector<VLine> vlines;
    std::vector<Rect> rects;
    std::vector<TextItem> digits;
    std::vector<TextItem> dots;   // 부점 (음악 폰트의 '.')
    std::vector<TextItem> glyphs; // 모든 글리프 (마디 반복 기호를 찾는 데 쓴다)
    std::vector<Arc> arcs;        // 이음줄/슬라이드 (타이·해머링·슬라이드 표현용)
    double height = 842;
};

void runContent(Doc& doc, const std::string& content, const std::map<std::string, FontInfo>& fonts,
                double pageH, PageContent& out) {
    Lexer lx((const uint8_t*)content.data(), content.size(), 0);
    std::vector<ObjPtr> st;
    std::vector<Mat> ctmStack;
    std::vector<double> lwStack;
    std::vector<bool> fwStack;
    Mat ctm, tm, tlm;
    double leading = 0, fsize = 1, lineW = 1.0;
    // 채우기 색이 흰색인가: 조판 프로그램은 프렛 숫자 뒤에 흰 사각형을 깔아
    // 보표 줄을 가린다. 이걸 빔으로 오인하면 리듬이 통째로 틀어진다.
    bool fillWhite = false;
    const FontInfo* font = nullptr;

    // 경로 누적
    std::vector<HLine> pending;
    std::vector<VLine> pendingV;
    std::vector<Arc> pendingA;
    double curX = 0, curY = 0, startX = 0, startY = 0;
    bool hasCur = false;
    int curveCnt = 0; // 이 경로의 곡선(c/v/y) 수 — 있으면 이음줄 후보
    // 경로의 좌/우 극단점 (이음줄은 채운 닫힌 곡선이라 시작=끝 — 극단점이 양 팁이다)
    double exMinX = 0, exMinY = 0, exMaxX = 0, exMaxY = 0;
    bool exValid = false;
    auto exTrack = [&](double x, double y) {
        if (!exValid || x < exMinX) {
            exMinX = x;
            exMinY = y;
        }
        if (!exValid || x > exMaxX) {
            exMaxX = x;
            exMaxY = y;
        }
        exValid = true;
    };

    auto num = [&](int i) -> double {
        const int k = (int)st.size() - i;
        return (k >= 0 && k < (int)st.size() && st[k]->isNum()) ? st[k]->num : 0.0;
    };

    auto addSeg = [&](double x0, double y0, double x1, double y1) {
        double dx0, dy0, dx1, dy1;
        apply(ctm, x0, y0, dx0, dy0);
        apply(ctm, x1, y1, dx1, dy1);
        if (std::fabs(dy1 - dy0) < 0.6 && std::fabs(dx1 - dx0) > 60.0) {
            HLine h;
            h.y = pageH - (dy0 + dy1) * 0.5; // top-down
            h.x0 = std::min(dx0, dx1);
            h.x1 = std::max(dx0, dx1);
            pending.push_back(h);
        } else if (std::fabs(dx1 - dx0) < 0.8 && std::fabs(dy1 - dy0) > 3.0) {
            // 세로선 = 기둥(stem) 또는 마디선 (굵으면 도돌이표의 굵은 줄)
            VLine v;
            v.x = (dx0 + dx1) * 0.5;
            v.y0 = pageH - std::max(dy0, dy1);
            v.y1 = pageH - std::min(dy0, dy1);
            v.w = lineW * std::sqrt(std::fabs(ctm.a * ctm.d - ctm.b * ctm.c));
            pendingV.push_back(v);
        } else if (std::fabs(dx1 - dx0) > 3.0 && std::fabs(dx1 - dx0) < 45.0 &&
                   std::fabs(dy1 - dy0) > 1.5 && std::fabs(dy1 - dy0) < 16.0) {
            // 짧은 사선 = 슬라이드 연결선 (두 프렛을 잇는다)
            Arc a;
            a.x0 = std::min(dx0, dx1);
            a.x1 = std::max(dx0, dx1);
            a.y0 = pageH - (dx0 < dx1 ? dy0 : dy1);
            a.y1 = pageH - (dx0 < dx1 ? dy1 : dy0);
            pendingA.push_back(a);
        }
    };

    while (true) {
        auto o = lx.parse();
        if (!o) break;
        if (o->type != Obj::Keyword) {
            st.push_back(o);
            if (st.size() > 32) st.erase(st.begin());
            continue;
        }
        const std::string& op = o->str;

        if (op == "q") {
            ctmStack.push_back(ctm);
            lwStack.push_back(lineW);
            fwStack.push_back(fillWhite);
        } else if (op == "Q") {
            if (!ctmStack.empty()) {
                ctm = ctmStack.back();
                ctmStack.pop_back();
            }
            if (!lwStack.empty()) {
                lineW = lwStack.back();
                lwStack.pop_back();
            }
            if (!fwStack.empty()) {
                fillWhite = fwStack.back();
                fwStack.pop_back();
            }
        } else if (op == "w") {
            lineW = num(1);
        } else if (op == "g" || op == "rg" || op == "k" || op == "sc" || op == "scn") {
            // 채우기 색 지정. 뒤쪽의 숫자 피연산자 개수로 회색/RGB/CMYK를 가른다.
            int n = 0;
            while (n < (int)st.size() && st[st.size() - 1 - (std::size_t)n]->isNum()) ++n;
            if (n >= 4 && op != "g" && op != "rg") {
                fillWhite = num(1) < 0.1 && num(2) < 0.1 && num(3) < 0.1 && num(4) < 0.1;
            } else if (n >= 3 && op != "g") {
                fillWhite = num(1) > 0.9 && num(2) > 0.9 && num(3) > 0.9;
            } else if (n >= 1) {
                fillWhite = num(1) > 0.9;
            }
        } else if (op == "cm") {
            Mat m;
            m.a = num(6); m.b = num(5); m.c = num(4);
            m.d = num(3); m.e = num(2); m.f = num(1);
            ctm = mul(m, ctm);
        } else if (op == "m") {
            curX = num(2); curY = num(1);
            startX = curX; startY = curY;
            hasCur = true;
            exTrack(curX, curY);
        } else if (op == "l") {
            const double x = num(2), y = num(1);
            if (hasCur) addSeg(curX, curY, x, y);
            curX = x; curY = y;
            hasCur = true;
            exTrack(curX, curY);
        } else if (op == "c" || op == "v" || op == "y") {
            curX = num(2); curY = num(1);
            hasCur = true;
            ++curveCnt;
            exTrack(curX, curY);
        } else if (op == "h") {
            if (hasCur) addSeg(curX, curY, startX, startY);
            curX = startX; curY = startY;
        } else if (op == "re") {
            const double x = num(4), y = num(3), w = num(2), hgt = num(1);
            double dx0, dy0, dx1, dy1;
            apply(ctm, x, y, dx0, dy0);
            apply(ctm, x + w, y + hgt, dx1, dy1);
            Rect r;
            r.x0 = std::min(dx0, dx1);
            r.x1 = std::max(dx0, dx1);
            r.y0 = pageH - std::max(dy0, dy1);
            r.y1 = pageH - std::min(dy0, dy1);
            // 흰색 사각형은 숫자 뒤 가림막(마스크)이다 — 선도 빔도 아니다
            if (fillWhite) {
                curX = x;
                curY = y;
                startX = x;
                startY = y;
                hasCur = true;
                st.clear();
                continue;
            }
            // 얇고 긴 사각형 = 채워 그린 가로줄 (보표 선)
            if (r.y1 - r.y0 < 1.6 && r.x1 - r.x0 > 60.0) {
                HLine hl;
                hl.y = (r.y0 + r.y1) * 0.5;
                hl.x0 = r.x0;
                hl.x1 = r.x1;
                pending.push_back(hl);
            } else if (r.y1 - r.y0 < 4.0 && r.x1 - r.x0 > 2.5) {
                out.rects.push_back(r); // 빔 후보
            }
            curX = x; curY = y;
            startX = x; startY = y;
            hasCur = true;
        } else if (op == "S" || op == "s" || op == "f" || op == "F" || op == "f*" || op == "B" ||
                   op == "B*" || op == "b" || op == "b*") {
            for (auto& h : pending) out.lines.push_back(h);
            for (auto& v : pendingV) out.vlines.push_back(v);
            for (auto& a : pendingA) out.arcs.push_back(a);
            // 곡선 경로 = 이음줄(타이/슬러) 후보: 좌/우 극단점(양 팁)을 기록
            if (curveCnt > 0 && exValid && !fillWhite) {
                double ax0, ay0, ax1, ay1;
                apply(ctm, exMinX, exMinY, ax0, ay0);
                apply(ctm, exMaxX, exMaxY, ax1, ay1);
                const double dx = std::fabs(ax1 - ax0);
                if (dx > 4.0 && dx < 120.0 && std::fabs(ay1 - ay0) < 14.0) {
                    Arc a;
                    a.x0 = std::min(ax0, ax1);
                    a.x1 = std::max(ax0, ax1);
                    a.y0 = pageH - (ax0 < ax1 ? ay0 : ay1);
                    a.y1 = pageH - (ax0 < ax1 ? ay1 : ay0);
                    out.arcs.push_back(a);
                }
            }
            pending.clear();
            pendingV.clear();
            pendingA.clear();
            hasCur = false;
            curveCnt = 0;
            exValid = false;
        } else if (op == "n") {
            pending.clear();
            pendingV.clear();
            pendingA.clear();
            hasCur = false;
            curveCnt = 0;
            exValid = false;
        } else if (op == "BT") {
            tm = Mat();
            tlm = Mat();
        } else if (op == "ET") {
            // nothing
        } else if (op == "Tf") {
            fsize = num(1);
            if (st.size() >= 2 && st[st.size() - 2]->type == Obj::Name) {
                auto it = fonts.find(st[st.size() - 2]->str);
                font = it != fonts.end() ? &it->second : nullptr;
            }
        } else if (op == "TL") {
            leading = num(1);
        } else if (op == "Td" || op == "TD") {
            const double tx = num(2), ty = num(1);
            if (op == "TD") leading = -ty;
            Mat t;
            t.e = tx;
            t.f = ty;
            tlm = mul(t, tlm);
            tm = tlm;
        } else if (op == "Tm") {
            Mat m;
            m.a = num(6); m.b = num(5); m.c = num(4);
            m.d = num(3); m.e = num(2); m.f = num(1);
            tm = m;
            tlm = m;
        } else if (op == "T*") {
            Mat t;
            t.f = -leading;
            tlm = mul(t, tlm);
            tm = tlm;
        } else if (op == "Tj" || op == "TJ" || op == "'" || op == "\"") {
            if (op == "'" || op == "\"") {
                Mat t;
                t.f = -leading;
                tlm = mul(t, tlm);
                tm = tlm;
            }
            std::string raw;
            if (!st.empty()) {
                const ObjPtr& top = st.back();
                if (top->type == Obj::Str) {
                    raw = top->str;
                } else if (top->type == Obj::Arr) {
                    for (auto& e : top->arr)
                        if (e->type == Obj::Str) raw += e->str;
                }
            }
            const std::string text = decodeShow(raw, font);
            if (!raw.empty()) {
                // 해독되지 않는 음악 기호도 위치는 남긴다 (마디 반복 기호 등)
                const Mat mg = mul(tm, ctm);
                const double sy = std::sqrt(mg.b * mg.b + mg.d * mg.d);
                TextItem gi;
                gi.x = mg.e;
                gi.y = pageH - mg.f; // 베이스라인 (top-down)
                gi.size = fsize * sy;
                gi.text = text;
                gi.code = (font && font->twoByte && raw.size() >= 2)
                              ? (((uint8_t)raw[0] << 8) | (uint8_t)raw[1])
                              : (uint8_t)raw[0];
                out.glyphs.push_back(gi);
            }
            if (!text.empty()) {
                const Mat m = mul(tm, ctm);
                const double scaleY = std::sqrt(m.b * m.b + m.d * m.d);
                const double eff = fsize * scaleY;
                TextItem ti;
                ti.x = m.e;
                ti.y = (pageH - m.f) - 0.36 * eff; // 베이스라인 -> 글자 중심 (top-down)
                ti.size = eff;
                ti.text = text;
                // 3자리까지 (마디 번호가 100을 넘을 수 있다. 프렛은 어차피 24 이하만 쓴다)
                bool allDigit = !text.empty() && text.size() <= 3;
                for (char ch : text)
                    if (!std::isdigit((uint8_t)ch)) allDigit = false;
                if (allDigit) {
                    out.digits.push_back(ti);
                } else if (text == ".") {
                    // 음악 폰트의 '.' = 부점 (원점이 곧 점의 위치라 보정하지 않는다)
                    ti.y = pageH - m.f;
                    out.dots.push_back(ti);
                }
            }
        } else if (op == "BI") {
            // 인라인 이미지 — EI까지 건너뛴다
            while (lx.p + 1 < lx.n) {
                if (lx.d[lx.p] == 'E' && lx.d[lx.p + 1] == 'I' &&
                    (lx.p + 2 >= lx.n || isWs(lx.d[lx.p + 2]))) {
                    lx.p += 2;
                    break;
                }
                ++lx.p;
            }
        }
        st.clear();
    }
}

// ───────────────────────── 보표/타브 ─────────────────────────
struct Staff {
    int page = 0;
    std::vector<double> ys; // 위에서 아래로
    double x0 = 0, x1 = 0;
};

std::vector<Staff> findStaves(const PageContent& pc, int page) {
    // y가 같은 선 병합
    std::vector<HLine> ls = pc.lines;
    std::sort(ls.begin(), ls.end(), [](const HLine& a, const HLine& b) {
        return a.y != b.y ? a.y < b.y : a.x0 < b.x0;
    });
    std::vector<HLine> merged;
    for (const auto& l : ls) {
        if (!merged.empty() && std::fabs(merged.back().y - l.y) < 0.4 &&
            l.x0 < merged.back().x1 + 3.0 && l.x1 > merged.back().x0 - 3.0) {
            merged.back().x0 = std::min(merged.back().x0, l.x0);
            merged.back().x1 = std::max(merged.back().x1, l.x1);
        } else {
            merged.push_back(l);
        }
    }

    std::vector<Staff> out;
    std::size_t i = 0;
    while (i < merged.size()) {
        std::vector<std::size_t> grp{i};
        std::size_t j = i + 1;
        while (j < merged.size()) {
            const double gap = merged[j].y - merged[grp.back()].y;
            if (gap < 2.0) { // 같은 줄 중복
                ++j;
                continue;
            }
            if (gap >= 2.5 && gap <= 9.0 &&
                std::fabs(merged[j].x0 - merged[grp.front()].x0) < 40.0) {
                grp.push_back(j);
                ++j;
            } else {
                break;
            }
        }
        if (grp.size() >= 5) {
            Staff s;
            s.page = page;
            s.x0 = merged[grp.front()].x0;
            s.x1 = merged[grp.front()].x1;
            for (auto k : grp) {
                s.ys.push_back(merged[k].y);
                s.x0 = std::min(s.x0, merged[k].x0);
                s.x1 = std::max(s.x1, merged[k].x1);
            }
            out.push_back(s);
            i = j;
        } else {
            ++i;
        }
    }
    return out;
}

struct Col {
    double x;
    std::string n[6];
};

// 이 보표에 찍힌 프렛 숫자의 대표 크기. 박자표 숫자는 이보다 훨씬 크게 그려지므로
// 이 값을 기준으로 둘을 가른다.
double fretDigitSize(const Staff& st, const std::vector<TextItem>& digits) {
    const double gap =
        st.ys.size() >= 2 ? (st.ys.back() - st.ys.front()) / (st.ys.size() - 1) : 4.5;
    std::vector<double> sizes;
    for (const auto& d : digits) {
        if (d.x < st.x0 - 6.0 || d.x > st.x1 + 6.0) continue;
        if (d.y < st.ys.front() - gap || d.y > st.ys.back() + gap) continue;
        sizes.push_back(d.size);
    }
    if (sizes.empty()) return 0.0;
    std::sort(sizes.begin(), sizes.end());
    // 중앙값이 아니라 하위 25%를 쓴다: 음이 적은 보표에서는 박자표 숫자(크다)가
    // 절반을 넘어 중앙값을 밀어올려, 크기 판별이 통째로 뒤집힌다.
    return sizes[sizes.size() / 4];
}

// 박자표 읽기: 보표 앞머리에 크게 쓰인 숫자 두 개(위=분자, 아래=분모).
// 못 찾으면 false (온음표 기호 c = 4/4이거나, 앞 단의 박자를 이어받는다).
bool detectTimeSig(const Staff& st, const std::vector<TextItem>& digits, double fretSize,
                   int& numOut, int& denOut) {
    if (fretSize <= 0.0) return false;
    const double gap =
        st.ys.size() >= 2 ? (st.ys.back() - st.ys.front()) / (st.ys.size() - 1) : 4.5;
    const double top = st.ys.front(), bot = st.ys.back();

    struct Big {
        double x, y;
        std::string t;
    };
    std::vector<Big> big;
    for (const auto& d : digits) {
        if (d.size < fretSize * 1.6) continue;               // 프렛 숫자 크기면 제외
        if (d.x < st.x0 - 4.0 || d.x > st.x0 + 80.0) continue; // 보표 앞머리에만
        if (d.y < top - gap || d.y > bot + gap) continue;
        big.push_back({d.x, d.y, d.text});
    }
    if (big.size() < 2) return false;
    std::sort(big.begin(), big.end(), [](const Big& a, const Big& b) { return a.x < b.x; });

    // 같은 x에 세로로 쌓인 것들만 (분자/분모). 가로로 붙은 건 12 같은 두 자리 수.
    const double baseX = big.front().x;
    std::vector<Big> col;
    for (const auto& b : big)
        if (b.x < baseX + 14.0) col.push_back(b);
    if (col.size() < 2) return false;

    std::sort(col.begin(), col.end(), [](const Big& a, const Big& b) { return a.y < b.y; });
    const double mid = (col.front().y + col.back().y) * 0.5;
    std::string num, den;
    {
        std::vector<Big> up, dn;
        for (const auto& b : col) (b.y < mid ? up : dn).push_back(b);
        auto join = [](std::vector<Big>& v) {
            std::sort(v.begin(), v.end(), [](const Big& a, const Big& b) { return a.x < b.x; });
            std::string s;
            for (const auto& b : v) s += b.t;
            return s;
        };
        num = join(up);
        den = join(dn);
    }
    const int n = std::atoi(num.c_str());
    const int d = std::atoi(den.c_str());
    if (n < 1 || n > 32) return false;
    if (d != 1 && d != 2 && d != 4 && d != 8 && d != 16) return false;
    numOut = n;
    denOut = d;
    return true;
}

// 보표 하나 -> 열 목록
std::vector<Col> staffColumns(const Staff& st, const std::vector<TextItem>& digits,
                              double fretSize) {
    const double gap = st.ys.size() >= 2 ? (st.ys.back() - st.ys.front()) / (st.ys.size() - 1) : 4.5;
    const double tol = std::max(2.0, gap * 0.55);

    struct Hit {
        double x;
        int s;
        std::string t;
    };
    std::vector<Hit> hits;
    for (const auto& d : digits) {
        if (d.x < st.x0 - 6.0 || d.x > st.x1 + 6.0) continue;
        if (d.y < st.ys.front() - tol || d.y > st.ys.back() + tol) continue;
        // 박자표 숫자는 프렛 숫자가 아니다 (훨씬 크게 그려진다)
        if (fretSize > 0.0 && d.size > fretSize * 1.6) continue;
        // 꾸밈음(작게 그린 숫자)은 리듬 칸을 차지하지 않는다 — 넣으면 박자가 밀린다
        if (fretSize > 0.0 && d.size < fretSize * 0.82) continue;
        int best = 0;
        double bd = 1e9;
        for (int s = 0; s < 6; ++s) {
            const double dist = std::fabs(st.ys[(std::size_t)s] - d.y);
            if (dist < bd) {
                bd = dist;
                best = s;
            }
        }
        if (bd > tol) continue;
        hits.push_back({d.x, best, d.text});
    }
    std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) {
        return a.x != b.x ? a.x < b.x : a.s < b.s;
    });

    // 같은 현에서 아주 가까운 한 자리 숫자 둘 = 두 자리 프렛 (예: 1 0 -> 10)
    std::vector<Hit> mergedHits;
    for (const auto& h : hits) {
        bool done = false;
        for (auto& m : mergedHits) {
            if (m.s == h.s && m.t.size() == 1 && h.t.size() == 1 && h.x - m.x > 0.1 &&
                h.x - m.x < 5.0) {
                const int two = std::atoi((m.t + h.t).c_str());
                if (two <= 24) {
                    m.t += h.t;
                    done = true;
                }
                break;
            }
        }
        if (!done) mergedHits.push_back(h);
    }
    std::sort(mergedHits.begin(), mergedHits.end(),
              [](const Hit& a, const Hit& b) { return a.x < b.x; });

    std::vector<Col> cols;
    for (const auto& h : mergedHits) {
        if (std::atoi(h.t.c_str()) > 24) continue;
        if (!cols.empty() && h.x - cols.back().x < 2.5) {
            cols.back().n[h.s] = h.t;
        } else {
            Col c;
            c.x = h.x;
            c.n[h.s] = h.t;
            cols.push_back(c);
        }
    }
    return cols;
}

// 표준 튜닝 개방현 (위 = 1번줄 high E)
constexpr int kOpenTab[6] = {64, 59, 55, 50, 45, 40};

struct Rhythm {
    std::vector<TabPdfNote> notes;
    uint32_t endTick = 0;
    int stems = 0;
    // 도돌이표: 반복 시작(‖:) / 반복 끝(:‖) 지점의 틱
    std::vector<uint32_t> repeatStarts, repeatEnds;
    // 마디 반복 기호(𝄎): (채울 마디의 시작 틱, 몇 마디짜리인지)
    std::vector<std::pair<uint32_t, int>> measureRepeats;
};

// 박 위치에 따른 셈여림: 마디 첫 박 > 박 > 8분 엇박 > 16분 엇박.
// 전부 100으로 넣으면 기계 소리가 난다 — 사람이 치는 강약의 뼈대만 준다.
uint8_t beatVel(uint32_t tick, uint32_t barTicks, uint32_t beatTicks) {
    if (barTicks == 0 || beatTicks == 0) return 96;
    const uint32_t inBar = tick % barTicks;
    if (inBar == 0) return 106;
    if (inBar % beatTicks == 0) return 97;
    if (inBar % (beatTicks / 2 ? beatTicks / 2 : 1) == 0) return 88;
    return 80;
}

// 가장 가까운 음표 길이로 스냅 (32분 ~ 온음표, 부점 포함)
uint32_t snapDuration(double ticks, uint32_t ppqn) {
    const double cand[] = {ppqn / 8.0,     ppqn / 4.0,     ppqn / 2.0,  ppqn * 0.75,
                           (double)ppqn,   ppqn * 1.5,     ppqn * 2.0,  ppqn * 3.0,
                           ppqn * 4.0};
    double best = cand[0];
    double bd = 1e18;
    for (double c : cand) {
        const double d = std::fabs(c - ticks);
        if (d < bd) {
            bd = d;
            best = c;
        }
    }
    return (uint32_t)std::max<long>(1, std::lround(best));
}

// 마디 반복 기호(𝄎)의 글리프 번호를 알아낸다.
// 폰트마다 번호가 달라서 못 박을 수 없다. 대신 "보표 한가운데 있고, 바로 위에 반복 마디 수
// (2 등)가 적힌 기호"라는 특징으로 찾아낸다. 이렇게 번호를 알아내면 숫자가 없는
// 1마디짜리 기호도 같은 번호로 알아볼 수 있다.
int findMeasureRepeatGlyph(const std::vector<Staff>& staves,
                           const std::vector<PageContent>& pages) {
    std::map<int, int> votes;
    for (const auto& st : staves) {
        if (st.ys.size() != 6) continue;
        const PageContent& pc = pages[(std::size_t)st.page];
        const double top = st.ys.front(), bot = st.ys.back();
        const double gap = (bot - top) / 5.0;
        const double midY = (top + bot) * 0.5;
        for (const auto& g : pc.glyphs) {
            if (g.size < gap * 2.0) continue;
            if (std::fabs(g.y - midY) > gap * 1.6) continue;
            if (g.x < st.x0 || g.x > st.x1) continue;
            for (const auto& d : pc.digits) {
                if (d.size < gap * 2.0) continue;
                if (d.y > top - gap * 0.3 || d.y < top - gap * 4.0) continue;
                if (std::fabs(d.x - g.x) > 16.0) continue;
                const int n = std::atoi(d.text.c_str());
                if (n >= 2 && n <= 8) ++votes[g.code];
                break;
            }
        }
    }
    int best = -1, bestN = 0;
    for (const auto& v : votes)
        if (v.second > bestN) {
            bestN = v.second;
            best = v.first;
        }
    return bestN >= 1 ? best : -1;
}

// 타브 보표 하나의 리듬을 읽는다: 기둥 = 연주 시점, 빔 개수 = 음길이, 마디선 = 시각 재동기화.
Rhythm staffRhythm(const Staff& st, const std::vector<Col>& cols, const PageContent& pc,
                   uint32_t ppqn, uint32_t startTick, int tsNum, int tsDen, int repeatGlyph) {
    Rhythm out;
    out.endTick = startTick;
    if (st.ys.size() != 6) return out;

    const double top = st.ys.front(), bot = st.ys.back();
    const double gap = (bot - top) / 5.0;
    // 한 마디의 길이 = 분자 × (온음표/분모).  4/4 -> ppqn*4, 3/4 -> ppqn*3, 6/8 -> ppqn*3
    const uint32_t barTicks =
        std::max<uint32_t>(1, (uint32_t)((uint64_t)ppqn * 4 * (uint32_t)tsNum / (uint32_t)tsDen));
    auto roundUpBar = [&](uint32_t t) {
        return (uint32_t)(((t + barTicks - 1) / barTicks) * barTicks);
    };

    // 기둥: 보표에 닿는 적당한 길이의 세로선. 빔이 어느 쪽 끝에 붙는지는
    // 조판마다 달라서(위/아래/보표 안), 양 끝 다 살펴 많은 쪽을 쓴다.
    struct Stem {
        double x;
        double y0, y1; // 위/아래 끝
    };
    std::vector<Stem> stems;
    // 마디선: 보표를 위아래로 관통하는 세로선
    std::vector<double> bars;
    for (const auto& v : pc.vlines) {
        if (v.x < st.x0 - 6.0 || v.x > st.x1 + 6.0) continue;
        const double len = v.y1 - v.y0;
        if (v.y0 <= top + gap * 0.6 && v.y1 >= bot - gap * 0.6) {
            // 마디선은 보표 가로 범위 안에 있어야 한다. 왼쪽의 시스템 대괄호도
            // 보표를 세로로 관통하지만 보표선보다 왼쪽에 있어 마디선이 아니다.
            if (v.x >= st.x0 - 1.0 && v.x <= st.x1 + 2.0) bars.push_back(v.x);
            continue;
        }
        if (len < gap * 1.2 || len > gap * 8.0) continue;
        // 기둥은 이 보표에 닿아 있어야 한다 — 세로 범위를 보표 주변으로 제한하지 않으면
        // 페이지 아래쪽 다른 보표의 기둥까지 끌어와 시각이 엉킨다.
        // 조판마다 기둥 모양이 다르다: 보표 밖으로 길게(SCORE류), 바닥선을 1pt만 지나게,
        // 심지어 보표 한가운데 떠 있게(짧은 mute 스트로크)도 그린다. 그래서
        // "위/아래로 뻗었는가"를 따지지 말고, 보표 근처의 적당한 세로선은 전부 기둥으로 본다.
        // (보표를 위아래로 다 관통하는 선은 위에서 이미 마디선으로 걸러졌다.)
        const bool touches = v.y1 > top - gap * 0.5 && v.y0 < bot + gap * 0.5;
        const bool nearby = v.y0 > top - gap * 5.0 && v.y1 < bot + gap * 5.0;
        if (touches && nearby) stems.push_back({v.x, v.y0, v.y1});
    }
    std::sort(stems.begin(), stems.end(),
              [](const Stem& a, const Stem& b) { return a.x < b.x; });
    std::sort(bars.begin(), bars.end());
    // 겹친 선 정리 (이중 마디선/반복 기호)
    stems.erase(std::unique(stems.begin(), stems.end(),
                            [](const Stem& a, const Stem& b) {
                                return std::fabs(a.x - b.x) < 1.5;
                            }),
                stems.end());
    // 겹세로줄·반복기호(∥: :∥)는 선이 2~3개 붙어 있다. 넉넉히 묶지 않으면
    // 마디를 여러 번 센 셈이 되어 단 길이가 늘어난다 (마디 폭은 최소 30pt대라 안전).
    bars.erase(std::unique(bars.begin(), bars.end(),
                           [](double a, double b) { return std::fabs(a - b) < 9.0; }),
               bars.end());
    out.stems = (int)stems.size();
#ifdef PDFTAB_DEBUG
    std::fprintf(stderr, "[staff y=%.1f x=%.1f~%.1f] 기둥 %d, 마디선 %d:", top, st.x0, st.x1,
                 (int)stems.size(), (int)bars.size());
    for (double b : bars) std::fprintf(stderr, " %.1f", b);
    std::fprintf(stderr, "\n");
#endif

    // 빔: 기둥이 뻗은 쪽(보표 위/아래)의 얇은 사각형. 한쪽만 보면 반대 방향
    // 기둥의 빔을 놓쳐 8분·16분이 통째로 4분음표가 되어 버린다.
    // 빔 후보: 얇은 검정 사각형 (흰 가림막은 수집 단계에서 이미 제외).
    // 빔이 보표 밖에 그려지는 악보도, 보표 안쪽 아랫줄에 그려지는 악보도 있어서
    // "어느 밴드에 있나"로 가르면 안 된다 — 기둥의 빔 쪽 끝 근처인지로 가른다.
    std::vector<Rect> beams;
    for (const auto& r : pc.rects) {
        if (r.x1 < st.x0 - 6.0 || r.x0 > st.x1 + 6.0) continue;
        if (r.y1 - r.y0 > 3.2) continue; // 빔은 얇다 (도돌이표 점 같은 사각 기호 제외)
        if (r.y1 < top - gap * 3.6 || r.y0 > bot + gap * 3.6) continue;
        beams.push_back(r);
    }

    struct Ev {
        double x;
        uint32_t dur = 0; // 빔으로 읽은 길이 (기둥 없으면 0)
        int beams = -1;   // -1 = 기둥 없음, 0 = 빔 없음(4분/깃발/온음표), 1+ = 빔 개수
        const Col* col = nullptr;
        bool muted = false; // 뮤트 점(·) — 짧고 여리게
        bool dead = false;  // X(데드) — 퍼커시브 척
    };
    std::vector<Ev> evs;
    std::vector<bool> used(cols.size(), false);

    for (const Stem& stem : stems) {
        const double sx = stem.x;
        Ev e;
        e.x = sx;
        // 빔은 기둥의 한쪽 끝에 몰려 붙는다 — 양 끝을 다 세서 많은 쪽을 쓴다
        int c0 = 0, c1 = 0;
        for (const auto& b : beams) {
            if (b.x0 - 1.0 > sx || sx > b.x1 + 1.0) continue;
            const double cy = (b.y0 + b.y1) * 0.5;
            if (std::fabs(cy - stem.y0) < gap * 2.4) ++c0;
            if (std::fabs(cy - stem.y1) < gap * 2.4) ++c1;
        }
        int beamCount = std::max(c0, c1);
        beamCount = std::min(beamCount, 4);
        e.beams = beamCount;
        e.dur = std::max<uint32_t>(1, ppqn >> beamCount); // 빔 0=4분, 1=8분, 2=16분...
        // 이 기둥에 붙는 프렛 열 (숫자는 기둥 살짝 왼쪽에 놓인다)
        int best = -1;
        double bd = 1e9;
        for (std::size_t i = 0; i < cols.size(); ++i) {
            if (used[i]) continue;
            const double dx = sx - cols[i].x;
            if (dx < 1.0 || dx > 14.0) continue;
            if (dx < bd) {
                bd = dx;
                best = (int)i;
            }
        }
        if (best >= 0) {
            used[(std::size_t)best] = true;
            e.col = &cols[(std::size_t)best];
        }
        // 부점: 반드시 프렛 숫자와 "같은 줄, 바로 오른쪽"에 있어야 한다.
        // 숫자 위/아래에 찍히는 점은 뮤트(·) 표시다 — 부점이 아니라 "짧고 여리게".
        if (e.col) {
            bool dotted = false;
            for (const auto& d : pc.dots) {
                const double ddx = d.x - e.col->x;
                if (ddx < -3.0 || ddx > 12.0) continue;
                if (d.y < top - gap * 1.6 || d.y > bot + gap * 1.6) continue;
                bool aug = false;
                if (ddx >= 2.0)
                    for (int s = 0; s < 6 && !aug; ++s) {
                        if (e.col->n[s].empty()) continue;
                        if (std::fabs(d.y - st.ys[(std::size_t)s]) < gap * 0.55) aug = true;
                    }
                if (aug)
                    dotted = true;
                else
                    e.muted = true;
            }
            if (dotted) e.dur = e.dur * 3 / 2;
        } else {
            // 프렛 숫자가 없는 기둥: 슬래시(직전 코드 재타) 또는 X(뮤트 척).
            // X는 현 줄 위에 놓인 작은 기호가 2개 이상 쌓인다 (코드의 각 줄마다 하나씩).
            int rowGlyphs = 0;
            for (const auto& g : pc.glyphs) {
                if (g.x < sx - 14.0 || g.x > sx - 0.5) continue;
                for (int s = 0; s < 6; ++s)
                    if (std::fabs(g.y - st.ys[(std::size_t)s]) < gap * 0.6) {
                        ++rowGlyphs;
                        break;
                    }
            }
            if (rowGlyphs >= 2) e.dead = true;
        }
        evs.push_back(e);
    }
    // 기둥이 없는 열 (온음표/2분음표 등) 도 사건으로
    for (std::size_t i = 0; i < cols.size(); ++i)
        if (!used[i]) {
            Ev e;
            e.x = cols[i].x;
            e.col = &cols[i];
            evs.push_back(e);
        }
    std::sort(evs.begin(), evs.end(), [](const Ev& a, const Ev& b) { return a.x < b.x; });

    // 보표 머리(음자리표·조표·박자표·여는 세로줄·반복기호 ‖:)의 세로줄은 마디를 나누지
    // 않는다. "첫 음보다 왼쪽"으로 자르면 안 된다 — 기타가 앞 몇 마디를 쉬는 보표에서는
    // 진짜 마디선까지 버려서 마디 번호가 통째로 어긋난다.
    const double headX = st.x0 + 45.0;
    double openX = st.x0;
    for (double b : bars)
        if (b < headX) openX = b;
    bars.erase(std::remove_if(bars.begin(), bars.end(),
                              [&](double b) { return b < headX; }),
               bars.end());

    // 이 x가 속한 마디의 가로 범위. 마디 하나의 길이는 정확히 barTicks이므로
    // (마디 폭 / barTicks) 이 "틱당 pt" 자가 된다 — 빔이 없는 음의 길이를 잰다.
    auto pointsPerTick = [&](double x) -> double {
        if (bars.empty()) return 0.0;
        std::size_t mi = 0;
        while (mi < bars.size() && bars[mi] < x - 1.0) ++mi;
        const double s = (mi == 0) ? openX : bars[mi - 1];
        const double e = (mi < bars.size()) ? bars[mi] : st.x1;
        return e > s ? (e - s) / (double)barTicks : 0.0;
    };
    auto measureEndX = [&](double x) -> double {
        std::size_t mi = 0;
        while (mi < bars.size() && bars[mi] < x - 1.0) ++mi;
        return (mi < bars.size()) ? bars[mi] : st.x1;
    };

    // ── 도돌이표 ──
    // 굵은 세로줄(보통 선 굵기 0.6, 도돌이표는 2.3쯤) + 점(·) 두 개.
    // 점이 줄 오른쪽에 있으면 반복 시작(‖:), 왼쪽에 있으면 반복 끝(:‖).
    // 점이 없으면 그냥 곡 끝의 굵은 마디선이다.
    {
        auto measureIndexAt = [&](double x, bool after) {
            std::size_t k = 0;
            const double lim = after ? x + 1.0 : x - 1.0;
            for (double b : bars)
                if (b < lim) ++k;
            return startTick + (uint32_t)k * barTicks;
        };
        for (const auto& v : pc.vlines) {
            if (v.w < 1.5) continue; // 보통 마디선은 얇다
            if (v.x < st.x0 - 8.0 || v.x > st.x1 + 8.0) continue;
            if (v.y0 > top + 3.0 || v.y1 < bot - 3.0) continue; // 이 보표를 관통해야 한다
            int left = 0, right = 0;
            for (const auto& d : pc.dots) {
                if (d.y < top - gap || d.y > bot + gap) continue;
                const double dx = d.x - v.x;
                if (std::fabs(dx) > 14.0) continue;
                if (dx > 1.0)
                    ++right;
                else if (dx < -1.0)
                    ++left;
            }
            if (right >= 2 && right > left)
                out.repeatStarts.push_back(measureIndexAt(v.x, false));
            else if (left >= 2 && left > right)
                out.repeatEnds.push_back(measureIndexAt(v.x, true));
        }

        // ── 마디 반복 기호 (𝄎: "앞 마디를 그대로") ──
        // 마디선 한가운데에 걸터앉은 음악 기호. 음표·쉼표는 마디선 위에 놓이지 않으므로
        // "보표 안 + 마디선 위"라는 위치만으로 확실히 가려낼 수 있다.
        // 기호 위에 숫자(2 등)가 있으면 그만큼의 마디를 반복한다 (없으면 2마디).
        const double midY = (top + bot) * 0.5;
        for (const auto& g : pc.glyphs) {
            if (repeatGlyph < 0 || g.code != repeatGlyph) continue; // 진짜 반복 기호만
            if (std::fabs(g.y - midY) > gap * 1.6) continue;        // 보표 한가운데
            if (g.x < st.x0 || g.x > st.x1) continue;

            // 기호 위의 숫자 = 몇 마디짜리인지 (없으면 1마디)
            int nbars = 0;
            for (const auto& d : pc.digits) {
                if (d.size < gap * 2.0) continue;
                if (d.y > top - gap * 0.3 || d.y < top - gap * 4.0) continue;
                if (std::fabs(d.x - g.x) > 16.0) continue;
                const int v2 = std::atoi(d.text.c_str());
                if (v2 >= 2 && v2 <= 8) nbars = v2;
                break;
            }

            // 마디선에 걸터앉았으면 그 마디선 앞뒤 마디들, 아니면 그 기호가 있는 마디 하나.
            std::size_t k = bars.size();
            for (std::size_t i = 0; i < bars.size(); ++i)
                if (std::fabs(g.x - bars[i]) < 14.0) {
                    k = i;
                    break;
                }
            uint32_t targetStart;
            if (k < bars.size()) {
                if (nbars == 0) nbars = 2; // 마디선을 걸터앉으면 앞뒤 두 마디
                targetStart = startTick + (uint32_t)k * barTicks;
            } else {
                if (nbars == 0) nbars = 1;
                std::size_t m = 0; // 이 기호가 들어 있는 마디
                while (m < bars.size() && bars[m] < g.x) ++m;
                targetStart = startTick + (uint32_t)m * barTicks;
            }
#ifdef PDFTAB_DEBUG
            std::fprintf(stderr, "  [마디반복] x=%.1f nbars=%d -> 마디 %u 부터\n", g.x, nbars,
                         targetStart / barTicks + 1);
#endif
            if (targetStart < (uint32_t)nbars * barTicks) continue; // 앞에 베낄 마디가 없다
            out.measureRepeats.push_back({targetStart, nbars});
        }
    }

    // "틱당 몇 pt" 자. 빔이 달린 음은 길이를 확실히 아니까, 그 음들의 가로 간격에서
    // 축척을 잰다 (같은 마디 안에서만 — 마디선을 넘으면 여백이 끼어 부정확해진다).
    // 첫 마디는 음자리표·박자표 때문에 폭이 넓어서, 마디 폭으로 재면 틀린다.
    std::vector<double> scales;
    for (std::size_t i = 0; i + 1 < evs.size(); ++i) {
        if (evs[i].beams < 1 || evs[i].dur == 0) continue;
        const double nx = evs[i + 1].x;
        if (nx <= evs[i].x || nx >= measureEndX(evs[i].x)) continue;
        scales.push_back((nx - evs[i].x) / (double)evs[i].dur);
    }
    double ptPerTick = 0;
    if (!scales.empty()) {
        std::sort(scales.begin(), scales.end());
        ptPerTick = scales[scales.size() / 2];
    }

    // 마디선 = 절대 기준점. 마디선 i를 지나면 (i+1)번째 마디의 정확한 시각으로 되돌린다.
    // 올림만 하면 길이를 길게 읽은 마디의 오차가 끝까지 번지므로 하드 리셋해야 한다.
    // 마디 첫 음의 "마디선으로부터의 거리" 중앙값 = 쉼표 없는 마디의 기본 여백.
    // 이보다 뚜렷이 오른쪽에서 시작하는 마디는 앞에 쉼표가 있는 것이다 (픽업 프레이즈).
    double headPad = 0.0;
    {
        std::vector<double> offs;
        std::size_t bj = 0;
        double lastBar = -1.0;
        for (const auto& e : evs) {
            while (bj < bars.size() && bars[bj] < e.x - 1.0) ++bj;
            const double mx0 = (bj == 0) ? -1.0 : bars[bj - 1];
            if (mx0 > 0.0 && mx0 != lastBar) { // 각 마디의 첫 사건만
                offs.push_back(e.x - mx0);
                lastBar = mx0;
            }
        }
        if (offs.size() >= 3) {
            std::sort(offs.begin(), offs.end());
            headPad = offs[offs.size() / 2];
        } else {
            headPad = gap * 2.6; // 표본이 없으면 관례적 여백(줄간격의 2.6배)으로
        }
    }

    uint32_t cur = startTick;
    std::size_t bi = 0;
    uint32_t prevDur = 0;
    std::vector<std::pair<uint8_t, int8_t>> prevChord;
    std::vector<std::pair<double, uint32_t>> anchors; // (사건 x, 틱) — 이음줄 매칭용
    for (std::size_t i = 0; i < evs.size() && !stems.empty(); ++i) {
        Ev& e = evs[i];
        bool newMeasure = false;
        while (bi < bars.size() && bars[bi] < e.x - 1.0) {
            cur = startTick + (uint32_t)(bi + 1) * barTicks;
            ++bi;
            newMeasure = true;
        }
        // 마디 앞 쉼표: 이 마디의 첫 음이 기본 여백보다 뚜렷이 오른쪽이면 그만큼 쉰다.
        // (마디선 직후 여백은 쉼표가 없어도 늘 있으므로 headPad로 상쇄한다)
        if (newMeasure && bi > 0 && headPad > 0.0 && ptPerTick > 0.0) {
            const double lead = (e.x - bars[bi - 1] - headPad) / ptPerTick;
            if (lead > 150.0)
                cur += (uint32_t)std::max<long>(1, std::lround(lead / 240.0)) * 240u;
        }
        // 쉼표: 같은 마디 안에서 앞 사건과의 가로 간격이 앞 사건의 길이보다
        // 뚜렷이 크면 그 차이만큼 쉬는 것이다 (쉼표 기호는 따로 안 읽는다).
        // 8분 단위로 반올림 — 조판 간격의 잔떨림이 쉼표로 둔갑하지 않게.
        if (i > 0 && prevDur > 0) {
            bool sameMeasure = true;
            for (double b : bars)
                if (b > evs[i - 1].x + 1.0 && b < e.x - 1.0) {
                    sameMeasure = false;
                    break;
                }
            const double ruler = ptPerTick > 0.0 ? ptPerTick : pointsPerTick(e.x);
            if (sameMeasure && ruler > 0.0) {
                const double excess = (e.x - evs[i - 1].x) / ruler - (double)prevDur;
                if (excess > 150.0)
                    cur += (uint32_t)std::max<long>(1, std::lround(excess / 240.0)) * 240u;
            }
        }
        uint32_t dur = e.dur;
        // 빔이 있으면 그게 가장 정확하다. 빔이 없는 음(4분·온음표·기둥 없는 음, 그리고
        // 빔 대신 깃발이 달린 홑 8분/16분)은 빔으로 구분할 수 없으므로 가로 간격으로 잰다.
        if (e.beams <= 0) {
            // 빔 있는 음에서 잰 자를 먼저 쓰고, 그런 음이 하나도 없으면 마디 폭으로 잰다.
            const double ppt = ptPerTick > 0.0 ? ptPerTick : pointsPerTick(e.x);
            const double mEnd = measureEndX(e.x);
            double nextX = (i + 1 < evs.size()) ? evs[i + 1].x : mEnd;
            if (nextX > mEnd) nextX = mEnd; // 마디 끝을 넘지 않는다
            const double gap = nextX - e.x;
            if (ppt > 0.0 && gap > 0.5) dur = snapDuration(gap / ppt, ppqn);
        }
        if (dur == 0) dur = ppqn;
        std::vector<std::pair<uint8_t, int8_t>> chord; // (노트, 줄)
        if (e.col) {
            for (int s = 0; s < 6; ++s) {
                if (e.col->n[s].empty()) continue;
                const int fret = std::atoi(e.col->n[s].c_str());
                if (fret < 0 || fret > 24) continue;
                const int note = kOpenTab[s] + fret;
                if (note >= 0 && note <= 127) chord.push_back({(uint8_t)note, (int8_t)s});
            }
        } else {
            chord = prevChord; // 슬래시(/) = 직전 코드를 다시 친다
        }
        // 셈여림: 박 위치 강약을 뼈대로, 뮤트/데드는 짧고 여리게
        uint8_t vel = beatVel(cur, barTicks, barTicks / (uint32_t)std::max(1, tsNum));
        uint8_t artic = 94;
        if (e.dead) {
            vel = 52;
            artic = 30;
        } else if (e.muted) {
            vel = (uint8_t)(vel > 18 ? vel - 18 : 40);
            artic = 50;
        }
        for (const auto& cn : chord)
            out.notes.push_back({cur, cn.first, dur, cn.second, vel, artic});
        if (!chord.empty()) prevChord = chord;
        anchors.push_back({e.x, cur});
        cur += dur;
        prevDur = dur;
    }

    // ── 이음줄/슬라이드 적용 ──
    // 같은 줄의 두 음을 잇는 호선: 같은 음(프렛)이면 타이(한 음으로 병합),
    // 다른 음이면 레가토(H/P/슬라이드 — 앞 음을 뒷 음까지 붙이고 뒷 음은 여리게).
    std::vector<std::pair<double, double>> doneArcs; // 처리한 호선 (윤곽+칠 중복 방지)
    for (const auto& a : pc.arcs) {
        if (a.x0 < st.x0 - 4.0 || a.x1 > st.x1 + 4.0) continue;
        bool dup = false;
        for (const auto& d : doneArcs)
            if (std::fabs(d.first - a.x0) < 2.0 && std::fabs(d.second - a.x1) < 2.0) dup = true;
        if (dup) continue;
        // 양 끝이 같은 현 줄 근처여야 한다. 타브의 타이 곡선은 숫자 아래로
        // 반 줄쯤 처져 그려지므로 허용치를 넉넉히 잡는다.
        int row = -1;
        double bestRowD = 1e9;
        for (int s = 0; s < 6; ++s) {
            const double d0 = std::fabs(a.y0 - st.ys[(std::size_t)s]);
            const double d1 = std::fabs(a.y1 - st.ys[(std::size_t)s]);
            if (d0 < gap * 1.4 && d1 < gap * 1.4 && d0 + d1 < bestRowD) {
                bestRowD = d0 + d1;
                row = s;
            }
        }
        if (row < 0) continue;
        // 호선 양 끝에서 가장 가까운 사건(틱) 찾기
        auto nearTick = [&](double x, uint32_t& tickOut) {
            double best = 14.0;
            bool found = false;
            for (const auto& an : anchors) {
                const double d = std::fabs(an.first - x);
                if (d < best) {
                    best = d;
                    tickOut = an.second;
                    found = true;
                }
            }
            return found;
        };
        uint32_t tA = 0, tB = 0;
        const bool okA = nearTick(a.x0, tA);
        const bool okB = nearTick(a.x1, tB);
#ifdef PDFTAB_DEBUG
        std::fprintf(stderr, "  [호선] x=%.1f~%.1f y=%.1f/%.1f row=%d A=%d B=%d tA=%u tB=%u\n",
                     a.x0, a.x1, a.y0, a.y1, row, (int)okA, (int)okB, tA, tB);
#endif
        if (!okA || !okB || tB <= tA) continue;
        // 곡선은 숫자를 피해 한 줄 위/아래에 그려지므로, 음의 줄과 ±1 줄까지 허용.
        // 두 틱 모두에 노트가 있는 줄을 고른다 (곡선 줄과 가까운 순서로).
        TabPdfNote* nA = nullptr;
        TabPdfNote* nB = nullptr;
        const int cand[3] = {row, row + 1, row - 1};
        for (int ci = 0; ci < 3 && !(nA && nB); ++ci) {
            const int s = cand[ci];
            if (s < 0 || s > 5) continue;
            nA = nullptr;
            nB = nullptr;
            for (auto& n : out.notes) {
                if ((int)n.strIdx != s) continue;
                if (n.tick == tA) nA = &n;
                if (n.tick == tB) nB = &n;
            }
        }
        if (!nA || !nB || nA->durTicks == 0 || nB->durTicks == 0) continue;
        doneArcs.push_back({a.x0, a.x1});
        if (nA->note == nB->note) {
            // 타이: 두 번째 타격을 없애고 첫 음을 그만큼 늘인다
            nA->durTicks = (tB - tA) + nB->durTicks;
            nB->durTicks = 0; // 아래에서 제거
        } else {
            // 해머링/풀링/슬라이드: 앞 음을 뒷 음 시작까지 꽉 채우고, 뒷 음은 여리게
            nA->artic = 100;
            nA->durTicks = tB - tA;
            if (nB->vel > 76) nB->vel = 76;
        }
    }
    out.notes.erase(std::remove_if(out.notes.begin(), out.notes.end(),
                                   [](const TabPdfNote& n) { return n.durTicks == 0; }),
                    out.notes.end());
    // 이 단의 길이도 마디선 개수로 못박는다 (마지막 마디를 잘못 읽어도 다음 단이 안 밀린다).
    // 단, 마지막 마디선이 보표 끝에 없으면 닫히지 않은 마디가 하나 더 있는 것이다.
    if (bars.empty()) {
        out.endTick = roundUpBar(cur);
    } else {
        const bool closed = bars.back() >= st.x1 - 15.0;
        const uint32_t measures = (uint32_t)bars.size() + (closed ? 0u : 1u);
        out.endTick = startTick + measures * barTicks;
    }
    // 마지막 마디를 조금 길게 읽어 단 밖으로 삐져나온 노트는 버린다 (다음 단과 겹치지 않게)
    {
        const uint32_t end = out.endTick;
        out.notes.erase(std::remove_if(out.notes.begin(), out.notes.end(),
                                       [end](const TabPdfNote& n) { return n.tick >= end; }),
                        out.notes.end());
    }
    return out;
}

// 열들 -> 6줄 ASCII 블록 (가로 간격 = 대략의 리듬)
std::string colsToAscii(const std::vector<Col>& cols) {
    static const char* kNames[6] = {"e", "B", "G", "D", "A", "E"};
    std::string lines[6];
    for (int s = 0; s < 6; ++s) lines[s] = std::string(kNames[s]) + "|";

    if (cols.empty()) {
        for (int s = 0; s < 6; ++s) lines[s] += "----------------|";
        std::string out;
        for (int s = 0; s < 6; ++s) out += lines[s] + "\n";
        return out;
    }

    // 열 간 x 간격 -> 시간 칸 수 (조판 간격이 리듬을 대략 담고 있다)
    std::vector<double> gaps;
    for (std::size_t i = 1; i < cols.size(); ++i) gaps.push_back(cols[i].x - cols[i - 1].x);
    double unit = 0;
    if (!gaps.empty()) {
        std::vector<double> sorted = gaps;
        std::sort(sorted.begin(), sorted.end());
        unit = sorted[sorted.size() / 10]; // 10퍼센타일 = 가장 촘촘한 간격
        if (unit < 1.0) unit = sorted[sorted.size() / 2];
        if (unit < 1.0) unit = 1.0;
    }

    for (std::size_t i = 0; i < cols.size(); ++i) {
        // 가장 촘촘한 간격 = 2칸으로 잡는다 (숫자 1칸 + 구분 대시 최소 1칸).
        // 열 사이에 대시가 없으면 "5" "7"이 붙어 두 자리 프렛 57로 읽히므로 최소 1칸은 필수.
        int steps = 2;
        if (i > 0 && unit > 0) {
            steps = (int)std::lround(gaps[i - 1] / unit * 2.0);
            steps = std::max(2, std::min(16, steps));
        }
        const int dashes = std::max(1, steps - 1);
        int width = 0;
        for (int s = 0; s < 6; ++s) width = std::max(width, (int)cols[i].n[s].size());
        if (width == 0) width = 1;
        for (int s = 0; s < 6; ++s) {
            lines[s] += std::string((std::size_t)dashes, '-');
            const std::string& v = cols[i].n[s];
            if (v.empty()) {
                lines[s] += std::string((std::size_t)width, '-');
            } else {
                lines[s] += v;
                if ((int)v.size() < width)
                    lines[s] += std::string((std::size_t)(width - (int)v.size()), '-');
            }
        }
    }
    std::string out;
    for (int s = 0; s < 6; ++s) out += lines[s] + "-|\n";
    return out;
}

// UTF-8 경로 열기. 윈도우에서 fopen은 ANSI 경로만 받으므로 (한글 경로가 깨진다)
// UTF-16으로 바꿔 _wfopen을 쓴다.
std::FILE* openUtf8(const std::string& path) {
#ifdef _WIN32
    std::wstring w;
    for (std::size_t i = 0; i < path.size();) {
        const uint8_t c = (uint8_t)path[i];
        uint32_t cp = 0;
        int extra = 0;
        if (c < 0x80) {
            cp = c;
        } else if ((c & 0xE0) == 0xC0) {
            cp = c & 0x1Fu;
            extra = 1;
        } else if ((c & 0xF0) == 0xE0) {
            cp = c & 0x0Fu;
            extra = 2;
        } else if ((c & 0xF8) == 0xF0) {
            cp = c & 0x07u;
            extra = 3;
        } else {
            ++i;
            continue;
        }
        ++i;
        for (int k = 0; k < extra && i < path.size(); ++k, ++i)
            cp = (cp << 6) | ((uint8_t)path[i] & 0x3Fu);
        if (cp >= 0x10000u) {
            cp -= 0x10000u;
            w += (wchar_t)(0xD800u + (cp >> 10));
            w += (wchar_t)(0xDC00u + (cp & 0x3FFu));
        } else {
            w += (wchar_t)cp;
        }
    }
    return _wfopen(w.c_str(), L"rb");
#else
    return std::fopen(path.c_str(), "rb");
#endif
}

} // namespace

TabPdfResult extractTabFromPdf(const std::string& utf8Path, uint32_t ppqn) {
    TabPdfResult res;
    if (ppqn == 0) ppqn = 480;

    std::FILE* fp = openUtf8(utf8Path);
    if (!fp) {
        res.error = "PDF 파일을 열 수 없습니다.";
        return res;
    }
    Doc doc;
    std::fseek(fp, 0, SEEK_END);
    const long sz = std::ftell(fp);
    std::fseek(fp, 0, SEEK_SET);
    if (sz <= 0) {
        std::fclose(fp);
        res.error = "PDF 파일이 비어 있습니다.";
        return res;
    }
    doc.bytes.resize((std::size_t)sz);
    const std::size_t rd = std::fread(&doc.bytes[0], 1, (std::size_t)sz, fp);
    std::fclose(fp);
    doc.bytes.resize(rd);
    if (doc.bytes.compare(0, 5, "%PDF-") != 0 && doc.bytes.find("%PDF-") == std::string::npos) {
        res.error = "PDF 파일이 아닙니다.";
        return res;
    }

    scanObjects(doc);
    findTrailer(doc);
    if (!setupDecryption(doc, res.error)) return res;
    doc.cache.clear(); // 복호화 설정 후 다시 읽는다
    expandObjectStreams(doc);

    // 페이지 모으기 (페이지 트리 우선, 실패 시 /Type /Page 스캔)
    std::vector<std::pair<int, ObjPtr>> pages; // (객체번호, 페이지)
    {
        std::set<int> seen;
        auto root = doc.trailer ? doc.dget(doc.trailer, "Root") : nullptr;
        auto pagesRoot = root ? doc.dget(root, "Pages") : nullptr;
        std::vector<ObjPtr> stack;
        std::vector<int> stackNum;
        if (pagesRoot) {
            stack.push_back(pagesRoot);
            stackNum.push_back(0);
        }
        // 깊이 우선 (Kids 순서 유지)
        std::vector<std::pair<ObjPtr, int>> work;
        if (pagesRoot) work.push_back({pagesRoot, 0});
        std::vector<std::pair<ObjPtr, int>> order;
        std::size_t guard = 0;
        while (!work.empty() && guard++ < 10000) {
            auto cur = work.front();
            work.erase(work.begin());
            auto t = doc.dget(cur.first, "Type");
            if (t && t->type == Obj::Name && t->str == "Page") {
                order.push_back(cur);
                continue;
            }
            auto kids = doc.dget(cur.first, "Kids");
            if (!kids || kids->type != Obj::Arr) continue;
            std::vector<std::pair<ObjPtr, int>> sub;
            for (auto& k : kids->arr) {
                const int kn = k->type == Obj::Ref ? k->refNum : 0;
                auto ko = doc.res(k);
                if (ko && ko->isDict() && !seen.count(kn)) {
                    seen.insert(kn);
                    sub.push_back({ko, kn});
                }
            }
            work.insert(work.begin(), sub.begin(), sub.end());
        }
        for (auto& p : order) pages.push_back({p.second, p.first});

        if (pages.empty()) { // 폴백
            for (auto& kv : doc.offsets) {
                auto o = doc.get(kv.first);
                if (!o || !o->isDict()) continue;
                auto t = o->dict.find("Type");
                if (t != o->dict.end() && t->second->type == Obj::Name && t->second->str == "Page")
                    pages.push_back({kv.first, o});
            }
        }
    }
    res.pages = (int)pages.size();
    if (pages.empty()) {
        res.error = "PDF에서 페이지를 찾지 못했습니다.";
        return res;
    }

    // 페이지별 해석
    std::vector<Staff> allStaves; // 오선보(5줄) + 타브(6줄)
    std::vector<Staff> tabStaves;
    std::vector<PageContent> pageContents;
    int pageIdx = 0;
    for (auto& pg : pages) {
        // MediaBox (상속)
        double pageH = 842;
        {
            ObjPtr cur = pg.second;
            for (int depth = 0; depth < 16 && cur; ++depth) {
                auto mb = doc.dget(cur, "MediaBox");
                if (mb && mb->type == Obj::Arr && mb->arr.size() == 4) {
                    auto y0 = doc.res(mb->arr[1]);
                    auto y1 = doc.res(mb->arr[3]);
                    if (y0 && y1 && y0->isNum() && y1->isNum())
                        pageH = std::fabs(y1->num - y0->num);
                    break;
                }
                cur = doc.dget(cur, "Parent");
            }
        }
        // Resources (상속)
        ObjPtr resources;
        {
            ObjPtr cur = pg.second;
            for (int depth = 0; depth < 16 && cur; ++depth) {
                auto r = doc.dget(cur, "Resources");
                if (r && r->isDict()) {
                    resources = r;
                    break;
                }
                cur = doc.dget(cur, "Parent");
            }
        }
        std::map<std::string, FontInfo> fonts;
        buildFonts(doc, resources, fonts);

        // 콘텐츠 이어붙이기
        std::string content;
        auto contentsRef = pg.second->dict.find("Contents");
        if (contentsRef != pg.second->dict.end()) {
            std::vector<ObjPtr> refs;
            if (contentsRef->second->type == Obj::Ref) {
                refs.push_back(contentsRef->second);
            } else {
                auto c = doc.res(contentsRef->second);
                if (c && c->type == Obj::Arr)
                    for (auto& e : c->arr) refs.push_back(e);
            }
            for (auto& r : refs) {
                const int rn = r->type == Obj::Ref ? r->refNum : 0;
                auto so = doc.res(r);
                if (!so || !so->hasStream) continue;
                content += doc.streamData(so, rn);
                content += "\n";
            }
        }
        PageContent pc;
        pc.height = pageH;
        if (!content.empty()) {
            runContent(doc, content, fonts, pageH, pc);
            // 오선보(5줄)도 함께 모은다. 기타가 쉬는 단에는 타브 보표가 아예 없는데,
            // 그 단의 마디 수를 세지 않으면 그만큼 곡이 짧아져 뒤가 전부 당겨진다.
            for (auto& s : findStaves(pc, pageIdx)) allStaves.push_back(s);
        }
        pageContents.push_back(std::move(pc));
        ++pageIdx;
    }
    for (const auto& s : allStaves)
        if (s.ys.size() == 6) tabStaves.push_back(s);

    res.tabStaves = (int)tabStaves.size();
    if (tabStaves.empty()) {
        res.error = "이 PDF에서 6줄 타브 보표를 찾지 못했습니다. "
                    "스캔(사진) 악보이거나 타브가 없는 악보일 수 있습니다.";
        return res;
    }

    // 보표를 (페이지, y) 순으로
    std::sort(allStaves.begin(), allStaves.end(), [](const Staff& a, const Staff& b) {
        return a.page != b.page ? a.page < b.page : a.ys.front() < b.ys.front();
    });

    // 템포 표기 (♩ = 164): 첫 보표보다 위에 있는 "= 숫자".
    // "= 164"가 한 덩어리 텍스트일 수도, '='와 숫자가 따로일 수도 있다.
    if (!allStaves.empty()) {
        const Staff& first = allStaves.front();
        const PageContent& pc0 = pageContents[(std::size_t)first.page];
        auto tryTempo = [&](int v) {
            if (v >= 30 && v <= 300 && res.tempoBpm == 0) res.tempoBpm = v;
        };
#ifdef PDFTAB_DEBUG
        std::fprintf(stderr, "[템포] 첫보표 top=%.1f page=%d\n", first.ys.front(), first.page);
        for (const auto& g : pc0.glyphs)
            if (g.text.find('=') != std::string::npos)
                std::fprintf(stderr, "  '=' 글리프 \"%s\" x=%.1f y=%.1f\n", g.text.c_str(), g.x,
                             g.y);
        for (const auto& g : pc0.glyphs)
            if (g.x > 55 && g.x < 140 && g.y > 175 && g.y < 215)
                std::fprintf(stderr, "  근처 글리프 \"%s\" x=%.1f y=%.1f code=%d\n",
                             g.text.c_str(), g.x, g.y, g.code);
#endif
        for (const auto& g : pc0.glyphs) {
            // 첫 보표 위쪽만 — 단, ♩= 글자가 보표 상단에 살짝 걸치기도 한다
            if (g.y > first.ys.front() + 4.0) continue;
            const std::size_t eq = g.text.find('=');
            if (eq != std::string::npos) {
                // 같은 덩어리 안의 "= 164"
                std::size_t i = eq + 1;
                while (i < g.text.size() && g.text[i] == ' ') ++i;
                if (i < g.text.size() && std::isdigit((uint8_t)g.text[i]))
                    tryTempo(std::atoi(g.text.c_str() + i));
                // '='만 따로 있으면 오른쪽의 숫자 글리프들을 이어붙인다
                // ("100"이 '1','0','0' 세 글자로 따로 그려지는 악보도 있다)
                std::vector<std::pair<double, std::string>> run;
                for (const auto& d : pc0.glyphs) {
                    // 앞 공백은 무시하고 숫자로 시작하는지 본다 (" 100"처럼 그려진다)
                    std::size_t s0 = 0;
                    while (s0 < d.text.size() && d.text[s0] == ' ') ++s0;
                    if (s0 >= d.text.size() || !std::isdigit((uint8_t)d.text[s0])) continue;
                    if (std::fabs(d.y - g.y) > 12.0) continue;
                    const double dx = d.x - g.x;
                    if (dx < 1.0 || dx > 60.0) continue;
                    run.push_back({d.x, d.text.substr(s0)});
                }
                std::sort(run.begin(), run.end());
                std::string numStr;
                double lastX = -1e9;
                for (const auto& rgl : run) {
                    if (!numStr.empty() && rgl.first - lastX > 10.0) break;
                    numStr += rgl.second;
                    lastX = rgl.first;
                }
#ifdef PDFTAB_DEBUG
                std::fprintf(stderr, "  '=' x=%.1f y=%.1f -> 숫자런 %d개, numStr=\"%s\"\n", g.x,
                             g.y, (int)run.size(), numStr.c_str());
#endif
                if (!numStr.empty()) tryTempo(std::atoi(numStr.c_str()));
            }
            if (res.tempoBpm > 0) break;
        }
    }

    // 단(시스템) 나누기: 한 단에 속한 보표들은 "마디선이 위아래로 관통"한다.
    // 보표 간격만으로 짐작하면(악보마다 여백이 제각각이라) 파트 수를 크게 틀린다.
    // 두 보표를 한 번에 지나는 세로선이 있으면 같은 단이다.
    auto sameSystem = [&](const Staff& a, const Staff& b) {
        if (a.page != b.page) return false;
        const PageContent& pc = pageContents[(std::size_t)a.page];
        const double top = std::min(a.ys.front(), b.ys.front());
        const double bot = std::max(a.ys.back(), b.ys.back());
        const double x0 = std::min(a.x0, b.x0), x1 = std::max(a.x1, b.x1);
        for (const auto& v : pc.vlines) {
            if (v.x < x0 - 8.0 || v.x > x1 + 8.0) continue;
            if (v.y0 > top + 2.0 || v.y1 < bot - 2.0) continue; // 두 보표를 다 덮어야 한다
            // 페이지 테두리처럼 페이지를 통째로 가로지르는 선만 제외한다.
            // 단 연결선은 멜로디+기타1+기타2를 다 지나면 300pt 가까이 되므로
            // "보표 기준 여유"로 자르면 보표가 많은 단이 통째로 쪼개진다.
            if (v.y1 - v.y0 > 480.0) continue;
            return true;
        }
        return false;
    };

    // 오선보까지 포함해 단을 묶는다 (기타가 쉬는 단도 마디 수를 세야 하므로)
    std::vector<std::vector<const Staff*>> allSystems;
    for (std::size_t i = 0; i < allStaves.size(); ++i) {
        const bool newSystem =
            allSystems.empty() || !sameSystem(*allSystems.back().back(), allStaves[i]);
        if (newSystem) allSystems.push_back({});
        allSystems.back().push_back(&allStaves[i]);
    }

    // 단마다 타브 보표만 골라낸다 (없으면 그 단은 기타가 쉬는 단 = 마디만 흘려보낸다).
    // 같은 단의 타브들 사이에는 오선보가 끼어 멀리 떨어져 있을 수 있으므로,
    // 거리로 다시 거르면 안 된다 — 단 묶기는 위의 sameSystem(마디선 관통)이 책임진다.
    std::vector<std::vector<const Staff*>> systems;
    for (const auto& band : allSystems) {
        std::vector<const Staff*> tabs;
        for (const Staff* s : band)
            if (s->ys.size() == 6) tabs.push_back(s);
        systems.push_back(std::move(tabs));
    }

    // 이 단이 몇 마디인가: 보표를 관통하는 마디선 개수 (앞쪽 음자리표 구역은 뺀다).
    // 타브 보표가 있으면 타브에서만 센다 — 오선보는 음표 기둥이 보표를 위아래로
    // 정확히 관통하는 경우가 흔해서(한 옥타브 음) 마디 수가 크게 부풀 수 있다.
    auto systemMeasures = [&](const std::vector<const Staff*>& sysAll) -> uint32_t {
        std::vector<const Staff*> sys;
        for (const Staff* s : sysAll)
            if (s->ys.size() == 6) sys.push_back(s);
        if (sys.empty()) sys = sysAll;
        uint32_t best = 0;
        for (const Staff* s : sys) {
            const PageContent& pc = pageContents[(std::size_t)s->page];
            const double top = s->ys.front(), bot = s->ys.back();
            const double g = (bot - top) / (double)(s->ys.size() - 1);
            std::vector<double> bs;
            for (const auto& v : pc.vlines) {
                if (v.x < s->x0 - 1.0 || v.x > s->x1 + 2.0) continue;
                // 마디선은 보표를 위아래로 "딱 맞게" 관통한다. 오선보의 음표 기둥도
                // 보표 높이만큼 길어서, 여유를 두면 기둥이 마디선으로 둔갑한다.
                if (v.y0 > top + 1.2 || v.y1 < bot - 1.2) continue;
                bs.push_back(v.x);
            }
            std::sort(bs.begin(), bs.end());
            bs.erase(std::unique(bs.begin(), bs.end(),
                                 [](double a, double b) { return std::fabs(a - b) < 9.0; }),
                     bs.end());
            // 보표 앞머리(음자리표·조표·박자표·반복기호)의 세로줄은 마디를 나누지 않는다
            std::size_t n = 0;
            for (double b : bs)
                if (b > s->x0 + 45.0) ++n;
            best = std::max(best, (uint32_t)n);
        }
        return best;
    };

    // 인쇄된 마디 번호: 단 맨 앞(보표 왼쪽 끝) 위에 붙는 작은 숫자.
    // 이걸 읽으면 마디를 셀 필요 없이 각 단의 시작 마디를 그대로 알 수 있다 —
    // 오선보의 음표 기둥을 마디선으로 착각하는 문제에서 자유롭다.
    auto systemBarNumber = [&](const std::vector<const Staff*>& sys) -> int {
        const Staff* topS = nullptr;
        for (const Staff* s : sys)
            if (!topS || s->ys.front() < topS->ys.front()) topS = s;
        if (!topS) return -1;
        const PageContent& pc = pageContents[(std::size_t)topS->page];
        const double stTop = topS->ys.front();
        // "10"이 '1','0' 두 글리프로 쪼개져 있기도 하다 — 모아서 이어붙인다
        std::vector<std::pair<double, std::string>> run;
        for (const auto& d : pc.digits) {
            if (d.size < 6.0 || d.size > 12.0) continue;      // 마디 번호 크기
            if (d.x > topS->x0 + 16.0) continue;              // 보표 왼쪽 끝 바깥
            if (d.y > stTop - 1.0 || d.y < stTop - 26.0) continue; // 보표 바로 위
            run.push_back({d.x, d.text});
        }
        if (run.empty()) return -1;
        std::sort(run.begin(), run.end());
        std::string num;
        double lastX = -1e9;
        for (const auto& r : run) {
            if (!num.empty() && r.first - lastX > 8.0) break; // 떨어진 숫자는 다른 것
            num += r.second;
            lastX = r.first;
        }
        const int n = std::atoi(num.c_str());
        return (n >= 1 && n <= 999) ? n : -1;
    };

    std::size_t nParts = 1;
    for (auto& s : systems) nParts = std::max(nParts, s.size());

    res.parts.assign(nParts, TabPdfPart());
    int totalStems = 0;
    // 도돌이표 표시들 (틱, 시작인가?) — 한 단의 두 보표에 같은 표시가 있으므로 나중에 중복 제거
    std::vector<std::pair<uint32_t, bool>> repeatMarks;
    // 마디 반복 기호(𝄎)로 채워야 할 마디들 — 파트마다 따로
    std::vector<std::vector<std::pair<uint32_t, int>>> partFills(nParts);
    // 이 악보에서 마디 반복 기호가 어떤 글리프인지 먼저 알아낸다 (폰트마다 다르다)
    const int repeatGlyph = findMeasureRepeatGlyph(tabStaves, pageContents);
    // 같은 단의 파트들은 동시에 연주되므로 반드시 같은 시각에서 시작해야 한다.
    uint32_t systemTick = 0;
    // 박자표는 바뀌는 곳에만 적힌다 — 못 찾으면 직전 박자를 이어간다 (기본 4/4).
    int tsNum = 4, tsDen = 4;
    int lastPrintedBar = 0; // 마지막으로 받아들인 인쇄 마디 번호 (단조 증가 검증)
    for (std::size_t si = 0; si < systems.size(); ++si) {
        auto& sys = systems[si];
        {
            // 인쇄된 마디 번호가 있으면 그 자리에 못 박는다 (누적 오차가 아예 없다).
            // 단, 마디 번호는 문서 순서상 반드시 커진다 — 프렛/운지 숫자를 마디 번호로
            // 잘못 읽으면 단이 뒤로 앉아 기존 마디 위에 겹치므로, 줄어드는 번호는 버린다.
            const uint32_t bt = std::max<uint32_t>(
                1, (uint32_t)((uint64_t)ppqn * 4 * (uint32_t)tsNum / (uint32_t)tsDen));
            const int bn = systemBarNumber(allSystems[si]);
            // 마디 세기는 거의 정확하므로, 인쇄 번호는 "세어온 위치 근처(±4마디)"일 때만
            // 미세 보정으로 받아들인다. 멀리 떨어진 숫자는 프렛/운지 등을 잘못 읽은 것.
            const long cursorBar = (long)(systemTick / bt);
            if (bn >= 1 && bn > lastPrintedBar && std::labs((long)(bn - 1) - cursorBar) <= 4) {
                systemTick = (uint32_t)(bn - 1) * bt;
                lastPrintedBar = bn;
            }
        }
        // 한 단 안에서는 어느 보표에 적혀 있든 같은 박자다
        for (std::size_t p = 0; p < sys.size(); ++p) {
            const Staff& st = *sys[p];
            const PageContent& pc = pageContents[(std::size_t)st.page];
            const double fs = fretDigitSize(st, pc.digits);
            int n = 0, d = 0;
            if (detectTimeSig(st, pc.digits, fs, n, d)) {
                tsNum = n;
                tsDen = d;
                break;
            }
        }
        if (res.timeSigNum == 0) {
            res.timeSigNum = tsNum;
            res.timeSigDen = tsDen;
        }
        for (std::size_t p = 0; p < sys.size(); ++p) {
            const Staff& st = *sys[p];
            const PageContent& pc = pageContents[(std::size_t)st.page];
            const double fs = fretDigitSize(st, pc.digits);
            const auto cols = staffColumns(st, pc.digits, fs);
            res.parts[p].ascii += colsToAscii(cols);
            res.parts[p].ascii += "\n";

            const Rhythm r =
                staffRhythm(st, cols, pc, ppqn, systemTick, tsNum, tsDen, repeatGlyph);
            totalStems += r.stems;
            for (const auto& n : r.notes) res.parts[p].notes.push_back(n);
            for (uint32_t t : r.repeatStarts) repeatMarks.push_back({t, true});
            for (uint32_t t : r.repeatEnds) repeatMarks.push_back({t, false});
            // 마디 반복 기호는 파트마다 따로다 (기타1만 반복인 경우도 있다)
            for (const auto& mr : r.measureRepeats) partFills[p].push_back(mr);
        }
        // 이 단의 길이 = 단 전체(오선보 포함)의 마디 수. 타브가 없는 단(기타가 쉬는 단)도
        // 그만큼 시간이 흘러야 뒤의 마디가 제자리에 온다.
        const uint32_t barTicks = std::max<uint32_t>(
            1, (uint32_t)((uint64_t)ppqn * 4 * (uint32_t)tsNum / (uint32_t)tsDen));
        uint32_t measures = systemMeasures(allSystems[si]);
        if (measures == 0) measures = systemMeasures(sys); // 폴백
#ifdef PDFTAB_DEBUG
        std::fprintf(stderr, "[단 %d] p%d 보표%d(타브%d) 마디%u 인쇄번호=%d -> 시작마디 %u\n",
                     (int)si, allSystems[si].front()->page + 1, (int)allSystems[si].size(),
                     (int)sys.size(), measures, systemBarNumber(allSystems[si]),
                     systemTick / barTicks + 1);
#endif
        systemTick += measures * barTicks;
    }
    const uint32_t songEnd = systemTick;

    // ── 마디 반복 기호(𝄎) 채우기 ──
    // "앞 N마디를 그대로" 라는 뜻이므로, 앞 N마디를 그대로 복사해 붙인다.
    // 앞에서부터 순서대로 처리해야 (반복의 반복도) 제대로 채워진다.
    {
        const uint32_t barTicks =
            (uint32_t)((uint64_t)ppqn * 4 * (uint32_t)std::max(1, res.timeSigNum) /
                       (uint32_t)std::max(1, res.timeSigDen));
        for (std::size_t p = 0; p < res.parts.size(); ++p) {
            auto& fills = partFills[p];
            std::sort(fills.begin(), fills.end());
            fills.erase(std::unique(fills.begin(), fills.end()), fills.end());
            auto& notes = res.parts[p].notes;
            for (const auto& f : fills) {
                const uint32_t dstStart = f.first;
                const uint32_t len = (uint32_t)f.second * barTicks;
                if (dstStart < len) continue;
                const uint32_t srcStart = dstStart - len;
                std::vector<TabPdfNote> add;
                for (const auto& n : notes)
                    if (n.tick >= srcStart && n.tick < srcStart + len)
                        add.push_back(
                            {n.tick + len, n.note, n.durTicks, n.strIdx, n.vel, n.artic});
                // 채울 자리에 이미 뭔가 있으면(오검출) 덮어쓰지 않는다
                bool occupied = false;
                for (const auto& n : notes)
                    if (n.tick >= dstStart && n.tick < dstStart + len) occupied = true;
#ifdef PDFTAB_DEBUG
                std::fprintf(stderr, "  [채우기] 파트%d 마디%u<-%u (%d마디) 베낄노트=%d 이미참=%d\n",
                             (int)p, dstStart / barTicks + 1, srcStart / barTicks + 1, f.second,
                             (int)add.size(), (int)occupied);
#endif
                if (occupied || add.empty()) continue;
                notes.insert(notes.end(), add.begin(), add.end());
                std::stable_sort(notes.begin(), notes.end(),
                                 [](const TabPdfNote& a, const TabPdfNote& b) {
                                     return a.tick < b.tick;
                                 });
                ++res.measureRepeats;
            }
        }
    }

    // ── 도돌이표 펼치기 ──
    // 같은 단의 두 보표에 같은 표시가 찍히므로 중복을 없애고, 시작:끝 짝을 지어
    // "연주 순서"를 만든 뒤 모든 파트를 같은 순서로 다시 깐다 (파트가 어긋나면 안 된다).
    {
        std::sort(repeatMarks.begin(), repeatMarks.end());
        repeatMarks.erase(std::unique(repeatMarks.begin(), repeatMarks.end()),
                          repeatMarks.end());

        std::vector<std::pair<uint32_t, uint32_t>> regions; // 두 번 연주할 구간
        uint32_t curStart = 0;
        bool haveStart = false;
        for (const auto& m : repeatMarks) {
            if (m.second) {
                curStart = m.first;
                haveStart = true;
            } else {
                // 시작 표시가 없는 반복 끝 = 곡 처음부터 되돌아간다 (악보 관례)
                const uint32_t s = haveStart ? curStart : 0;
                if (m.first > s) regions.push_back({s, m.first});
                haveStart = false;
            }
        }

#ifdef PDFTAB_DEBUG
        for (const auto& m : repeatMarks)
            std::fprintf(stderr, "  [도돌이] %s 마디 %u\n", m.second ? "시작" : "끝  ",
                         m.first / (ppqn * 4) + 1);
        for (const auto& rg : regions)
            std::fprintf(stderr, "  [반복구간] 마디 %u ~ %u\n", rg.first / (ppqn * 4) + 1,
                         rg.second / (ppqn * 4));
        std::fprintf(stderr, "  [곡끝] 마디 %u\n", songEnd / (ppqn * 4));
#endif
        if (!regions.empty() && songEnd > 0) {
            // 연주 순서: [0..끝1] [시작1..끝1] [끝1..끝2] [시작2..끝2] ... [마지막끝..곡끝]
            std::vector<std::pair<uint32_t, uint32_t>> segs;
            uint32_t pos = 0;
            for (const auto& rg : regions) {
                if (rg.second <= pos) continue; // 이미 지난 구간(겹침) 은 건너뛴다
                if (pos < rg.second) segs.push_back({pos, rg.second});
                segs.push_back({std::max(rg.first, (uint32_t)0), rg.second}); // 반복
                pos = rg.second;
            }
            if (pos < songEnd) segs.push_back({pos, songEnd});

            for (auto& part : res.parts) {
                std::vector<TabPdfNote> out;
                uint32_t outPos = 0;
                for (const auto& sg : segs) {
                    for (const auto& n : part.notes)
                        if (n.tick >= sg.first && n.tick < sg.second)
                            out.push_back({outPos + (n.tick - sg.first), n.note, n.durTicks,
                                           n.strIdx, n.vel, n.artic});
                    outPos += sg.second - sg.first;
                }
                std::stable_sort(out.begin(), out.end(),
                                 [](const TabPdfNote& a, const TabPdfNote& b) {
                                     return a.tick < b.tick;
                                 });
                part.notes = std::move(out);
            }
            res.repeats = (int)regions.size();
        }
    }

    // 같은 시각에 같은 음이 두 번 들어가면 그 음만 두 배로 커진다 — 한 번만 남긴다.
    for (auto& p : res.parts) {
        std::sort(p.notes.begin(), p.notes.end(), [](const TabPdfNote& a, const TabPdfNote& b) {
            if (a.tick != b.tick) return a.tick < b.tick;
            if (a.note != b.note) return a.note < b.note;
            return a.durTicks > b.durTicks; // 겹치면 긴 쪽을 남긴다
        });
        p.notes.erase(std::unique(p.notes.begin(), p.notes.end(),
                                  [](const TabPdfNote& a, const TabPdfNote& b) {
                                      return a.tick == b.tick && a.note == b.note;
                                  }),
                      p.notes.end());
    }

    // 기둥을 충분히 읽었을 때만 리듬을 신뢰한다 (리듬 표기 없는 타브책도 있다)
    for (const auto& p : res.parts)
        if (!p.notes.empty()) res.hasRhythm = true;
    if (totalStems < 4) {
        res.hasRhythm = false;
        for (auto& p : res.parts) p.notes.clear();
    }
    return res;
}

} // namespace midipro::pdf

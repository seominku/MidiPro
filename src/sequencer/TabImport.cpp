// =============================================================
// MidiPro - sequencer/TabImport.cpp
// ASCII 타브 파서 구현. 형식 설명은 TabImport.h 참고.
// =============================================================

#include "sequencer/TabImport.h"

#include <algorithm>
#include <cctype>

namespace midipro::seq {

namespace {
constexpr int kOpen[6] = {64, 59, 55, 50, 45, 40}; // 위(1번줄 e)부터 표준 튜닝

// 타브 줄처럼 생겼는가: '-'가 여러 개 있고, 내용 문자가 타브 기호 위주다
bool looksLikeTabLine(const std::string& s) {
    int dashes = 0, content = 0;
    for (char c : s) {
        if (c == '-') ++dashes;
        if (!std::isspace((unsigned char)c)) ++content;
    }
    return dashes >= 4 && content >= 6;
}

// 내용 시작 위치: 첫 '|' 뒤 (줄 라벨 "e|" 등을 건너뜀). 없으면 첫 '-' 위치.
std::size_t contentStart(const std::string& s) {
    const std::size_t bar = s.find('|');
    const std::size_t dash = s.find('-');
    if (bar != std::string::npos && (dash == std::string::npos || bar < dash + 3))
        return bar + 1;
    return dash == std::string::npos ? 0 : dash;
}
} // namespace

std::vector<TabNote> parseAsciiTab(const std::string& text, uint32_t stepTicks) {
    std::vector<TabNote> out;
    if (stepTicks == 0) stepTicks = 1;

    // 줄 나누기
    std::vector<std::string> lines;
    {
        std::string cur;
        for (char c : text) {
            if (c == '\n') {
                lines.push_back(cur);
                cur.clear();
            } else if (c != '\r') {
                cur += c;
            }
        }
        lines.push_back(cur);
    }

    uint32_t blockBase = 0; // 이번 블록이 시작하는 틱 (블록들은 시간상 이어진다)
    for (std::size_t li = 0; li + 5 < lines.size();) {
        // 연속 6줄이 모두 타브 줄이면 하나의 블록
        bool isBlock = true;
        for (int s = 0; s < 6; ++s)
            if (!looksLikeTabLine(lines[li + (std::size_t)s])) {
                isBlock = false;
                break;
            }
        if (!isBlock) {
            ++li;
            continue;
        }

        // 각 줄의 내용 구간을 정렬해 같은 열 = 같은 시각으로 본다
        std::string rows[6];
        std::size_t minLen = SIZE_MAX;
        for (int s = 0; s < 6; ++s) {
            const std::string& src = lines[li + (std::size_t)s];
            rows[s] = src.substr(std::min(contentStart(src), src.size()));
            minLen = std::min(minLen, rows[s].size());
        }
        if (minLen == SIZE_MAX || minLen == 0) {
            li += 6;
            continue;
        }

        uint32_t col2tick = 0; // 이 블록 안에서 진행된 틱
        for (std::size_t c = 0; c < minLen; ++c) {
            // 세로 마디선 열(대부분 '|')은 시간을 진행시키지 않는다
            int bars = 0;
            for (int s = 0; s < 6; ++s)
                if (rows[s][c] == '|') ++bars;
            if (bars >= 4) continue;

            // 두 자리 프렛의 뒷자리만 있는 열(예: "12"의 '2')은 시각을 진행시키지 않는다.
            // 그래야 12가 5보다 한 칸 더 길게 울리는 일이 없다.
            bool anyNew = false, anyCont = false;
            for (int s = 0; s < 6; ++s) {
                if (!std::isdigit((unsigned char)rows[s][c])) continue;
                if (c > 0 && std::isdigit((unsigned char)rows[s][c - 1]))
                    anyCont = true;
                else
                    anyNew = true;
            }
            if (!anyNew && anyCont) continue;

            for (int s = 0; s < 6; ++s) {
                const char ch = rows[s][c];
                if (!std::isdigit((unsigned char)ch)) continue;
                if (c > 0 && std::isdigit((unsigned char)rows[s][c - 1]))
                    continue; // 두 자리 프렛의 둘째 자리
                int fret = ch - '0';
                if (c + 1 < rows[s].size() && std::isdigit((unsigned char)rows[s][c + 1])) {
                    const int two = fret * 10 + (rows[s][c + 1] - '0');
                    if (two <= 24) fret = two; // 25 이상은 한 자리씩 취급
                }
                const int note = kOpen[s] + fret;
                if (note < 0 || note > 127) continue;
                out.push_back({blockBase + col2tick, (uint8_t)note, stepTicks, (int8_t)s});
            }
            col2tick += stepTicks;
        }
        blockBase += col2tick; // 다음 블록(단)은 이어서
        li += 6;
    }

    std::stable_sort(out.begin(), out.end(),
                     [](const TabNote& a, const TabNote& b) { return a.tick < b.tick; });
    return out;
}

} // namespace midipro::seq

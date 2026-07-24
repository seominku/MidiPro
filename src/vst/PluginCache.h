#pragma once
// =============================================================
// MidiPro - vst/PluginCache.h
// "이 .vst3에 악기 클래스가 있나 / 이펙트 클래스가 있나"를 파일에 기억해 둔다.
//
// 왜 필요한가:
//   그 판정은 플러그인 DLL을 실제로 열어 봐야 알 수 있다. Omnisphere·Trilian
//   같은 큰 플러그인은 여는 데만 수 초가 걸리고, 설치된 플러그인이 많으면
//   트랙의 [+]를 처음 누를 때마다 수십 초를 기다리게 된다. 프로그램을 껐다
//   켤 때마다 반복되므로 결과를 디스크에 남겨 둔다.
//
// 무효화:
//   파일 크기와 수정 시각을 함께 저장해, 플러그인을 업데이트하면 그 항목만
//   자동으로 다시 조사한다. 사용자가 직접 전체를 지울 수도 있다(다시 검색).
//
// 저장 위치: %LOCALAPPDATA%\MidiPro\plugincache.ini
// =============================================================

#include <cstdint>
#include <map>
#include <string>

namespace midipro::vst {

struct PluginKinds {
    bool hasInstrument = false;
    bool hasEffect = false;
};

class PluginCache {
public:
    void load(const std::string& file);
    void save(const std::string& file) const;

    // 캐시에 쓸 만한 값이 있으면 out에 채우고 true. 없거나 파일이 바뀌었으면 false.
    bool lookup(const std::string& path, PluginKinds& out) const;
    void store(const std::string& path, const PluginKinds& kinds);

    void clear() { m_entries.clear(); }
    bool dirty() const { return m_dirty; }
    std::size_t size() const { return m_entries.size(); }

private:
    struct Entry {
        std::uint64_t size = 0;  // 번들 대표 파일 크기
        std::int64_t mtime = 0;  // 마지막 수정 시각
        PluginKinds kinds;
    };
    std::map<std::string, Entry> m_entries;
    mutable bool m_dirty = false;
};

} // namespace midipro::vst

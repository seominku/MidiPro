// =============================================================
// MidiPro - vst/PluginCache.cpp
// =============================================================

#include "vst/PluginCache.h"

#include "core/PathUtf8.h"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace midipro::vst {

namespace {

// .vst3는 폴더(번들)일 수도, 단일 파일일 수도 있다. 번들이면 안쪽 실제 DLL을
// 기준으로 삼아야 업데이트를 알아챌 수 있으므로, 가장 큰 파일 하나를 대표로 쓴다.
bool stampOf(const std::string& path, std::uint64_t& size, std::int64_t& mtime) {
    std::error_code ec;
    const fs::path p = core::pathFromUtf8(path);
    if (!fs::exists(p, ec)) return false;

    if (fs::is_directory(p, ec)) {
        std::uint64_t best = 0;
        std::int64_t bestT = 0;
        fs::recursive_directory_iterator it(p, fs::directory_options::skip_permission_denied, ec);
        const fs::recursive_directory_iterator end;
        for (; !ec && it != end; it.increment(ec)) {
            if (ec) { ec.clear(); continue; }
            if (!it->is_regular_file(ec)) continue;
            const auto sz = (std::uint64_t)fs::file_size(it->path(), ec);
            if (ec) { ec.clear(); continue; }
            if (sz > best) {
                best = sz;
                const auto t = fs::last_write_time(it->path(), ec);
                bestT = ec ? 0 : (std::int64_t)t.time_since_epoch().count();
                ec.clear();
            }
        }
        if (best == 0) return false;
        size = best;
        mtime = bestT;
        return true;
    }

    size = (std::uint64_t)fs::file_size(p, ec);
    if (ec) return false;
    const auto t = fs::last_write_time(p, ec);
    mtime = ec ? 0 : (std::int64_t)t.time_since_epoch().count();
    return true;
}

} // namespace

void PluginCache::load(const std::string& file) {
    m_entries.clear();
    m_dirty = false;
    std::ifstream f(core::pathFromUtf8(file));
    if (!f) return;

    std::string line;
    if (!std::getline(f, line) || line.rfind("midipro_plugincache", 0) != 0) return;

    // 형식: <size> <mtime> <inst> <fx> <경로(줄 끝까지, 공백 허용)>
    while (std::getline(f, line)) {
        std::istringstream ls(line);
        Entry e;
        int inst = 0, fx = 0;
        if (!(ls >> e.size >> e.mtime >> inst >> fx)) continue;
        std::string path;
        std::getline(ls, path);
        while (!path.empty() && (path.front() == ' ' || path.front() == '\t'))
            path.erase(path.begin());
        if (path.empty()) continue;
        e.kinds.hasInstrument = inst != 0;
        e.kinds.hasEffect = fx != 0;
        m_entries[path] = e;
    }
}

void PluginCache::save(const std::string& file) const {
    std::ofstream f(core::pathFromUtf8(file), std::ios::trunc);
    if (!f) return;
    f << "midipro_plugincache 1\n";
    for (const auto& [path, e] : m_entries)
        f << e.size << ' ' << e.mtime << ' ' << (e.kinds.hasInstrument ? 1 : 0) << ' '
          << (e.kinds.hasEffect ? 1 : 0) << ' ' << path << '\n';
    m_dirty = false;
}

bool PluginCache::lookup(const std::string& path, PluginKinds& out) const {
    const auto it = m_entries.find(path);
    if (it == m_entries.end()) return false;

    std::uint64_t size = 0;
    std::int64_t mtime = 0;
    if (!stampOf(path, size, mtime)) return false; // 파일이 사라졌다 -> 다시 조사
    if (size != it->second.size || mtime != it->second.mtime) return false; // 업데이트됨

    out = it->second.kinds;
    return true;
}

void PluginCache::store(const std::string& path, const PluginKinds& kinds) {
    Entry e;
    e.kinds = kinds;
    stampOf(path, e.size, e.mtime); // 실패해도 저장은 한다(다음에 다시 조사될 뿐)
    m_entries[path] = e;
    m_dirty = true;
}

} // namespace midipro::vst

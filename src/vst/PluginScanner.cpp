// =============================================================
// MidiPro - vst/PluginScanner.cpp
// =============================================================

#include "vst/PluginScanner.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <set>

namespace fs = std::filesystem;

namespace midipro::vst {

namespace {

// 환경변수 경로에 하위 폴더를 붙여 후보 루트를 만든다. 없으면 빈 문자열.
std::string envPath(const char* var, const char* sub) {
    const char* base = std::getenv(var);
    if (!base || !*base) return {};
    return (fs::path(base) / sub).string();
}

// 한 루트를 재귀 탐색하되, .vst3 항목을 만나면 그 안으로는 안 들어간다.
// (번들 내부의 Contents\...\X.vst3 를 중복 수집하지 않기 위함)
void scanRoot(const std::string& root, std::vector<PluginEntry>& out,
              std::set<std::string>& seen) {
    std::error_code ec;
    if (root.empty() || !fs::exists(root, ec)) return;

    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
    const fs::recursive_directory_iterator end;
    for (; it != end; it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        const fs::path& p = it->path();
        if (p.extension() == ".vst3") {
            it.disable_recursion_pending(); // 번들 내부로 내려가지 않음
            std::string full = p.string();
            if (seen.insert(full).second) {
                PluginEntry e;
                e.name = p.stem().string(); // 확장자 제외 이름
                e.path = std::move(full);
                out.push_back(std::move(e));
            }
        }
    }
}

} // namespace

std::vector<PluginEntry> scanVst3Plugins() {
    std::vector<PluginEntry> out;
    std::set<std::string> seen;

    scanRoot(envPath("CommonProgramFiles", "VST3"), out, seen);
    scanRoot(envPath("CommonProgramFiles(x86)", "VST3"), out, seen);
    scanRoot(envPath("LOCALAPPDATA", "Programs\\Common\\VST3"), out, seen);

    std::sort(out.begin(), out.end(),
              [](const PluginEntry& a, const PluginEntry& b) { return a.name < b.name; });
    return out;
}

} // namespace midipro::vst

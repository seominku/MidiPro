// VST3 SDK 빌드 검증 + 플러그인 열거 도구 (호스트 본구현 전 스모크)
#include "public.sdk/source/vst/hosting/module.h"

#include <windows.h>
#include <cstdio>
#include <string>

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    if (argc < 2) {
        std::printf("사용법: vst_probe <플러그인.vst3 경로 또는 VST3 폴더>\n");
        std::printf("[OK] VST3 SDK가 MSVC로 정상 빌드/링크되었습니다.\n");
        return 0;
    }

    std::string path = argv[1];
    std::string error;
    auto module = VST3::Hosting::Module::create(path, error);
    if (!module) {
        std::printf("모듈 로드 실패: %s\n  (%s)\n", path.c_str(), error.c_str());
        return 1;
    }

    const auto& factory = module->getFactory();
    auto info = factory.info();
    std::printf("모듈 로드 성공: %s\n  벤더: %s\n", path.c_str(), info.vendor().c_str());
    int n = 0;
    for (const auto& ci : factory.classInfos()) {
        std::printf("  [%d] %-32s  category=%s  subCategories=%s\n", n++, ci.name().c_str(),
                    ci.category().c_str(), ci.subCategoriesString().c_str());
    }
    std::printf("클래스 %d개\n", n);
    return 0;
}

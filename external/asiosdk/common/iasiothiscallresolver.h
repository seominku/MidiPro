#ifndef included_iasiothiscallresolver_h
#define included_iasiothiscallresolver_h
// =============================================================
// iasiothiscallresolver.h (MSVC용 빈 스텁)
//
// 원본은 MinGW/GCC에서 ASIO의 thiscall 호출 규약을 보정하기 위한
// PortAudio/RtAudio 헬퍼다. MSVC(_MSC_VER)에서는 보정이 필요 없고
// RtAudio가 ASIO SDK 함수를 직접 호출하므로 이 헤더는 비어 있어도 된다.
// (ASIO SDK 자체에는 포함돼 있지 않아 빌드용으로 여기 둔다.)
// =============================================================
#if !defined(_MSC_VER)
#error "iasiothiscallresolver.h stub는 MSVC 전용입니다. MinGW/GCC 빌드는 정식 파일이 필요합니다."
#endif
#endif // included_iasiothiscallresolver_h

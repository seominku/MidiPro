// =============================================================
// MidiPro - main.cpp (Phase 1: MIDI I/O 콘솔 테스트)
//
// 계층 규칙 (Rule 1):
//   이 파일은 조립 지점(composition root)이므로 유일하게
//   구체 타입(RtMidiInput/RtMidiOutput)을 생성한다.
//   생성 이후에는 IMidiInput/IMidiOutput 인터페이스로만 다룬다.
//
// 스레딩 (Rule 3):
//   수신 메시지는 구현체 내부의 실시간 콜백 -> 락프리 큐를 거쳐
//   이 메인 스레드에서 poll()로 꺼내 출력한다. 로깅은 항상
//   메인 스레드에서만 일어난다.
// =============================================================

#include "midi/IMidiDevice.h"
#include "midi/MidiConstants.h"
#include "midi/MidiMessage.h"
#include "midi/RtMidiDevice.h"

#include <windows.h>
#include <conio.h>

#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

using namespace midipro;
using midi::MidiMessage;

namespace {

void printDevices(midi::IMidiInput& input, midi::IMidiOutput& output) {
    const auto inPorts = input.listPorts();
    const auto outPorts = output.listPorts();

    std::cout << "\n--- MIDI 입력 장치 (" << inPorts.size() << "개) ---\n";
    if (inPorts.empty()) std::cout << "  (연결된 입력 장치 없음)\n";
    for (std::size_t i = 0; i < inPorts.size(); ++i)
        std::cout << "  [" << i << "] " << inPorts[i] << "\n";

    std::cout << "--- MIDI 출력 장치 (" << outPorts.size() << "개) ---\n";
    if (outPorts.empty()) std::cout << "  (연결된 출력 장치 없음)\n";
    for (std::size_t i = 0; i < outPorts.size(); ++i)
        std::cout << "  [" << i << "] " << outPorts[i] << "\n";
}

int askInt(const std::string& prompt, int defaultValue = 0) {
    std::cout << prompt;
    std::string line;
    std::getline(std::cin, line);
    if (line.empty()) return defaultValue;
    try {
        return std::stoi(line);
    } catch (...) {
        return defaultValue;
    }
}

// 노트 하나 재생 (Note On -> 대기 -> Note Off)
void playNote(midi::IMidiOutput& output, uint8_t note, int durationMs, uint8_t velocity = 100) {
    const auto on = MidiMessage::makeNoteOn(0, note, velocity);
    std::cout << "  송신: " << MidiMessage::parse(on).toString() << "\n";
    output.send(on);
    std::this_thread::sleep_for(std::chrono::milliseconds(durationMs));
    output.send(MidiMessage::makeNoteOff(0, note));
}

// 입력 모니터링: 큐에 쌓인 수신 메시지를 폴링해 출력. Enter로 종료.
void monitorInput(midi::IMidiInput& input) {
    std::cout << "입력 모니터링 중... (Enter를 누르면 메뉴로 돌아갑니다)\n";
    const std::size_t droppedBefore = input.droppedCount();

    for (;;) {
        MidiMessage message;
        bool printed = false;
        while (input.poll(message)) {
            std::cout << "  [IN] " << message.toString() << "\n";
            printed = true;
        }
        if (_kbhit()) {
            const int key = _getch();
            if (key == '\r' || key == '\n') break;
        }
        // 왜 sleep인가: 메인 스레드는 실시간이 아니므로 바쁜 대기로
        // CPU를 태울 이유가 없다. 1ms 폴링이면 표시용으로 충분하다.
        if (!printed) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    const std::size_t dropped = input.droppedCount() - droppedBefore;
    if (dropped > 0)
        std::cout << "(경고: 큐가 가득 차 " << dropped << "개 메시지를 버렸습니다)\n";
    std::cout << "모니터링을 종료했습니다.\n";
}

void printMenu() {
    std::cout << "\n----------- 메뉴 -----------\n"
              << " 1. 장치 목록 새로고침\n"
              << " 2. 입력 포트 열고 모니터링 (Enter로 중단)\n"
              << " 3. 입력 포트 닫기\n"
              << " 4. 출력 포트 열기\n"
              << " 5. 테스트 노트 전송 (C4)\n"
              << " 6. 기타 튜닝 기준음 (E2 A2 D3 G3 B3 E4)\n"
              << " 7. 프로그램 체인지 (악기 변경)\n"
              << " 8. CC 전송 테스트 (볼륨 CC7)\n"
              << " 0. 종료\n"
              << "선택 > ";
}

} // namespace

int main() {
    // 콘솔 한글(UTF-8) 출력 설정
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    std::cout << "=====================================\n"
              << "  MidiPro - Phase 1 (MIDI I/O 테스트)\n"
              << "=====================================\n";

    // 조립 지점: 구체 타입은 여기서만 만든다.
    std::unique_ptr<midi::IMidiInput> input = std::make_unique<midi::RtMidiInput>();
    std::unique_ptr<midi::IMidiOutput> output = std::make_unique<midi::RtMidiOutput>();

    printDevices(*input, *output);

    bool running = true;
    while (running) {
        printMenu();

        std::string line;
        if (!std::getline(std::cin, line)) break;
        int selection = -1;
        try {
            selection = std::stoi(line);
        } catch (...) {
            continue;
        }

        switch (selection) {
        case 1:
            printDevices(*input, *output);
            break;

        case 2: {
            const auto ports = input->listPorts();
            if (ports.empty()) {
                std::cout << "입력 장치가 없습니다. (키보드/컨트롤러를 연결하거나 "
                             "loopMIDI 같은 가상 포트를 사용하세요)\n";
                break;
            }
            const int port = askInt("입력 포트 번호 > ", 0);
            if (port < 0 || port >= (int)ports.size()) {
                std::cout << "잘못된 번호입니다.\n";
                break;
            }
            if (input->openPort((unsigned)port)) {
                std::cout << "입력 포트 열림: [" << port << "] " << ports[port] << "\n";
                monitorInput(*input);
            }
            break;
        }

        case 3:
            input->closePort();
            std::cout << "입력 포트를 닫았습니다.\n";
            break;

        case 4: {
            const auto ports = output->listPorts();
            if (ports.empty()) {
                std::cout << "출력 장치가 없습니다.\n";
                break;
            }
            const int port = askInt("출력 포트 번호 > ", 0);
            if (port < 0 || port >= (int)ports.size()) {
                std::cout << "잘못된 번호입니다.\n";
                break;
            }
            if (output->openPort((unsigned)port))
                std::cout << "출력 포트 열림: [" << port << "] " << ports[port] << "\n";
            break;
        }

        case 5:
            if (!output->isOpen()) {
                std::cout << "먼저 4번으로 출력 포트를 여세요.\n";
                break;
            }
            playNote(*output, midi::kNoteC4, 500);
            std::cout << "C4 노트를 전송했습니다.\n";
            break;

        case 6: {
            if (!output->isOpen()) {
                std::cout << "먼저 4번으로 출력 포트를 여세요.\n";
                break;
            }
            std::cout << "기타 표준 튜닝 기준음을 6번줄부터 재생합니다.\n";
            struct GuitarString {
                uint8_t note;
                const char* label;
            };
            const GuitarString kTuning[6] = {
                {midi::kNoteE2, "6번줄 E2"}, {midi::kNoteA2, "5번줄 A2"},
                {midi::kNoteD3, "4번줄 D3"}, {midi::kNoteG3, "3번줄 G3"},
                {midi::kNoteB3, "2번줄 B3"}, {midi::kNoteE4, "1번줄 E4"},
            };
            for (const auto& s : kTuning) {
                std::cout << "  " << s.label << "\n";
                playNote(*output, s.note, 800);
            }
            break;
        }

        case 7: {
            if (!output->isOpen()) {
                std::cout << "먼저 4번으로 출력 포트를 여세요.\n";
                break;
            }
            std::cout << "GM 악기 번호 예시: " << (int)midi::kGmAcousticGrandPiano
                      << "=피아노, " << (int)midi::kGmNylonGuitar << "=나일론기타, "
                      << (int)midi::kGmSteelGuitar << "=스틸기타, "
                      << (int)midi::kGmCleanGuitar << "=클린일렉기타, "
                      << (int)midi::kGmOverdrivenGuitar << "=오버드라이브, "
                      << (int)midi::kGmDistortionGuitar << "=디스토션\n";
            const int program = askInt("프로그램 번호 (0~127) > ", midi::kGmNylonGuitar);
            if (program < 0 || program > 127) {
                std::cout << "잘못된 번호입니다.\n";
                break;
            }
            const auto pc = MidiMessage::makeProgramChange(0, (uint8_t)program);
            std::cout << "  송신: " << MidiMessage::parse(pc).toString() << "\n";
            output->send(pc);
            playNote(*output, midi::kNoteE3, 700); // 바뀐 음색 확인용
            break;
        }

        case 8: {
            if (!output->isOpen()) {
                std::cout << "먼저 4번으로 출력 포트를 여세요.\n";
                break;
            }
            int value = askInt("볼륨 값 (0~127) > ", 100);
            if (value < 0) value = 0;
            if (value > 127) value = 127;
            const auto cc = MidiMessage::makeControlChange(0, midi::kCcVolume, (uint8_t)value);
            std::cout << "  송신: " << MidiMessage::parse(cc).toString() << "\n";
            output->send(cc);
            playNote(*output, midi::kNoteC4, 400); // 볼륨 반영 확인용
            break;
        }

        case 0:
            running = false;
            break;

        default:
            std::cout << "알 수 없는 메뉴입니다.\n";
            break;
        }
    }

    std::cout << "MidiPro를 종료합니다.\n";
    return 0;
}

# CLAUDE.md

이 파일은 Claude Code(claude.ai/code)가 이 저장소에서 작업할 때 참고하는 안내 문서입니다.

## 프로젝트 개요

Moonlight-Qt는 NVIDIA GameStream 및 Sunshine 프로토콜을 구현한 크로스 플랫폼 게임 스트리밍 클라이언트입니다(PC, macOS, Linux, Steam Link, Raspberry Pi 지원). H.264, HEVC, AV1 코덱과 하드웨어 가속 비디오 디코딩을 사용해 호스트 PC의 게임을 원격 클라이언트로 스트리밍합니다.

## 빌드 시스템

프로젝트는 **qmake**를 사용합니다(CMake 아님). 루트 `moonlight-qt.pro`가 서브프로젝트들을 관리합니다: `moonlight-common-c`, `qmdnsengine`, `h264bitstream`, `AntiHooking`(Windows), `soundio`(Windows/macOS), 그리고 메인 `app`.

**개발 빌드:**
```bash
# Linux/macOS
qmake6 moonlight-qt.pro
make -j$(nproc)          # 또는 'make debug' / 'make release'

# Windows (MSVC 개발자 명령 프롬프트에서)
qmake6 moonlight-qt.pro
jom                       # 또는 nmake
```

**플랫폼별 배포 빌드:**
```bash
scripts/build-arch.bat          # Windows
scripts/generate-dmg.sh         # macOS
scripts/build-appimage.sh       # Linux AppImage
scripts/build-steamlink-app.sh  # Steam Link
```

**빌드 구성 플래그** (qmake에 전달):
- `CONFIG+=embedded` — 창 모드/데스크톱 기능 비활성화 (Steam Link, Raspberry Pi용)
- `CONFIG+=gpuslow` — 느린 GPU에서 OpenGL/Vulkan 대신 DRM/KMSDRM 직접 렌더링 우선

자동화된 단위 테스트나 린트 설정은 없습니다. CI는 AppVeyor(`appveyor.yml`)를 통해 Windows, macOS, Linux, Steam Link 전 플랫폼에서 실행됩니다.

## 아키텍처

### 레이어 구조

```
QML UI (gui/)
    ↕ signals/slots
C++ 모델 & 매니저 (backend/)
    ↕
스트리밍 세션 (streaming/session.*)
    ↕
비디오 디코더 + 렌더러 | 오디오 렌더러 | 입력 핸들러
    ↕
moonlight-common-c (Limelight 프로토콜 라이브러리)
```

### 주요 컴포넌트

**`app/backend/`** — 호스트 탐색, 페어링, 앱 목록:
- `ComputerManager` — mDNS/수동 IP로 호스트를 탐색·추적하고 UI에 시그널 전송
- `NvHTTP` — GameStream/Sunshine API 호출용 HTTP 클라이언트
- `PairingManager` — OpenSSL 암호화를 이용한 PIN 기반 페어링 처리
- `IdentityManager` — 클라이언트 인증서 ID 관리
- `BoxArtManager` — 게임 박스 아트 다운로드 및 캐싱
- `RichPresenceManager` — Discord/Steam 리치 프레즌스 연동

**`app/streaming/`** — 활성 스트리밍 세션:
- `Session` — 핵심 조율자; 디코더/렌더러 선택 및 생명주기 관리
- `video/` — 디코더 구현체(FFmpeg, DXVA2, D3D11VA, VideoToolbox, VAAPI, VDPAU, MMAL) 및 렌더러(EGL, DRM, Vulkan/libplacebo)
- `audio/` — 렌더러(SDL, libsoundio, Steam Link 오디오)
- `input/` — 키보드, 마우스, 게임패드(SDL), 터치, 펜 입력 핸들러
- `overlaymanager.*` — 화면 내 통계 및 상태 오버레이

**`app/gui/`** — Qt Quick/QML UI 및 C++ 리스트 모델:
- `ComputerModel`, `AppModel` — QML 뷰를 지원하는 `QAbstractListModel` 서브클래스
- QML 파일이 네비게이션, 설정 화면, 스트리밍 실행을 담당

**`app/settings/`** — 영구 사용자 설정 (`StreamingPreferences`, `MappingManager`)

**`app/cli/`** — 헤드리스 CLI 서브커맨드 (pair, list apps, stream, quit)

### 비디오 디코더/렌더러 선택

`Session`이 스트림 시작 시 플랫폼과 코덱 지원 여부에 따라 디코더를 선택합니다. 디코더는 `IVideoDecoder`, 렌더러는 `IVideoRenderer` 인터페이스를 구현합니다. FFmpeg 기반 디코더는 소프트웨어 디코딩과 VAAPI/VDPAU/VideoToolbox/D3D11VA 하드웨어 디코딩을 지원합니다. 플랫폼 전용 디코더(`DXVA2DecoderRenderer`, `MmalVideoDecoder`)는 디코드+렌더 파이프라인 전체를 자체 처리합니다.

### 프로토콜

`moonlight-common-c`(`app/moonlight-common-c/` 서브모듈)는 Limelight 스트리밍 프로토콜을 구현한 C 라이브러리입니다. 이 라이브러리의 콜백이 `Session`의 비디오/오디오/제어 파이프라인을 구동합니다.

## 기술 스택

| 컴포넌트 | 상세 |
|---------|------|
| Qt | 6.7+ 권장; 5.9+ 최소 |
| C++ 표준 | C++11 (`CONFIG += c++11`) |
| 컴파일러 (Windows) | MSVC 2022 |
| FFmpeg | 4.0+ (libav*) |
| SDL2 | 입력, 창 관리, 오디오 폴백 |
| OpenSSL | TLS 및 페어링 암호화 |
| libplacebo | Vulkan 렌더링 (v7.349.0+) |
| Opus | 오디오 코덱 |

## 플랫폼별 참고사항

- **Windows**: DXVA2 및 D3D11VA 디코더; AntiHooking 라이브러리가 입력 후킹 간섭 방지; x86/x64/ARM64 지원.
- **macOS**: VideoToolbox 하드웨어 디코더; Qt를 통한 Metal 렌더링.
- **Linux**: VAAPI(Intel/AMD), VDPAU(NVIDIA), DRM 직접 렌더링; Wayland 및 X11 모두 지원.
- **Raspberry Pi**: MMAL 디코더; DRM 직접 렌더링을 위해 `CONFIG+=gpuslow` 권장.
- **Steam Link**: `CONFIG+=embedded`; Steam Link SDK 오디오/비디오 API 사용.

## 주요 파일

| 파일 | 역할 |
|------|------|
| `moonlight-qt.pro` | 루트 프로젝트, 서브프로젝트 순서, 공유 의존성 |
| `app/app.pro` | 메인 앱 빌드 설정, 플랫폼별 라이브러리/정의 |
| `globaldefs.pri` | 전역 디버그/새니타이저/컴파일러 플래그 설정 |
| `app/main.cpp` | 진입점: QML 엔진, 로깅 콜백, CLI 디스패치 |
| `app/streaming/session.cpp` | 핵심 스트리밍 조율 로직 |
| `appveyor.yml` | CI 빌드 매트릭스 (전 플랫폼) |

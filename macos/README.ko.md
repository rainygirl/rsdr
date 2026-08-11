# <img src="../icon.png" alt="" width="28" align="top"> macOS용 R SDR

*[English](README.md) · [문서 목록](../README.ko.md)*

![macOS에서 실행 중인 R SDR](screenshot.png)

## 설치

Homebrew로 빌드 의존성을 설치한 뒤 앱을 빌드·설치하세요.

```sh
brew install librtlsdr
cd macos
make
make install
```

앱은 `/Applications/R SDR.app`에 설치돼요. RTL-SDR 실행 라이브러리는 앱
번들에 포함되므로 설치된 앱을 실행하는 데 Homebrew가 필요하지는 않아요.

제한수신 T-DMB 서비스에 적법하게 보유한 DVB-CSA CW를 사용하려면 빌드 전에
선택 의존성을 설치하세요.

```sh
brew install libdvbcsa
```

일반 clear T-DMB 서비스에는 필요하지 않아요.

## 사용법

1. RTL2832U 호환 동글을 연결한 뒤 R SDR을 실행하세요.
2. Preset을 고르거나 Mode를 고른 뒤 주파수를 입력하세요. AM은 kHz, 나머지
   모드는 MHz 단위예요.
3. Play를 누르고 Volume을 조절하세요. Squelch는 AM, Air, LSB, USB 모드에서
   쓸 수 있고 맨 왼쪽으로 옮기면 꺼져요.
4. WFM의 Stereo는 신호가 충분히 강할 때 켜세요. 스펙트럼이나 워터폴을
   클릭하면 해당 주파수로 튜닝돼요.
5. T-DMB는 송출 중인 프리셋을 고르고 자동 게인 및 Station 탐색이 끝날 때까지
   기다리세요. 방송국을 고른 뒤 Play를 누르세요. Aspect로 영상을 16:9와 4:3
   사이에서 바꿀 수 있어요.

28.8 MHz 아래에서는 Q-branch 직접 샘플링으로 자동 전환돼요. 이 대역을
수신하려면 동글의 Q 입력이 안테나에 연결돼 있거나 별도 HF 입력으로 나와 있어야
해요.

동글 연결이 끊기면 다시 연결하고 컨트롤이 활성화될 때까지 기다리세요. 재생은
자동으로 시작되지 않으므로 준비된 뒤 Play를 누르세요.

### 제한수신 T-DMB 서비스

사용 권한이 있는 DVB-CSA CW를 입력하려면 `libdvbcsa`를 설치한 뒤
**R SDR > DMB Control Words…**를 여세요. Even/Odd CW는 12자리 또는 16자리
16진수를 받고 공백, 콜론, 하이픈을 구분자로 쓸 수 있어요. CW는 현재 실행의
메모리에만 보관되고 저장되지 않아요.

앱 실행 전에 `RSDR_DMB_EVEN_CW`, `RSDR_DMB_ODD_CW` 환경변수로 지정할 수도
있어요. 두 값을 모두 비우면 복호가 꺼져요.

## 라이선스

R SDR의 자체 소스 코드는 [MIT 라이선스](../LICENSE)로 배포돼요.
`third_party/`에 포함된 외부 구성 요소는 MIT로 재라이선스되지 않고,
각 구성 요소의 별도 라이선스를 따라요. 자세한 내용은
[서드파티 고지](../THIRD_PARTY_NOTICES.md)를 참고하세요.

## AI 사용 고지

R SDR의 일부는 AI 코딩 도구(Anthropic Claude)의 도움을 받아 개발했어요.
모든 코드는 작성자가 검토하고 테스트했어요.

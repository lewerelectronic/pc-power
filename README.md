# PC 원격 전원 v2 (PC Remote Power)

밖에서 텔레그램 버튼으로 집 PC를 켜고 끄는 ESP32-C3 장치예요.
설정은 PC USB에 꽂은 채로 크롬/엣지에서 설정 페이지(Web Serial)로 해요.
설정 페이지·설명서·봇 메시지는 한국어와 영어를 지원해요.

> An ESP32-C3 module that turns your home PC on/off from Telegram. Set it up over USB in Chrome/Edge (Web Serial).
> The settings page, user guide and bot messages are available in Korean and English.

## 설정 페이지 주소

설정 페이지는 **웹 주소로 여는 사이트**예요. 앱 설치는 필요 없어요.

- 설정 페이지 주소: **https://lewerelectronic.github.io/pc-power/** (GitHub Pages)
- 고객에겐 이 주소(또는 QR)를 제품 상자·설명서에 넣어 주면 돼요.
- `docs/index.html` 파일을 PC에서 더블클릭해서 열면 크롬이 설치 부품을 막아서 **펌웨어 설치가 안 돼요.** 꼭 https 주소로 열어 주세요.

## 구성

```
firmware/                     펌웨어 (PlatformIO, arduino-esp32 4.x)
  PCPowerSwitch/PCPowerSwitch.ino
  platformio.ini
docs/                         GitHub Pages로 올라가는 설정 페이지
  index.html                  설정 + 사용 설명서 탭
  manifest.json               펌웨어 설치 버튼(ESP Web Tools)용
  firmware/pcpower-factory.bin  설치용 펌웨어 (빌드된 파일, USB 설치)
  firmware/pcpower-app.bin      원격 업데이트용 앱 이미지 (개발자 모드)
  firmware/ota.json             원격 업데이트 정보: 버전·크기·MD5
  vendor/                     ESP Web Tools(Apache-2.0), qrcode-generator(MIT) — 외부 CDN 없이 동작
.github/workflows/firmware.yml  firmware/ 수정 시 자동 빌드 → docs에 반영
```

## 처음 올리는 법

1. GitHub에서 새 저장소를 **Public**으로 만들어요 (예: `pc-power`).
2. 이 폴더 내용을 그대로 올려요 (웹에서 "Add file → Upload files"로 끌어다 놓아도 돼요. `.github` 폴더도 같이).
3. 저장소 **Settings → Pages** → Source: **Deploy from a branch** → Branch: **main**, 폴더: **/docs** → Save.
4. 1~2분 뒤 `https://<아이디>.github.io/<저장소이름>/` 가 설정 페이지 주소예요.
5. **Settings → Actions → General → Workflow permissions**를 **Read and write**로 바꿔 두면, 펌웨어를 고쳐 올릴 때 자동으로 빌드돼요.

> Web Serial은 https에서만 동작해서 파일을 PC에서 직접 열면 펌웨어 설치 버튼이 안 돼요. GitHub Pages 주소로 열어 주세요.

## 펌웨어 고치기

- `firmware/PCPowerSwitch/PCPowerSwitch.ino` 수정 → `FW_VERSION` 올리기 → 커밋.
- Actions가 빌드해서 `docs/firmware/pcpower-factory.bin`, `pcpower-app.bin`, `ota.json`과 `docs/manifest.json` 버전을 갱신해요.
- 개발자 모드를 켠 기기는 텔레그램 `/update`(또는 자동 업데이트)로 이 새 버전을 받아요. GitHub Pages 캐시 때문에 올린 뒤 10분쯤 지나야 받아질 수 있어요. 캐시 때문에 파일이 덜 바뀌어 있으면 MD5가 안 맞아서 설치를 안 하니 안전해요.
- 로컬 빌드: `pip install platformio` 후 `pio run -d firmware`

## 설계 요약

| 항목 | 내용 |
|---|---|
| 제어 | 텔레그램 봇 (하단 버튼 4개, 끄기·강제 종료는 1분 유효 확인 버튼) |
| 등록 | 설정 페이지 QR = `t.me/<봇>?start=<10분짜리 코드>` → 시작 누르면 그 계정만 등록 |
| 설정 | USB 시리얼 JSON 명령만 (설정용 와이파이 AP 없음) |
| PC 상태 | USB SOF 패킷 감지 (`HWCDC::isPlugged()`), 추가 배선 없음 |
| 안전장치 | 보드 부팅 전/2분 지난 명령 무시, 이미 켜졌으면 켜기 안 누름, 재시작 알림 |
| 인증서 | 코어 내장 Mozilla CA 묶음 (`useBuiltinCACertBundle`) |
| 업데이트 | 기본은 설정 페이지에서 USB로. 개발자 모드(USB로만 켤 수 있음)면 텔레그램 `/update`·자동 업데이트로 원격 설치 |
| 롤백 | 새 펌웨어가 5분 안에 텔레그램에 연결 못 하거나 재부팅을 반복하면 이전 버전으로 복귀, 그 버전은 자동 업데이트에서 건너뜀 |
| 플래시 | `min_spiffs.csv` (앱 영역 2개, 각 1.9MB). NVS 위치가 예전 `huge_app`과 같아서 USB 업데이트해도 설정 유지 |

## USB 설정 프로토콜

요청 한 줄: `{"id":1,"cmd":"info"}` — `info | scan | wifi{ssid,pass} | token{token} | pair | unpair | set{txLow|lang|dev|autoUpd} | reboot | reset`
응답: `@{"id":1,"ok":true,...}`, 이벤트: `@{"evt":"hello|wifi|paired|pair_expired",...}`, 그 외 줄은 로그.

## 배선

```
GPIO4 ─[330Ω]─ PC817 1번(애노드)      GND ─ PC817 2번(캐소드)
GPIO4 ─[10kΩ]─ GND                    (보드가 켜지는 순간 버튼이 눌리는 것 방지)
PC817 4번(컬렉터) ─ PWR_SW +          PC817 3번(이미터) ─ PWR_SW −
                                      (Y 케이블로 케이스 버튼과 병렬)
```

XIAO ESP32C3는 GPIO4가 `D2` 핀이에요. PC357(미니 플랫)도 핀 순서가 같아서 PC817 자리에 그대로 쓸 수 있어요.

## 바뀐 점

- **v2.2.0**: 개발자 모드 — 텔레그램 `/update`로 원격 펌웨어 업데이트(확인 버튼, MD5 검사), 자동 업데이트(PC가 켜졌다 꺼지고 10분 동안 안 쓰면), 새 펌웨어가 5분 안에 텔레그램 연결 못 하면 자동 롤백·알림 · 플래시 배치를 앱 2개 영역으로 변경 · 설정 페이지 고급 설정에 개발자 모드 추가
  - v2.1.x에서 올리려면 설정 페이지에서 USB로 한 번 업데이트해야 해요 (그다음부터 원격 가능)
- **v2.1.1**: 켜기·끄기를 누른 뒤 결과 확인 전까지 같은 명령 재실행 거절(연타로 부팅 중 꺼짐 방지) · 와이파이/텔레그램이 30분 넘게 안 되면 스스로 재시작(재시작 알림에 이유 표시) · 텔레그램 429 응답의 대기 시간 준수 · 설정 페이지에 BOOT 버튼 복구 방법 추가
- **v2.1.0**: 영어 지원, 설정 페이지 주소 안내

## 검증 상태

- 펌웨어: arduino-esp32 4.x로 컴파일 확인 (RAM 12%, 앱 영역 66%), 롤백 훅(`verifyRollbackLater`) 적용 확인
- 원격 업데이트: 빌드·배포 스크립트와 설정 페이지는 확인, **실제 기기에서 OTA·롤백은 아직 테스트 전**
- 설정 페이지: 모의 기기로 브라우저에서 전체 흐름(연결 → 와이파이 → 토큰 → QR 등록) 확인
- **실제 하드웨어에서는 아직 테스트 전**이에요. 처음엔 📊 상태가 켜짐/꺼짐을 맞게 보여 주는지부터 확인해 주세요.

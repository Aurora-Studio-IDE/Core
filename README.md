# Aurora Studio Native

Aurora Studio의 네이티브 파일시스템 코어 모듈입니다.

이 저장소는 C99 기반의 공유 라이브러리 `aurora_studio-native`를 빌드하며,
파일/디렉터리 조작 명령과 에러 코드 매핑, 로깅, undo/redo 히스토리를 제공합니다.

## 주요 기능

- 파일/디렉터리 명령 처리
	- `list [path]`
	- `exists <path>`
	- `read <path>`
	- `write <path> <content>`
	- `append <path> <content>`
	- `delete <path>`
	- `mkdir <path>`
	- `rmdir <path>`
	- `undo`
	- `redo`
- 플랫폼 분기 지원
	- Linux/Unix 계열: `dirent.h`, `unistd.h`
	- Windows: Win32 API 분기
- 표준화된 에러 타입
	- 음수 기반 에러 코드 체계
- 간단한 로그 시스템
	- 표준 출력/에러 기반 로그 출력

## 디렉터리 구조

```text
.
├── CMakeLists.txt
├── LICENSE.md
├── README.md
└── fs
		├── CMakeLists.txt
		├── app
		│   ├── fs.c
		│   └── log.c
		└── inc
				├── err.h
				├── fs.h
				└── log.h
```

## 요구 사항

- CMake 3.16 이상
- C 컴파일러(C99 지원)

## 빌드

프로젝트 루트에서 아래 명령으로 빌드합니다.

```bash
cmake -S . -B build
cmake --build build
```

빌드 결과로 공유 라이브러리 `aurora_studio-native`가 생성됩니다.

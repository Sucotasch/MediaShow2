# MediaShow2: Контекст проекта

## 1. Исходный плагин MediaShow v0.9.5

### Общие сведения
- **Автор:** Chernih Sergey (SCHM@ukr.net), Украина
- **Год:** 2006 (Delphi 6 + DSPack 2.31)
- **Тип:** WLX-плагин для Total Commander (лиSTER)
- **Формат:** `.wlx` ZIP-архив содержащий: MediaShow.wlx (DLL), MScontrol.exe, MediaShow.lng, pluginst.inf, readme.txt
- **Зависимость:** DirectX 8.1+
- **Размер:** ~500KB (UPX packed), ~3.2MB (unpacked)

### Поддерживаемые форматы
- **Видео:** AVI (включая DivX), MPG/MPEG, DAT (VideoCD), ASF, VOB (DVD)
- **Аудио:** MP1, MP2, MP3, WAV, WMA, OGG, MIDI/KAR
- **Плейлисты:** WinAmp (.m3u/.pls), SMPlayer (.mpl), SMViewer (.smv), LightAlloy (.lap)
- **Детекция:** по расширению + по заголовку файла (magic bytes)

### Архитектура
- **Движок:** DirectShow (Overlay Mixer — deprecated с Vista)
- **Два режима:**
  - **Lister mode (F3 / Ctrl+Q):** child window ВНУТРИ TC lister panel. TC показывает заголовок "Lister (MEDIA Show)-[filename]"
  - **Independent player:** отдельное popup окно "MEDIA Show player-[filename]"
- **Компоненты:** TMediaEngine, TMediaEngineFast, TMediaSetts, TMediaInfoTipForm
- **DSP модули (7):** dspAmplify, dspDynamicAmplify, dspEqualizer, dspFastFourier, dspSound3D, dspTrueBass, dspUtils
- **Реестр:** HKCU\Software\SCHMaster\

**Важно:** Оригинал НЕ использует WS_VISIBLE напрямую (0 вхождений в бинарнике). Delphi VCL управляет видимостью автоматически через TForm.Show. Окно lister — это child window внутри TC lister panel.

**КРИТИЧЕСКИЙ ВЫВОД ИЗ TCPlayer:** TC lister НЕ ПОДДЕРЖИВАЕТ создание сложного UI (trackbar, toolbar) внутри lister panel. TC перехватывает/модифицирует mouse сообщения. Правильный подход — запуск отдельного окна плеера (как делает TCPlayer через ShellExecuteEx). ListLoad создаёт minimal child window и запускает основной плеер.

### UI/UX оригинала (из анализа бинарника)

**Lister mode (F3):**
- Основное окно — Delphi TForm с MenuBar и ToolBar
- Транспортные кнопки: BTN_PLAY, BT_PREV, BT_STEP_N
- Seekbar (TrackBar) для перемотки
- Volume control (TrackBar)
- OSD (On-Screen Display) — поверх видео
- QuickView mode (Ctrl+Q) — встроен в панель TC

**Independent player mode:**
- Отдельное окно "MEDIA Show player-[filename]"
- Полноценный MenuBar с разделами
- Toolbar с кнопками управления
- Плейлист встраивался в основное окно (не popup)
- Status bar с информацией о файле

**Центральная область окна:**
- **Видео режим:** рендеринг видео
- **Аудио режим:** плейлист (TListView) в центре окна
  - pnlDisplay — основная панель отображения
  - DisplayPanelPaint — кастомная отрисовка
  - BKG_LIST / PLAYLIST — плейлист встраивался в layout
  - Show playlist — меню для показа/скрытия плейлиста
  - Album, Track no — информация из тегов отображалась рядом
- **Визуализация:** DisplayVisualisation (FFT-based) — опционально

**Context Menu (правый клик):**
- Play/Pause, Stop
- Previous/Next file
- Volume Up/Down, Mute
- Forward/Back 10s
- File information
- Screenshot, Copy image
- Fullscreen, Stay on top
- DSP settings (Equalizer, 3D Sound, True BASS, Amplification)
- Video adjustment (Brightness, Contrast, Saturation, Zoom)
- Settings, About

**Keyboard Shortcuts:**
- Play/pause, Volume up/down, Position change
- Step forward/backward frame
- DVD navigation (prev/next chapter)
- Various mouse settings

**Features:**
- Playlist library (сохранение/загрузка плейлистов)
- Random/Repeat режимы
- Auto-save/load позиции
- Find media files in folder (recursive)
- Drag & Drop файлов
- Clipboard — копирование информации о файле
- Screenshot — сохранение кадра в файл/буфер
- Brightness/Contrast/Saturation/Zoom
- Digital zoom
- Tray mode (фоновое воспроизведение)
- Stay on top
- Skins (LightAlloy формат)
- CPU load indicator
- DirectShow filter settings
- SCHM-DSP companion filter (для DVD, audio enhancement)
- Auto-fullscreen при просмотре видео
- Accompaniment (воспроизведение всех файлов в директории)
- Speed control (play speed)
- Step-by-step video (пошаговое воспроизведение)
- Tag editing (MP3/OGG)
- AVI info editor
- MEDIA Content Base integration

**File information format:**
```
[bitrate] [channel mode]|[resolution] [FPS] [videobitrate]
```

**Settings (TMediaSetts):**
- Applications work directory
- Work directory
- User profile directory
- Total Commander directory
- Save/load parameters
- Disable screen saver
- Disable monitor power off
- Beep on SysHotKey click
- SecondaryShortCuts
- Fullscreen mode only
- Start full screen mode
- Use video resolution
- Display full file name / Display name / Display font
- Use OSD / OSD mode
- Tray icon / Minimize to tray / Tray info mode
- Auto show information in QuickView mode
- Auto exit on play end in QuickView mode
- Get file information on start playing
- Detect on file header/extension
- Play speed
- Volume change step
- Fade IN/OUT on play/pause
- Stop video on minimize
- Restore last position
- Auto save/load last playlist
- On play list complete: Stop/Repeat/Exit/Power off

### Known issues (original)
1. Overlay Mixer — deprecated since Vista, removed in Windows 8+
2. No ASLR/DEP in original binary
3. DPI-unaware (hardcoded pixel values in DFM)
4. Font charset issues (Delphi 6 DEFAULT_CHARSET)
5. GDI text on themed backgrounds can be invisible

### Бинарные данные (из анализа)
- **DFM формы:** TMediaInfoTipForm, MEDIA Show bitmap preview, MEDIA Show FX, MEDIA Show, Attention, ShutDownProgress, MediaSetts
- **Компоненты:** 47 TButton, 47 TCheckBox, 24 TComboBox, 45 TEdit, 76 TLabel, 9 TToolBar, 3 TPageControl, 25 TTabSheet, 7 TTimer, 3 TPopupMenu, 2 TMainMenu
- **DLL импорты:** kernel32, user32, gdi32, advapi32, shell32, comctl32, comdlg32, ole32, oleaut32, quartz, winmm
- **DirectShow:** IGraphBuilder, IMediaControl, IVideoWindow, IMediaEvent, IBasicAudio, IBasicVideo, IMediaSeeking

---

## 2. Задача: создание современного аналога

### Цель
Создать WLX-плагин для Total Commander, заменяющий устаревший MediaShow v0.9.5, с:
- Совместимостью с Windows 10/11
- Нативной поддержкой современных форматов (MP4, MKV, FLAC, AAC, WEBM)
- 64-битной архитектурой (.wlx64)
- Современным UI/UX, соответствующим текущим стандартам Windows
- Минимальными зависимостями (только Windows SDK)

### Требования к функционалу

**Воспроизведение:**
- Видео/аудио воспроизведение (Media Foundation + DirectShow fallback)
- Сохранение соотношения сторон видео при изменении размера окна
- Fullscreen режим (F11 / двойной клик)
- QuickView mode (Ctrl+Q в TC)
- Auto-advance при окончании файла (itm_next)
- Плавное перемотка (seekbar с ползунком)
- Управление громкостью регулятором (volume slider)
- Mouse wheel: ТОЛЬКО управление громкостью в районе Volume Slider (привязка колеса к seek НЕ НУЖНА)

**Плейлист:**
- Плейлист встраивается в основное окно (не popup) — показывается в центральной области при аудио
- При видео: плейлист скрыт, центральная область = видео
- Переключение: context menu "Show playlist" или кнопка
- Редактирование: добавление/удаление/перемещение файлов
- Prev/Next навигация по плейлисту
- Random/Repeat режимы
- Сохранение/загрузка плейлистов

**Интерфейс:**
- Современный UI, соответствующий стандартам Windows 10/11
- Context menu (правый клик)
- Keyboard shortcuts
- Dark mode support

**Информация о файле:**
- Вывод информации о медиафайле (кодек, битрейт, разрешение, длительность)
- Чтение тегов (MP3 ID3, OGG Vorbis, FLAC, MP4 metadata)
- Редактирование тегов

**Режимы:**
- Always On Top (поверх всех окон)
- Close To Tray (фоновое воспроизведение, сворачивание в трей)

**НЕ включать (нерентабельно):**
- DVD навигация (мёртвый формат)
- Skins (использовать нативную тему Windows)
- DSP/Equalizer
- OSD overlay
- Brightness/Contrast/Saturation
- Speed control
- 3D Sound / True BASS
- MEDIA Content Base integration

---

## 3. Реализация и текущее состояние

### Технологический стек
- **Язык:** C++ (MSVC)
- **Сборка:** CMake
- **Движок:** Media Foundation (primary) + DirectShow (fallback)
- **UI:** Win32 API (GDI, CommCtrl)
- **Платформа:** Windows 7+ (x86 + x64)

### Структура проекта
```
MediaShow2/
├── CMakeLists.txt
├── src/
│   ├── dllmain.cpp         — основной код плагина (TC API, UI, playlist)
│   ├── mf_player.h/cpp     — Media Foundation движок
│   ├── ds_player.h/cpp     — DirectShow fallback
│   ├── plugin_api.h        — TC WLX SDK + ID контролов
│   └── app.manifest        — DPI awareness, Common Controls v6
├── sdk/
│   └── listplug.h          — TC WLX SDK header
├── package.py              — скрипт упаковки в .wlx/.wlx64
├── PROJECT_CONTEXT.md      — этот документ
└── MediaShow2.wlx64        — готовый плагин
```

### Что реализовано (частично)
1. **Воспроизведение:** Media Foundation (IMFPMediaPlayer) + DirectShow (IGraphBuilder) fallback — работает
2. **Toolbar:** Play/Stop/Prev/Next/<</>> кнопки — работают
3. **Seekbar:** TrackBar control — работает
4. **Volume slider:** TrackBar control — работает
5. **Status bar:** Win32 StatusBar — работает
6. **Context menu:** WM_CONTEXTMENU — работает
7. **Keyboard shortcuts:** Space, S, N, P, M, F11, Esc — работают
8. **Mouse wheel:** Текущая реализация: Ctrl+wheel = volume, wheel = seek. ТРЕБУЕТСЯ: только volume, без привязки к seek
9. **Auto-advance:** itm_next — работает
10. **Playlist:** сканирование директории — работает (но нужен из выделенных файлов)
11. **Dark mode:** через GetSysColor + lcp_darkmode — работает
12. **ASLR + DEP:** CMAKE_SHARED_LINKER_FLAGS — работает
13. **DPI awareness:** manifest — работает
14. **Fullscreen:** F11 / двойной клик — работает
15. **Aspect ratio:** letterboxing — работает

### Что НЕ реализовано
1. **Плейлист из выделенных файлов TC** — механизм найден (LCLListBox + LB_GETTEXT), но парсинг имени файла сломан (прилипает размер), тип определяется неверно, даты пусты. См. секцию "Плейлист из выделенных файлов"
2. **Современный UI** — текущий interface примитивен
3. **Плейлист с редактированием**
4. **Информация о файле + теги** (Album, Track no, bitrate и т.д.)
5. **Редактирование тегов**
6. **Always On Top**
7. **Close To Tray (фоновое воспроизведение)**

---

## 4. Проблемы и ограничения

### Исправленные баги
- **Seekbar/Volume:** Были исправлены (Defect #1: toolbar covers trackbars — исправлено через CCS_NORESIZE | CCS_NOPARENTALIGN)

### Плейлист из выделенных файлов

**Статус:** Работает частично. Найден рабочий механизм через `LCLListBox` (TC использует Lazarus/Free Pascal).

**Как работает:**
1. `FindWindow(TTOTAL_CMD)` находит главное окно TC
2. `EnumChildWindows` ищет `LCLListBox` — файловую панель TC
3. `LB_GETSELCOUNT` / `LB_GETSELITEMS` получают список выделенных индексов
4. `LB_GETTEXT` для каждого индекса возвращает строку вида:
   ```
   filename.ext NNN NNN NNN DD.MM.YYYY HH:MM -a--
   ```
5. Имя файла извлекается отсечением: ищем `DD.MM.YYYY`, берём всё до него, затем убираем размер (3 группы цифр) с конца

**Отладочные выводы подтверждают:**
- `LCLListBox` найден ✅
- `LB_GETSELCOUNT=3` — 3 файла выделены ✅
- `LB_GETTEXT` возвращает правильные строки ✅
- `datePos` находится ✅

**Баги:**

1. **Имя файла содержит прилипший размер** — парсинг не отсекает размер `12 681 905` от конца строки. Пример вывода:
   ```
   file[0]='f:\...\02 - Arpadhazi Margit balladaja.mp3 12 681 905 '
   ```
   Причина: размер removal логика не работает корректно — `beforeLen` включает trailing space перед датой, а размер содержит пробелы между группами цифр. Нужен более надёжный алгоритм.

2. **Тип файла всегда "Video"** — `showPlaylist` выставляется по `IsAudioOnly(state->filePath)` до замены плейлиста, но после `RequestSelectedFiles` плейлист содержит mp3 файлы. Нужно обновлять `showPlaylist` на основе текущего файла.

3. **Дата пуста (1601-01-01 04:00)** — `FreePlaylist()` уничтожает `state->fileDates` после заполнения через `GetFileAttributesEx`. Нужно сохранять массив дат перед вызовом `FreePlaylist`.

4. **Проигрывание только одного файла** — после проигрывания текущего файла плейлист закрывается. Причина: `showPlaylist = FALSE` (тип "video") → плейлист скрыт → навигация по плейлисту не работает.

**Исправлено (commit 920bd55):**
- Сохранение `fileDates` перед `FreePlaylist`
- Обновление `showPlaylist` после замены плейлиста

**Осталось исправить:**
- Парсинг имени файла — нужно убрать размер из строки LB_GETTEXT
- Добавить больше отладки для понимания где именно ломается парсинг

### Сохранение соотношения сторон
**Проблема:** При изменении размера окна видео растягивается непропорционально.

**Решение:** Использовать MFVideoNormalizedRect или IVideoWindow::SetAspectRatio для сохранения aspect ratio. При необходимости — черные полосы (letterboxing).

---

## 5. Требования к UI/UX

### Принципы
- Соответствие стандартам Windows 10/11 (Fluent Design language)
- Минимализм — только необходимые элементы управления
- Адаптивность — корректное отображение при любом размере окна
- Accessibility — поддержка DPI scaling, keyboard navigation

### Раскладка окна (Lister mode)

**Видео режим:**
```
┌──────────────────────────────────────────┐
│ [▶] [■] [◄◄] [►►]  ━━━━●━━━━━━━━━━━━  │ ← Транспорт + Seekbar
│ ┌──────────────────────────────────────┐ │
│ │                                      │ │
│ │          Video Area                  │ │
│ │    (сохранение aspect ratio)        │ │
│ │                                      │ │
│ └──────────────────────────────────────┘ │
│ Vol ━●━━━  [file info]  [playlist]      │ ← Volume + Info
└──────────────────────────────────────────┘
```

**Аудио режим (плейлист в центре):**
```
┌──────────────────────────────────────────┐
│ [▶] [■] [◄◄] [►►]  ━━━━●━━━━━━━━━━━━  │ ← Транспорт + Seekbar
│ ┌──────────────────────────────────────┐ │
│ │ ▶ song1.mp3                         │ │
│ │   song2.mp3                         │ │ ← Плейлист
│ │   song3.mp3                         │ │
│ │   song4.mp3                         │ │
│ │   ...                               │ │
│ └──────────────────────────────────────┘ │
│ Vol ━●━━━  [file info]  [playlist]      │ ← Volume + Info
└──────────────────────────────────────────┘
```

**Переключение:** context menu "Show playlist" или кнопка. При видео — плейлист скрыт.

### Элементы управления
- **Транспорт:** Play/Pause (▶/⏸), Stop (■), Prev (◄◄), Next (►►)
- **Seekbar:** Горизонтальный TrackBar, tooltip с временем при наведении
- **Volume slider:** Вертикальный TrackBar справа, mouse wheel = громкость
- **Info bar:** Название файла, длительность, кодек, битрейт
- **Playlist panel:** Список файлов с возможностью добавления/удаления/перемещения

### Клавиатура
| Действие | Клавиша |
|----------|---------|
| Play/Pause | Пробел |
| Stop | S |
| Seek ±5с | ←/→ |
| Volume ±5% | ↑/↓ |
| Mute | M |
| Fullscreen | F11 |
| Always On Top | Ctrl+T |
| Playlist | L |
| File Info | I |
| Close | Esc |

### Mouse
| Действие | Гест |
|----------|------|
| Play/Pause | Клик по видео |
| Fullscreen | Двойной клик по видео |
| Volume | Mouse wheel (в любом месте) |
| Context menu | Правый клик |

---

## 6. Текущее состояние файлов

| Файл | Описание | Строки |
|------|----------|--------|
| `src/dllmain.cpp` | Основной код: TC API, UI, playlist | ~760 |
| `src/mf_player.cpp` | Media Foundation движок | ~230 |
| `src/ds_player.cpp` | DirectShow fallback | ~234 |
| `src/plugin_api.h` | TC WLX SDK + ID контролов | ~43 |
| `MediaShow2.wlx64` | Готовый плагин | 14.7KB |

---

## 7. Полезные данные для продолжения

### TC WLX API (из ghisler/WLX-SDK)
```c
// Обязательные
HWND __stdcall ListLoad(HWND ParentWin, char* FileToLoad, int ShowFlags);
void __stdcall ListCloseWindow(HWND ListWin);
void __stdcall ListGetDetectString(char* DetectString, int maxlen);
void __stdcall ListSetDefaultParams(ListDefaultParamStruct* dps);

// Опциональные
int __stdcall ListLoadNext(HWND ParentWin, HWND PluginWin, char* FileToLoad, int ShowFlags);
int __stdcall ListNotificationReceived(HWND ListWin, int Message, WPARAM wParam, LPARAM lParam);
int __stdcall ListSendCommand(HWND ListWin, int Command, int Parameter);
```

**Ключевые факты TC API:**
- TC subclassит окно плагина для перехвата 'n'/'p' клавиш
- ListNotificationReceived пересылает WM_COMMAND от toolbar кнопок
- itm_next сообщает TC переключиться на следующий файл
- ParentWin — окно lister, плагин создаёт child window
- Нельзя перехватывать Tab/стрелки/F3 — TC использует их
- ListLoad вызывается при F3 и Ctrl+Q (QuickView)
- ListLoadNext вызывается при n/p клавишах и навигации в QuickView

### Оригинальный MediaShow: как получал выделенные файлы
- FindWindowEx для поиска SysListView32
- Clipboard API (OpenClipboard, GetClipboardData, CloseClipboard)
- Сканирование директории (Find media files in folder)
- Плейлист с библиотекой (Save/Load)

### Движок воспроизведения

**Media Foundation (primary, Windows 7+):**
- IMFPMediaPlayer через MFPCreateMediaPlayer
- IMFVideoDisplayControl для рендеринга видео
- SetPosition/GetPosition для seek
- SetVolume для громкости

**DirectShow (fallback):**
- IGraphBuilder::RenderFile
- IMediaControl::Run/Pause/Stop
- IVideoWindow для видео
- IBasicAudio для громкости
- IMediaSeeking для seek

**Нативная поддержка MF (Windows 7+):**
AVI, ASF/WMA/WMV, MP3, WAV, MPEG-4 (MP4/M4A/MOV), AAC

**НЕ поддерживается MF (нужен DirectShow):**
OGG, FLAC, MKV/WebM, DAT, VOB, MIDI, MPEG-1

### Сборка
```bash
cmake -B build -G "Visual Studio 17 2022" -A Win32
cmake --build build --config Release

cmake -B build_x64 -G "Visual Studio 17 2022" -A x64
cmake --build build_x64 --config Release

python package.py
```

### Detect string
```
MULTIMEDIA & (ext="AVI" | ext="MPG" | ext="MPEG" | ext="ASF" | ext="VOB" | ext="MP1" | ext="MP2" | ext="MP3" | ext="WAV" | ext="OGG" | ext="WMA" | ext="DAT" | ext="MKV" | ext="WEBM" | ext="MP4" | ext="M4A" | ext="FLAC" | ext="AAC" | ext="OPUS" | ext="MID" | ext="MIDI" | ext="KAR")
```

### Полезные ресурсы
- ghisler/WLX-SDK — TC WLX SDK header + docs
- ghisler/WDX-SDK — TC WDX SDK
- little-brother/csvtab-wlx — референсный WLX плагин (C, ~800 строк)
- AlexanderPro/Wlx2Explorer — WLX плагины из Explorer

### Паттерны из csvtab-wlx
- `SetProp/GetProp` для per-instance состояния
- `SetWindowLongPtr(GWLP_WNDPROC)` для subclassing
- `CreateStatusWindow` со SB_SETPARTS
- `TrackPopupMenu` с TPM_RETURNCMD
- WM_MOUSEWHEEL на main window
- WM_KEYDOWN через subclass
- NM_RCLICK на ListView для context menu
- Статус-бар с несколькими частями (SB_SETPARTS)

### Known issues с текущим подходом
1. **КРИТИЧНО: UI внутри TC lister не работает** — TC перехватывает/модифицирует mouse сообщения. Trackbar, seekbar, volume slider не получают ввод. TCPlayer (референс) использует ДРУГОЙ подход: ListLoad запускает отдельное окно (TCPlayer.exe) через ShellExecuteEx, а не создаёт UI внутри lister.
2. **TC lister API** — не даёт доступа к выделенным файлам напрямую
3. **Соотношение сторон** — видео искажается при изменении размера окна
4. **Архитектура требует пересмотра** — нужно запускать отдельное окно плеера, а не пытаться встроить UI в lister

# Audit.md — Инженерный обзор MediaShow2 (v2, по полному прослеживанию кода и истории)

**Дата:** 2026-08-15
**Метод:** полное чтение исходников (`src/dllmain.cpp` 2952 стр., `src/mf_player.cpp`, `src/ds_player.cpp`, заголовки, `CMakeLists.txt`, `package.py`, все `test_*.py`), полное чтение `PROJECT_CONTEXT.md`, `FIXES.md`, `Summary.txt`, `deep_analysis.md` (929 стр.), история git, трассировка ключевых алгоритмов (переключение движков, плейлист, append, QuickView, жизненный цикл MF-колбэка) по реальному коду.
**Валидация (реальная):** `cmake --build build` (x86) и `build-x64` (x64) — 0 ошибок (MSBuild 17.14.51, VS 2022 Community, MSVC 14.44.35207); `test_parse.py`, `test_parse_c.py`, `test_bug.py` — PASS; `test_verify.py` — T01–T06 (после пересборки).

> **Важно про документацию:** `deep_analysis.md` описывает **старое** состояние кода (там прямо заявлено «оба плеера — DirectShow»). Текущий `mf_player.cpp` — **настоящий Media Foundation** (`MFStartup`, `MFPCreateMediaPlayer`, `IMFPMediaPlayer`, `MFCallback` с рефкаунтом и флагом `destroying`). Все выводы ниже сделаны по **текущему коду**; упоминания документации — только как история решений.

---

## Addendum 2026-08-15: реальный баг — webm (AV1) не воспроизводился, исправлено (F17)

Пользовательский отчёт: `6ix9ine.webm` в папке MediaShow2 не воспроизводится в TC, другие плееры играют. Диагностика реальными функциями плагина (консольный харнесс, линкующий `mf_player.cpp`/`ds_player.cpp`, окно за пределами экрана):

| Файл (кодек) | MF с окном | DS (VMR-9) |
|---|---|---|
| 7.mp4 (H.264/AAC) | ✅ pos растёт | — |
| **6ix9ine.webm (AV1/Opus)** | ❌ Open/Play S_OK, но **pos/dur = 0.00** — стопор | ✅ pos растёт, dur=177 с |
| 6.avi (VP9) | ✅ pos растёт | — |

**Корень:** MFP с реальным видео-окном не может рендерить AV1 (EVR встаёт, часы не идут); без окна (NULL) тот же файл играет. `MFPlayer_AudioNeedsDS` для него возвращал 0 (аудио-проверка не срабатывала), файл уходил в MF и молча замирал — то есть проявление найденного в C2 класса «асинхронная/скрытая остановка MF», но с конкретным и воспроизводимым триггером.

**Фикс (внедрён, пересобран, T01–T06 PASS):** эвристика расширена — помимо аудио-кодеков проверяется видео-субтип; **AV1 (`MFVideoFormat_AV1`) → DirectShow**. Функция переименована `MFPlayer_AudioNeedsDS` → `MFPlayer_NeedsDS` (3 точки вызова: PlayIndex, ListLoadW, ListLoadNextW). VP9/H.264 остаются на MF. Проверка: `NeedsDS(webm)=1`, `NeedsDS(mp4)=0`, DS играет webm. **[TC-тест]** — F3 на 6ix9ine.webm.

---

## Часть 0. Как устроен проект и почему именно так (что и для чего сделано)

### 0.1. Два движка и почему их два

- **`mf_player.cpp` — Media Foundation** (`MFPCreateMediaPlayer`), основной движок. Асинхронная модель: `Open()` возвращает S_OK сразу, media item создаётся в фоне, события приходят в `MFCallback`.
- **`ds_player.cpp` — DirectShow** (ручная сборка графа с **VMR-9**, чтобы не получить Overlay Mixer), fallback для кодеков, которые MF умеет читать из контейнера, но **не декодирует**: Opus, Vorbis, AC-3, E-AC-3, DTS. Определяется `MFPlayer_AudioNeedsDS()` по `Data1` GUID субтипа первого аудиострима (через `IMFSourceReader`). FLAC исключён намеренно (MF поддерживает FLAC с Win10 1709). MKV/DAT/VOB/MIDI тоже идут через DS — либо по `AudioNeedsDS`, либо как вторая попытка, когда `MFPlayer_Open`/`MFPlayer_Play` не сработал.
- **`useDirectShow`** — флаг «активен DSPlayer, а не MFPlayer». Критично для чтения всех веток переключения.

### 0.2. Центральная проблема: состояние рендера на HWND (сюжет VP9)

Задокументировано в PROJECT_CONTEXT §8 и подтверждено кодом. MF рендерит видео во внутреннюю D3D-поверхность, привязанную к `hVideoWnd`. При `MFPlayer_Destroy` COM-объекты освобождаются, но **HWND сохраняет D3D-состояние**. Новый MF-плеер на том же HWND наследует его и не может инициализировать рендер другого кодека (симптом: VP9 → видео-only H.264 — зависание на последнем кадре). Аудиострим сбрасывает pipeline (поэтому файлы со звуком работали). Решение, выработанное серией коммитов:

| Коммит | Решение | Результат |
|---|---|---|
| `b5adad0` | QuickView и MF→MF: `RecreateVideoWindow` (уничтожить HWND и создать новый) вместо `DestroyChildVideoWindows` | VP9-freeze ушёл из QuickView |
| `7a5732a` | Отложенное пересоздание через таймер (`IDT_RECREATE`) | «Без хэнга на быстрых переключениях», но мерцание/сложность |
| `4f2d48a` | Фаза 2 (F7): немедленный `RecreateVideoWindow` в `ListLoadNextW`, таймер `IDT_RECREATE` удалён | Единая стратегия во всех ветках; риск хэнга на быстрых ↓/↑ остаётся **[TC-тест]** |
| `ba60965` | `MFPlayer_Stop`: `Sleep(50)` только если плеер реально рендерил (`wasPlaying`) | -50 мс на каждом втором Stop |
| `79b7220` | F1–F15 (парсер, безопасный realloc, рефкаунт MF, dirty-флаг плейлиста…) | База аудита внедрена |

Инвариант всех веток переключения: **`MFPlayer_Destroy` → `RecreateVideoWindow` → `MFPlayer_Create` → `Open`** плюс `DSPlayer_SetVideoWnd` для синхронизации (DSPlayer держит свой HWND).

Дополнительно: `switchInProgress` + таймер `IDT_COOLDOWN` (500 мс) — сериализация переключений: MFP досылает события асинхронно, повторный вход в `PlayIndex` во время тейрдауна мог бы дать двойное открытие. `WM_PLAYER_TRACK_END` при `switchInProgress` отбрасывается (сознательный компромисс).

### 0.3. Жизненный цикл MF-колбэка (F6, рефкаунт)

`MFCallback` держит `tagMFPlayer` живым через `refCount` (1 = владелец API, +1 пока жив колбэк), `destroying` подавляет `onEnd` после начала `MFPlayer_Destroy`. `onEnd → OnMFEnd` только `PostMessage(WM_PLAYER_TRACK_END)` — вся работа переключается в UI-поток (наследие D4: прямой вызов из потока события был гонкой). Проверено по коду: порядок освобождения корректен и для успешного, и для провального Open, и для in-flight события.

### 0.4. Плейлист: пайплайн

1. **Источник:** выделение в TC — `EnumChildWindows` ищет `LCLListBox` (панель TC = Lazarus, **не** SysListView32), `LB_GETSELITEMS` + `LB_GETTEXT`; строка `name NNN[NBSP]NNN[NBSP]NNN[TAB]DD.MM.YYYY HH:MM -a--`. Если выделения нет — `ScanDirectoryForMedia`.
2. **Парсер** `ParseTCFileName` (F1): ищет `DD.MM.YYYY` слева, режет 1–3 группы размера справа, разделители — пробел/NBSP/TAB (`IsTCSeparator`). Проверка существования файла (`GetFileAttributesEx`) отсекает мёртвые записи (M9).
3. **Хранение:** `TCHAR** playlist` + `FILETIME* fileDates` + `playlistIndex`. `playlistDirty` (F14) — запись на диск только при реальном изменении.
4. **Персистентность:** `MediaShow2_playlist.txt` (индекс + пути), prune отсутствующих файлов при загрузке; `ClearPlaylist` удаляет файл.
5. **Сортировка** — `qsort_s` по глобальным `g_sort_*` (только UI-поток, безопасно), колонки #/Name/Type/Date, реверс по клику.
6. **Синхронизация индекса:** в `ListLoadNextW` (навигация n/p TC) индекс ищется по `filePath` (F4) — иначе маркер «▶» и Prev/Next считали бы от старого индекса.

### 0.5. Append mode и QuickView

- **Append ON:** F3 создаёт новую вкладку; плагин собирает выделенные файлы, **дописывает** в плейлист существующего окна (`hLastPluginWnd`), закрывает новую вкладку `PostMessage(ParentWin, WM_CLOSE)` и возвращает `NULL` (F3 всегда создаёт новую вкладку — лимитация WLX API). Дубликаты отсекаются `IsDuplicate`. При старте с append ON — загрузка сохранённого плейлиста + добавление выделения/скана директории.
- **Append OFF:** закрывается старая вкладка, **только если она из той же директории** (`SameDirectory`, F8) — чтобы не убивать параллельные сессии.
- **QuickView** (`GetParent(ParentWin) != NULL`): плейлист не грузится/не сохраняется, `showPlaylist=FALSE`, `UpdatePlaylist` пропускается. При повторном `ListLoadW` на том же `ParentWin` (TC не вызывает `ListCloseWindow`) prep-блок сбрасывает MF-состояние **старого** окна, затем создаётся **новое** окно.
- **`s_fixMaximizeWnd` + таймер 9998:** TC выставляет `WS_MAXIMIZE` на lister-окне — снимается сразу и повторно по таймеру (TC может перезаписать после возврата).

### 0.6. Что ещё есть

- Громкость/повтор/append — персистентность через INI (`iniPath` из `ListSetDefaultParams`), `ApplyVolume` после каждого Open (наследие D2).
- Тайтл lister'а: `UpdateListerTitle` — TC сам обновляет заголовок только при своём `ListLoad*`; при переключении плейлистом плагин переписывает содержимое `[...]` на полный путь (коммиты `2638cae`→`d7d0e94`: последняя `]` как граница — имена с `[1080p]`; полный путь, а не базу; диагностика вычищена).
- Тёмная тема: `lcp_darkmode`/`lcp_darkmodenative` → `ApplyTheme` (+ повторно в `lc_newparams`, F13 M10/D20).
- Таймер-поллинг позиции (id=1, 500 мс): обновляет `duration`, `position`, статус и сикбар **безусловно** (важно для анализа C1 ниже).

---

## Часть 1. Статус известных дефектов (проверено по текущему коду)

| # (deep_analysis) | Суть | Статус в коде |
|---|---|---|
| D1 | Toolbar CCS_TOP накрывает трекбары | ✅ `CCS_NORESIZE\|CCS_NOPARENTALIGN` (F1-фикс на месте) |
| D2 | Громкость прыгает | ✅ `LoadVolume`/`SaveVolume`/`ApplyVolume` после каждого Open |
| D3 | Колесо: инверсия+seek | ✅ wheel в `cbNewMain`/`VolSliderProc` — громкость; сикбар — только своё колесо (seek ±10с по спец.) |
| D4 | OnMFEnd из потока — гонка | ✅ `PostMessage(WM_PLAYER_TRACK_END)`, UI-поток |
| D5 | DS: тейрдаун потока событий | ✅ `DS_StopEventThread` перед `DS_ReleaseGraph` |
| D6 | COM в потоке | ✅ `CoInitializeEx(COINIT_APARTMENTTHREADED)` в `EventThread` (DS) и в `DSPlayer_Open` (F11) |
| D7 | itm_next кодировка | ✅ ушло вместе с auto-advance через `WM_PLAYER_TRACK_END` (PlayIndex) |
| D8 | Тёмная тема | ✅ `isDarkMode` + `ApplyTheme` + `lc_newparams` |
| D9 | Манифест | ⚠️ `resources.rc` есть, но **манифест отключён из-за SxS** (PROJECT_CONTEXT) → см. R5 |
| D10 | Aspect ratio | ✅ letterbox в `UpdateLayout`/`ToggleFullscreen` (через `videoAr`); `pVideoCtrl==NULL` на этой системе — `GetAspectRatio`=0 → 16:9 по умолчанию |
| D11 | Плейлист нередактируемый | ✅ NM_DBLCLK, Del, Enter, Ctrl+↑/↓, сортировка |
| D12 | Утечка при пустой директории | ✅ `if (count == 0) { free…; return; }` |
| D13 | realloc без проверки | ✅ F3-паттерн «последовательный realloc» во всех 6 местах |
| D14/D15 | DblClick/Fullscreen | ✅ `ToggleFullscreen` (попап + репарентинг), F11/Esc/dblclick |
| D16 | Клавиатура | ✅ Space/S/←/→/↑/↓/M/F11/L/I/Esc (с учётом перехвата TC) |
| D17 | Always On Top | ❌ осознанно не реализовано (убрано из меню, F13 M2) |
| D18 | Fluent-иконки | ✅ Segoe UI Symbol глифы ⏮▶■⏭⏪⏩↻☰ |
| D19 | Мёртвый параметр hListerWnd | ⚠️ остался `/*hListerWnd*/` (закомментирован) — косметика, см. Q7 |
| D20 | Тёмная тема в lc_newparams | ✅ |
| D21 | WM_SETFOCUS всегда на видео | ✅ фокус на плейлист при showPlaylist |

Плюс внедрённые F1–F16 (FIXES.md, 2026-08-09) — подтверждены чтением кода: F1 парсер+M9, F2 `_tcsncpy_s`, F3 realloc, F4 индекс в ListLoadNextW, F5 m4a, F6 рефкаунт MF, F7 RecreateVideoWindow, F8 SameDirectory, F9 мёртвая ветка, F10 ftLastWriteTime, F11 COM, F12 мёртвый код, F13 M2/M4/M5/M6/M10, F14 playlistDirty, F15 тесты.

---

## Часть 2. Находки (приоритет 1 — корректность)

### C1. [BUG] `ListLoadNextW` и `PlayIndex` расходятся в ветке needsDS: безусловное пересоздание окна при DS→DS

**Где:** `ListLoadNextW` (~стр. 2450) vs `PlayIndex` (~стр. 1130).

**Почему это важно (из 0.2):** пересоздание `hVideoWnd` нужно, чтобы сбросить **MF/D3D-состояние на HWND**. Когда предыдущий файл тоже игрался через DS, MF-состояния на окне нет — пересоздание теряет VMR-9-окно старого (уже остановленного) графа и добавляет мерцание.

**Текущий код:**
- `PlayIndex`: `if (needsDS) { if (!state->useDirectShow) { MFPlayer_Destroy; RecreateVideoWindow; … } DSPlayer_Stop; DSPlayer_Open; }` — **guard есть** (`!useDirectShow`).
- `ListLoadNextW`: `if (needsDS) { MFPlayer_Destroy; RecreateVideoWindow; … DSPlayer_Open; }` — **guard нет**, пересоздание безусловно.

TC-навигация n/p (ListLoadNextW) и внутреннее переключение (PlayIndex) — **две копии одной машины состояний**, и они уже разошлись. Это и баг (лишний churn на DS→DS в n/p), и признак дублирования (см. M1).

**Фикс (минимальный, по образцу PlayIndex):**
```cpp
// ListLoadNextW, ветка needsDS:
if (!prevWasDS) {                       // выходим из MF — нужен чистый HWND
    MFPlayer_Destroy(state->pMFPlayer);
    RecreateVideoWindow(state);
    state->pMFPlayer = MFPlayer_Create(state->hVideoWnd, OnMFEnd, state);
}
DSPlayer_SetVideoWnd(state->pDSPlayer, state->hVideoWnd);   // синк всегда
hr = DSPlayer_Open(state->pDSPlayer, FileToLoad);
```
Проверка: DS→DS (OGG→OGG) — окно не пересоздаётся, `DSPlayer_Open` сам отвязывает старый граф (`DS_ReleaseGraph`). MF→DS — пересоздание происходит (prevWasDS==FALSE). **[TC-тест]** OGG→OGG→MP4→OGG.

### C2. [BUG] Асинхронные ошибки MF (MEDIAITEM_CREATED_FAILED / ERROR) молча игнорируются

**Где:** `MFCallback::OnMediaPlayerEvent` (обрабатывает только `MFP_EVENT_TYPE_PLAYBACK_ENDED`); следствие — `MFPlayer_Open` возвращает S_OK до того, как media item реально создан.

**Почему это важно:** для битого/неподдерживаемого файла `MFPCreateMediaPlayer` и `Play()` возвращают S_OK (создание item — асинхронное), `isPlaying=TRUE`, затем приходит `MEDIAITEM_CREATED_FAILED` — и **ничего** не происходит: нет перехода к следующему треку, нет DS-fallback (он срабатывает только при синхронном `FAILED(hr)`), UI показывает «Playing» с нулевой позицией. Это ровно тот случай, ради которого существует механизм «skip unplayable» в `PlayIndex` — но он не подключён к асинхронному пути.

**Фикс:** трактовать оба события как конец трека (маршрут уже безопасен — `PostMessage` в UI-поток, флаг `destroying`):

```cpp
if (pEventHeader->eEventType == MFP_EVENT_TYPE_PLAYBACK_ENDED ||
    pEventHeader->eEventType == MFP_EVENT_TYPE_ERROR ||
    pEventHeader->eEventType == MFP_EVENT_TYPE_MEDIAITEM_CREATED_FAILED) {
    InterlockedExchange(&m_p->isPlaying, FALSE);
    InterlockedExchange(&m_p->isPaused,  FALSE);
    if (!InterlockedCompareExchange(&m_p->destroying, 0, 0))
        if (m_p->onEnd) m_p->onEnd(m_p->userData);
}
```
Результат: битый файл → `WM_PLAYER_TRACK_END` → `PlayIndex` → следующий трек (или остановка, если его открыть нельзя — там уже цикл попыток). Поведение при нормальном воспроизведении не меняется.

### C3. [BUG] Курсор остаётся скрытым при закрытии окна во время фуллскрина

**Где:** `ToggleFullscreen` (вход `ShowCursor(FALSE)`, выход `ShowCursor(TRUE)`), `cbNewMain::WM_DESTROY` (фуллскрин-попап уничтожается без `ShowCursor(TRUE)`), `FullscreenWndProc` (нет обработки `WM_CLOSE`).

**Почему это важно:** `ShowCursor` — счётчик в рамках потока. Закрытие листера/вкладки, пока `isFullscreen==TRUE` (в т.ч. Alt+F4 по попапу), уничтожает попап и его детей (`hVideoWnd`), но не возвращает курсор. Сценарий из `WM_DESTROY`: `state->isFullscreen && state->hFullscreenWnd` → `SetParent(hVideoWnd, hWnd)` + `DestroyWindow(hFullscreenWnd)` — с уже уничтоженными дескрипторами это безопасно (возвращают FALSE), но `ShowCursor(TRUE)` отсутствует → курсор пропадает до конца жизни потока (в т.ч. в TC).

**Фикс (два дешёвых изменения):**
1. В `FullscreenWndProc` — штатный выход вместо уничтожения:
```cpp
case WM_CLOSE:
    if (state)
        SendMessage(state->hMainWnd, WM_COMMAND, IDM_FULLSCREEN, 0); // аккуратный выход
    return 0;
```
2. В `cbNewMain::WM_DESTROY` — страховка:
```cpp
if (state->isFullscreen && state->hFullscreenWnd) {
    SetParent(state->hVideoWnd, hWnd);
    DestroyWindow(state->hFullscreenWnd);
    ShowCursor(TRUE);          // восстановить счётчик
}
```
`ShowCursor(TRUE)` идемпотентен (возвращает -1, если курсор уже видим) — двойной вызов безопасен. **[TC-тест]** F11 → Alt+F4.

### C4. [BUG] `UpdateLayout`: деление на ноль при `contentH == 0`

**Где:** `UpdateLayout` (~стр. 640): `int contentH = h - tbH - pad - statusH; if (contentH < 0) contentH = 0;` затем `double contentAr = (double)w / (double)contentH;`.

**Почему это важно:** при высоте окна 60–66 px (guard `if (w < 100 || h < 60) return;` пропускает) `contentH == 0` → `w/0 = inf` → ветка `ar > inf` всегда false → `vw = 0`, видео в окно 0×0. Не крах (double), но деградация.

**Фикс:**
```cpp
int contentH = h - tbH - pad - statusH;
if (contentH <= 0) {
    if (state->hVideoWnd) ShowWindow(state->hVideoWnd, SW_HIDE);
    if (state->hPlaylist) ShowWindow(state->hPlaylist, SW_HIDE);
    return;                       // тулбар/статус уже расставлены выше
}
```
Поведение при нормальных размерах не меняется.

### C5. [BUG-layout] Сикбар наезжает на слайдер громкости на узких окнах

**Где:** `UpdateLayout` (~стр. 625): `int seekW = volX - seekX - 4; if (seekW < 40) seekW = 40;`.

**Почему это важно:** при `w < ~412 px` `seekW` принудительно 40, но `seekX + 40 > volX` — сикбар рисуется поверх `hVolSlider` (который позиционируется после, по `volX`). Слайдер громкости становится недостижимым.

**Фикс:** кламп по правой границе:
```cpp
int seekW = volX - seekX - 4;
if (seekW < 40) seekW = 40;
if (seekW > volX - seekX) seekW = volX - seekX;   // не заезжать на слайдер
if (seekW < 0) seekW = 0;
```

---

## Часть 3. Находки (приоритет 2 — сопровождаемость/тесты)

### M1. [MAINT] Машина переключения движков продублирована: `PlayIndex` vs `ListLoadNextW`

Полный цикл «Stop → (нужен ли DS) → Destroy/Recreate/Create → Open → Play → ApplyVolume → duration/videoAr → флаги → обновления UI» написан **дважды** (PlayIndex ~60 строк, ListLoadNextW ~80 строк) и уже разошёлся (см. C1: guard `!useDirectShow` есть только в PlayIndex). Плюс третий, упрощённый вариант — в `ListLoadW`.
**Решение:** вынести в общий `static BOOL SwitchToFile(PluginState*, const TCHAR* path, BOOL fromTC)` (возвращает успех), использующий `state->useDirectShow` как единственный источник истины; `ListLoadNextW` передаёт `prevWasDS` для сохранения текущего поведения входа. Это устраняет класс расхождений (C1) и упрощает будущие правки VP9-логики. **[TC-тест]** после рефакторинга: F3, n/p, Prev/Next, QuickView, OGG/MP4/AVI-смесь.

### M2. [MAINT] Цикл «собрать выделенные файлы из LCLListBox» написан трижды

`RequestSelectedFiles` (~600), append-ветка ListLoadW «в существующее окно» (~2420), append-ветка ListLoadW «в новое окно» (~2640) — один и тот же блок: `LB_GETSELCOUNT/ITEMS → LB_GETTEXT → ParseTCFileName → проверка ext → GetFileAttributesEx → dup-check → append`. F1 централизовал только парсер.
**Решение:** одна функция `static int CollectSelectedFiles(HWND hListerWnd, const TCHAR* dir, TCHAR*** outFiles, FILETIME** outDates, BOOL dupCheckWithState)`; три ветки зовут её и применяют результат. Меньше мест для рассинхрона (как уже произошло с M9-проверкой существования — она есть не во всех копиях в равном виде).

### M3. [MAINT] `test_verify.py`: сводка «PASS» печатается безусловно, нет exit-кода

**Где:** `test_verify.py`, блок «TEST SUMMARY».
Проверки выше печатают OK/FAIL по факту, но строки `print("  T01: … - PASS")` — литералы. Упавший тест всё равно покажет PASS и вернёт 0.
**Фикс:** аккумулировать `results[name] = bool(ok)` в каждой проверке; в конце `failed = [k for k,v in results.items() if not v]; print("ALL PASSED" if not failed else f"FAILED: {failed}"); sys.exit(1 if failed else 0)`.

### M4. [TEST] Мусорные диагностические скрипты `test_parse2.py`, `test_debug.py`

- `test_parse2.py`: алгоритм `rsplit(' ', 4)` + regex **неверен** — на реальных данных имя включает размеры (`03 - Regression.mp3 10 865`); ни одного assert.
- `test_debug.py`: трассировка без assert.
Оба выглядят «зелёными», ничего не проверяя. **Решение:** удалить (полезные кейсы уже в `test_parse.py`/`test_parse_c.py`).

### M5. [TEST] `test_bug.py` не различает buggy и fixed

Обе функции на всех кейсах дают одинаковый правильный результат — тест не ловит регрессию, ради которой написан. **Решение:** добавить кейс, разделяющий версии (строка, где финальный trim по stale `before_len` отличается), и явный assert на неравенство; если такой кейс не находится — честно переименовать в «инвариант парсера», а не держать за регрессионный.

### M6. [TEST] Нет покрытия реального TC-формата: NBSP (U+00A0) и TAB (U+0009)

`ParseTCFileName` (C) обрабатывает три разделителя через `IsTCSeparator`, а все Python-тесты используют только обычные пробелы. По PROJECT_CONTEXT реальный TC выдаёт NBSP между группами размера и TAB перед датой — главный риск парсера не покрыт.
**Решение:** добавить в `test_parse.py` кейсы `"name.mp3 12\u00A0681\u00A0905\u000902.10.2021 18:57 -a--"` и т.п. (и привести `parse_tc_line` к тем же трём разделителям).

### M7. [MAINT] `OutputDebugString` остаются в релизном коде

~13 вызовов: `EnumFindLCLListBox` (2), `RequestSelectedFiles` (2), `ListLoadW` (3), `ListLoadNextW` (5), `MFPlayer_Open/Play` (3). PROJECT_CONTEXT §8 прямо требует их убрать («нужно убрать перед релизом»); последний коммит (`d7d0e94`) вычистил только `UpdateListerTitle`. Горячий путь — n/p-навигация.
**Решение:** удалить все (или под `#ifdef _DEBUG`). Поведение не меняется.

### M8. [MAINT] Глобал `s_fixMaximizeWnd` общий для всех окон плагина

Таймер 9998 каждой вкладки читает один глобал — при двух вкладках окно A может «чинить» родителя окна B (работает только благодаря идемпотентности операции).
**Решение:** перенести в `PluginState` (`HWND fixMaximizeWnd`), в `WM_TIMER` читать `state->fixMaximizeWnd`. **[TC-тест]** — две вкладки MediaShow2.

### M9. [MAINT] Непроверенные `calloc` в hot-путях

`ListLoad`/`ListLoadNext` (ANSI-обёртки), `RequestSelectedFiles` (`selItems`, `files`, `fileDates`), append-блоки. OOM → падение хоста (TC). F3 закрыл realloc-пути, но голые `calloc` остались.
**Решение:** проверки в трёх местах с крупнейшими буферами (обёртки: `if (!w) return NULL/LISTPLUGIN_ERROR;`; `RequestSelectedFiles`: единый guard с free). Поведение при OOM — «файлы не добавлены», а не краш.

### M10. [MAINT] Связывание библиотек: `#pragma comment(lib)` в .cpp + `target_link_libraries` в CMake

`mfreadwrite`, `ole32`, `oleaut32`, `comctl32`, `uxtheme` дублируются в обоих механизмах. Работает для MSVC, но конфигурация размазана по 4 файлам.
**Решение:** перенести все библиотеки в `target_link_libraries` (mfplat, mf, mfplay, mfuuid, strmiids + уже перечисленные), убрать прагмы. Проверка — сборка.

### M11. [MAINT] Мёртвое/полуживое

- `EnumFindData.processId` — не читается.
- `BuildPlaylist(state, HWND /*hListerWnd*/, …)` — параметр закомментирован, зовётся с NULL (D19): удалить параметр.
- В корне: `test_debug.c`, `test_tb2.cpp`, `test_trackbar.cpp` — экспериментальные файлы вне пайплайна и документации; `test_debug.exe/.obj` gitignored. Удалить исходники.
- `_check_wlx.py` + `_extracted_check.dll` — одноразовая диагностика (DLL уже в .gitignore; скрипт можно удалить или задокументировать).

---

## Часть 4. Исправление ошибок первой версии этого аудита

Прозрачно фиксирую, что в v1 было неверно и почему:

1. **«WM_PLAYER_TRACK_END: статус-бар навсегда показывает Playing» — ОТОЗВАНО.** Я не учёл таймер-поллинг `WM_TIMER` (id=1, 500 мс), который **безусловно** вызывает `UpdateStatus`/`UpdateSeekbar`. После финала последнего трека статус сам станет «Stopped» в течение 500 мс. Остаётся только косметика: `position` не сбрасывается в 0 и сикбар остаётся на 100% (позиция = конец файла). В v1 это была «баг №1» — неверно; в v2 её нет вообще.
2. **«QuickView: старая вкладка не закрывается» — ПОНИЖЕНО до наблюдения.** Трассировка показала: prep-блок (`MFPlayer_Destroy`+`RecreateVideoWindow`) трогает **только старое окно** (`existState`), а новое создаётся с чистым состоянием. Предложенный в v1 `PostMessage(WM_CLOSE)` старому окну — рискованное вмешательство в чужой жизненный цикл без TC-инструментовки. Остаётся как вопрос для [TC-тест]: кто и когда уничтожает старое окно в QuickView.
3. **«Лишний RecreateVideoWindow при DS→DS» — переформулирован в C1:** в `PlayIndex` этого лишнего пересоздания нет (guard `!useDirectShow`), оно есть только в `ListLoadNextW` — это **расхождение двух копий**, а не общий дефект пересоздания.
4. **«Курсор не восстанавливается» — уточнён сценарий (C3):** вход через закрытие попапа/листера во время фуллскрина; добавлен также `WM_CLOSE` в `FullscreenWndProc`.
5. **Не понял историю движков:** v1 цитировала `deep_analysis.md` («оба плеера DirectShow») как актуальное, не проверив, что `mf_player.cpp` с тех пор переписан на настоящий MF (F6-рефкаунт, MFCallback, `MFStartup`). v2 строит анализ на текущем коде, а документацию использует только как историю решений (Часть 0, таблица коммитов).

---

## Часть 4.5. Внедрение правок по аудиту (2026-08-15)

Все решения предварительно проработаны и проверены на последствия; код менялся только после этого.

| Пункт | Статус | Проверка |
|---|---|---|
| C1 — guard `!prevWasDS` в needsDS-ветке ListLoadNextW (DS→DS не пересоздаёт окно; `DSPlayer_Open` сам отвязывает старый граф) | ✅ внедрено | сборка x86+x64 чистая; **[TC-тест]** n/p OGG→OGG |
| C2 — `MFP_EVENT_TYPE_ERROR` и `MEDIAITEM_CREATED` с `FAILED(hrEvent)` трактуются как конец трека | ✅ внедрено | сборка; по SDK 10.0.19041 `MEDIAITEM_CREATED_FAILED` **не существует** — провал создания item приходит как `MEDIAITEM_CREATED`+`hrEvent` (см. Н2) |
| C3 — `WM_CLOSE` в FullscreenWndProc (аккуратный выход) + `ShowCursor(TRUE)` в WM_DESTROY | ✅ внедрено | сборка; **[TC-тест]** F11 → Alt+F4 |
| C4 — guard `contentH <= 0` в UpdateLayout (скрыть контент вместо 0×0) | ✅ внедрено | сборка |
| C5 — кламп `seekW` по `volX - seekX` | ✅ внедрено | сборка |
| M3 — test_verify.py: честные PASS/FAIL + `sys.exit(1)` | ✅ внедрено | **доказал себя**: поймал мою же ошибку («DLL внутри .wlx заканчивается на .dll» — на самом деле имя `MediaShow2.wlx`/`.wlx64`, см. Н3) |
| M4 — удалены `test_parse2.py`, `test_parse3.py`, `test_debug.py`, `test_debug.c`, `test_tb2.cpp`, `test_trackbar.cpp` | ✅ внедрено | git rm; не входят в пайплайн |
| M5 — test_bug.py: задокументировано, что buggy/fixed неразличимы (финальный trim по stale before_len невыполним: символ на границе после trim не пробел) | ✅ внедрено | анализ + комментарий в файле |
| M6 — test_parse.py: кейсы с NBSP (U+00A0)/TAB (U+0009) + явные ожидания имён | ✅ внедрено | ALL TESTS PASSED |
| M8 — `s_fixMaximizeWnd` перенесён в `PluginState` (per-window) | ✅ внедрено | сборка; **[TC-тест]** две вкладки |
| M9 — проверки `calloc` (ANSI-обёртки, RequestSelectedFiles) | ✅ внедрено | сборка |
| M10 — библиотеки перенесены из `#pragma comment(lib)` в CMake (`mfplat mf mfplay mfuuid strmiids` + уже бывшие) | ✅ внедрено | обе сборки без pragma-дублей |
| M11 — мёртвый код: `EnumFindData.processId`, параметр `hListerWnd` у BuildPlaylist | ✅ внедрено | сборка |

**Новые наблюдения (документируются для проверки):**

- **Н1.** `DSPlayer_Play` при первом `Run` для webm вернул **S_FALSE (0x1)** (позиция при этом растёт). По MSDN S_FALSE = «граф уже запущен»; для свежесозданного графа неожиданно. Вреда не видно (SUCCEEDED → isPlaying=TRUE), но причина не установлена — проверить при TC-прогоне (не влияет на воспроизведение).
- **Н2.** SDK 10.0.19041 `mfplay.h`: `MFP_EVENT_TYPE_MEDIAITEM_CREATED_FAILED` не существует; провал создания media item приходит как `MFP_EVENT_TYPE_MEDIAITEM_CREATED` с `header.hrEvent != S_OK` (структура `MFP_MEDIAITEM_CREATED_EVENT` без поля hrStatus). Первая редакция C2 компилировалась с несуществующей константой — поймано сборкой.
- **Н3.** Внутри `.wlx`-архива DLL лежит под именем `MediaShow2.wlx`/`.wlx64` (не `.dll`) — `package.py` так и упаковывает. Первая версия проверки test_verify искала `.dll` — новая версия теста это поймала (сработал exit-код).

**Отложено (причина):**

| Пункт | Причина откладывания |
|---|---|
| M1 (единый SwitchToFile), M2 (единый CollectSelectedFiles) | Рефакторинг критичного VP9/переключательного кода; без TC-валидации риск утраты функционала (принцип пользователя). C1 уже закрывает фактическое расхождение минимально |
| M7 (удаление OutputDebugString) | Инструментарий нужен для текущих TC-проверок; снять перед релизом (задокументировано в PROJECT_CONTEXT §8) |
| Q9 (/W4+/WX) | Потребует чистки предупреждений; низкий приоритет |
| Q10 (DPI-манифест) | Требует TC-проверки SxS-загрузки |
| B1 (run_checks.py), B2 (build_test.bat) | Инфраструктура, не влияет на корректность |

---

## Часть 5. Остаточные риски (не чинятся безопасно/автоматически)

1. **[TC-тест] Быстрые ↓/↑ в TC** (VP9→H.264): F7 фаза 2 внедрена, риск исторического хэнга; откат к `79b7220` при регрессе (FIXES.md). В этой среде не проверяемо.
2. **[KNOWN] Узкая гонка в MF-колбэке** (F6, FIXES.md §7): окно закрывается точно в момент финального PLAYBACK_ENDED → `onEnd → state->hMainWnd` после освобождения. Полное решение — рефкаунт PluginState; намеренно отложено.
3. **[KNOWN] M3:** `EnumFindLCLListBox` при двух панелях TC может взять не ту (нет привязки к активной панели).
4. **[KNOWN] DPI:** манифест отключён из-за SxS (resources.rc) — на HiDPI интерфейс масштабируется системой (размытие). Включение требует TC-проверки загрузки.
5. **[RESIDUAL] DS→MF:** старый DS-граф (VMR-9 с D3D-устройством) после `DSPlayer_Stop` не освобождается до следующего `DSPlayer_Open`/`Destroy` — удержание видеопамяти на время MF-воспроизведения. Освобождение графа сразу при уходе из DS — кандидат на отдельный фикс (аккуратно: `DS_ReleaseGraph` в переключении).
6. **[RESIDUAL] switchInProgress-кулдаун** может проглотить `WM_PLAYER_TRACK_END` очень короткого трека (< 500 мс) — воспроизведение просто остановится. Сознательный компромисс против хэнгов.
7. **x86-артефакт устарел** (DLL от 2026-07-25): пересобрать оба и переупаковать `package.py` перед дистрибуцией (сейчас `MediaShow2.wlx` в корне не соответствует исходникам).

---

## Часть 6. Допущения

1. Поведение TC (кто уничтожает старое окно QuickView; перехват клавиш; точный формат LB_GETTEXT) — по PROJECT_CONTEXT; точные форматы NBSP/TAB в тестах не покрыты (M6) — при первом живом прогоне сверить.
2. `MFP_EVENT_TYPE_ERROR`/`MEDIAITEM_CREATED_FAILED` трактуются как «конец трека» (авто-переход), согласовано с DS-путем (EC_COMPLETE). Если желаема остановка с ошибкой — не звать `onEnd`.
3. Результаты сборки/тестов валидны для текущего окружения (VS 2022 Community, MSBuild 17.14.51, MSVC 14.44.35207); `pwsh`-сообщение в конце сборки — внешний импорт MSBuild, не проект.
4. Код не изменялся (требование ReviewPrompt); все фиксы — готовый код для внедрения.
5. `[FIXED]`-статусы в Части 1 — только там, где подтверждено чтением текущего кода.

---

## Приложение: карта ревью

| Файл | Роль | Пункты |
|---|---|---|
| `src/dllmain.cpp` (2952) | TC API, UI, плейлист, переключения, тайтл | C1, C3–C5, M1, M2, M7–M9, M11 |
| `src/mf_player.cpp` (339) | MF-движок (реальный MF) | C2, M7, M10, риск 2 |
| `src/ds_player.cpp` (367) | DS fallback (VMR-9) | C1, M10, риск 5 |
| `src/plugin_api.h` | константы | — (вычищено F12) |
| `CMakeLists.txt` | сборка | M10, B-пункты |
| `test_parse.py`, `test_parse_c.py`, `test_bug.py` | регрессия парсера | M5, M6 |
| `test_verify.py` | PE/архивы | M3 |
| `test_parse2.py`, `test_debug.py`, `test_debug.c`, `test_tb2.cpp`, `test_trackbar.cpp` | мусор | M4, M11 |
| `package.py` | упаковка | риск 7 |

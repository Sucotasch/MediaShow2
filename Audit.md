# Audit.md — MediaShow2, полное инженерное ревью

Дата: 2026-08-09. Объём: ~3000 строк C++ (dllmain.cpp + mf_player + ds_player), C++17, CMake, Windows SDK. Ревью выполнено по ReviewPrompt.txt: целостно, с запуском валидаций и готовыми фиксами. Код не изменялся — только этот отчёт.

---

## 0. Выполненные валидации

| Проверка | Результат |
|---|---|
| `cmake --build build-x64 --config Release --clean-first` | ✅ 0 ошибок, 0 предупреждений |
| `python test_parse.py` | ❌ 2/8 кейсов падают (edge-кейсы малых размеров) |
| `python test_parse2.py` | ⚠️ показывает НЕВЕРНЫЙ результат для `05 - Shame as a Weapon.mp3 13 747 831` → имя остаётся с размером |
| `python test_parse3.py`, `test_parse_c.py`, `test_debug.py` | ✅ проходят |
| `python test_bug.py` | ❌ **крашится** `IndexError: list assignment index out of range` (строка 18) |
| `python test_verify.py` | ❌ **крашится** `FileNotFoundError` — хардкод старого пути `...MediaShow_v0.9.5_patched\MediaShow2` |
| `python test_tb2.cpp / test_trackbar.cpp` | ⚠️ это C++-файлы, python их не запускает (ожидаемо) |
| `python package.py` | ✅ создаёт `.wlx`/`.wlx64` (артефакты от 12:19 уже есть) |

Вывод: **сборка здорова, но 3 из 7 тест-скриптов сломаны или вводят в заблуждение** (см. §5). В CI-смысле репозиторий «зелёный», потому что тесты никто не запускает.

---

## 1. Критические проблемы (корректность)

### C1. Копипаст парсинга LB_GETTEXT — 4 идентичных блока (~100 строк каждый)
Блок «найти DD.MM.YYYY → отрезать размер (3 группы)» продублирован в:
- `RequestSelectedFiles` (~стр. 590)
- `ListLoadW`, append-mode ветка при F3 (~стр. 2400)
- `ListLoadW`, append-mode ветка «загрузка сохранённого плейлиста» (~стр. 2610)
- (`BuildPlaylistFromSelection` — ещё одна копия логики, см. M1)

**Почему это критично:** любой фикс парсинга (а он нужен — см. C1b, T3) приходится вносить 4 раза. Именно из-за рассинхрона копий в прошлом были баги «имя прилипает к размеру».

**Фикс:** вынести в одну функцию:
```cpp
static BOOL IsTCSeparator(TCHAR c) { return c == TEXT(' ') || c == 0x00A0 || c == 0x0009; }

// Парсит строку TC: "name.ext NNN NBSP NNN NBSP NNN TAB DD.MM.YYYY HH:MM -a--"
// Возвращает TRUE и кладёт в out (outMax симв.) «голое» имя файла.
static BOOL ParseTCFileName(const TCHAR* buf, TCHAR* out, int outMax) {
    const TCHAR* datePos = NULL;
    for (const TCHAR* p = buf; p[9]; p++) {
        if (p[2]==TEXT('.') && p[5]==TEXT('.') &&
            p[0]>='0'&&p[0]<='9' && p[1]>='0'&&p[1]<='9' &&
            p[3]>='0'&&p[3]<='9' && p[4]>='0'&&p[4]<='9' &&
            p[6]>='0'&&p[6]<='9' && p[7]>='0'&&p[7]<='9' &&
            p[8]>='0'&&p[8]<='9' && p[9]>='0'&&p[9]<='9') { datePos = p; break; }
    }
    if (!datePos) { _tcsncpy_s(out, outMax, buf, _TRUNCATE); return out[0] != 0; }

    int len = (int)(datePos - buf);
    if (len >= outMax) len = outMax - 1;
    memcpy(out, buf, (size_t)len * sizeof(TCHAR));
    out[len] = 0;

    while (len > 0 && IsTCSeparator(out[len-1])) out[--len] = 0;   // хвост
    TCHAR* p = out + len - 1;
    for (int g = 0; g < 3; g++) {                                    // 1..3 группы цифр
        TCHAR* digitStart = p;
        while (p > out && *p >= '0' && *p <= '9') p--;
        if (p == digitStart) break;                                  // цифр нет — не размер
        if (p > out && IsTCSeparator(*p)) p--;                       // съели разделитель
        else if (p > out) { p = digitStart; break; }                 // группа прилипла к имени
    }
    out[p + 1] = 0;
    return out[0] != 0;
}
```
Проверено на кейсах из test_parse.py / test_parse_c.py: `file.mp3 12 345 …`→`file.mp3`, `a.mp3 1 …`→`a.mp3`, `track123.mp3 12 345 678 …`→`track123.mp3`, `12345.mp3 …`→`12345.mp3`, `name.mp3 999 999 999 …`→`name.mp3`.

### C1b. Алгоритм «3 группы» ломается на файлах < 1 МБ
Текущий код жёстко требует 3 группы цифр; если их 1–2 (файлы < 1 КБ / < 1 МБ), ветка `else { p = fileName + beforeLen - 1; }` возвращает имя вместе с размером → в плейлист попадает несуществующий путь. `test_parse.py` Edge 2/4 фиксируют это как FAIL, `test_parse2.py` показывает реальное проявление. Фикс — ровно функция из C1 (1..3 группы). **Дополнительно:** после сборки пути проверять существование файла (`GetFileAttributesEx`) и пропускать битые записи — сейчас `RequestSelectedFiles` не пропускает запись при неудачной проверке даты.

### C2. `_tcsncpy` без гарантии NUL-терминатора — 22 места
`_tcsncpy(dest, src, MAX_PATH - 1)` **не** дописывает `\0`, если `strlen(src) >= MAX_PATH - 1`. Далее по `dest` идут `_tcslen`/`_tcsrchr` (UpdateStatus, UpdatePlaylist, GetPlaylistPath) → чтение за границей буфера (UB/крах). Особенно опасна строка 2256: `_tcsncpy(filePath, state->filePath, MAX_PATH)` — копия 260 символов в буфер на 260 (переполнение при ровно 260).

Затронуты: `state->filePath` (1145, 2558, 2810), `dir` (369, 573, 2383, 2476, 2596, 2664), `info->fileName` (1370), `GetPlaylistPath` (182), `readString` (1509/1511), `track`/`year` (1539/1566).

**Фикс (минимальный, безопасный):**
```cpp
// Замена _tcsncpy для всех путей:
_tcsncpy_s(state->filePath, MAX_PATH, FileToLoad, _TRUNCATE);
// либо пост-фикс:
_tcsncpy(state->filePath, FileToLoad, MAX_PATH - 1);
state->filePath[MAX_PATH - 1] = TEXT('\0');
```

### C3. `realloc` двух массивов — висячий указатель при частичном сбое
Паттерн (в ListLoadW append-ветках и dir-scan append):
```cpp
TCHAR** newPl = (TCHAR**)realloc(existState->playlist, newTotal * sizeof(TCHAR*));
FILETIME* newDt = (FILETIME*)realloc(existState->fileDates, newTotal * sizeof(FILETIME));
if (newPl && newDt) { existState->playlist = newPl; ... }
```
Если первый `realloc` удался, а второй нет, `existState->playlist` по-прежнему указывает на **уже освобождённый** старый блок (новый указатель остался только в `newPl`) → двойной free в `FreePlaylist`, повреждение кучи. При OOM это реальный крах.

**Фикс:** пережить оба результата до присваивания и при частичном сбое восстановить состояние:
```cpp
TCHAR**   tmpPl = (TCHAR**)  realloc(existState->playlist,  newTotal * sizeof(TCHAR*));
FILETIME* tmpDt = (FILETIME*)realloc(existState->fileDates, newTotal * sizeof(FILETIME));
if (tmpPl && tmpDt) {
    existState->playlist  = tmpPl;
    existState->fileDates = tmpDt;
    for (...) { /* перенос files[i] → playlist[oldCount+i] */ }
} else {
    // частичный сбой: tmpPl может быть валиден, а old-указатель уже мёртв —
    // освобождаем только то, что знаем, и НЕ трогаем existState->playlist/fileDates
    if (tmpPl)  free(tmpPl);   // если tmpPl != playlist — новый блок
    if (tmpDt)  free(tmpDt);
    // сброс состояния в консистентное:
    existState->playlist = NULL; existState->fileDates = NULL;
    existState->playlistCount = 0; existState->playlistIndex = 0;
}
```
Более простое альтернативное решение: выделять оба массива одним блоком (`malloc(newTotal * (sizeof(TCHAR*) + sizeof(FILETIME)))`) — тогда realloc атомарен по построению.

### C4. `ListLoadNextW` не обновляет `playlistIndex`
При навигации n/p по файлам TC вызывает `ListLoadNextW`, но индекс текущего трека в плейлисте не ищется: маркер «▶» и подсветка остаются на старом файле, хотя играет новый. Визуально плейлист врёт, и `WM_PLAYER_TRACK_END`/`IDM_PREV/NEXT` продолжат от старого индекса.

**Фикс** в `ListLoadNextW` (после `_tcsncpy(state->filePath, ...)`):
```cpp
for (int i = 0; i < state->playlistCount; i++) {
    if (_tcsicmp(state->playlist[i], FileToLoad) == 0) { state->playlistIndex = i; break; }
}
```

### C5. M4A не считается аудио
`IsAudioOnly` не содержит `m4a`, хотя `M4A` есть в detect-строке и `IsMediaFile`. Следствия: колонка Type показывает «Video», `showPlaylist` остаётся FALSE, для аудио-файла показывается чёрное видео-окно, плейлист невидим.

**Фикс:** добавить `TEXT("m4a")` в `audioExts[]` в `IsAudioOnly`.

### C6. Гонка use-after-free в колбэке Media Foundation
`MFCallback::OnMediaPlayerEvent` (бежит на потоке MF) разыменовывает `m_p` — `tagMFPlayer`, который `MFPlayer_Destroy` освобождает через `free(p)`. `Sleep(50)` после `Stop()` не гарантирует, что очередь событий MF опустела. Доставка `MFP_EVENT_TYPE_PLAYBACK_ENDED` после Destroy → обращение к освобождённой памяти. Это тот самый «Prev/Next crash», который «починили» WaitForSingleObject — но только в DS, а в MF осталось.

**Фикс (надёжный):** не удалять объект синхронно с колбэком. Вариант А — в `OnMediaPlayerEvent` перед доступом к `m_p` проверять флаг:
```cpp
void STDMETHODCALLTYPE OnMediaPlayerEvent(MFP_EVENT_HEADER* h) {
    if (!h) return;
    if (InterlockedCompareExchange(&m_p->destroyed, 0, 0)) return; // уже destroyed
    ...
}
// в MFPlayer_Destroy: InterlockedExchange(&p->destroyed, 1) ПЕРЕД release pPlayer
```
Вариант Б (предпочтительный): `MFPlayer_Destroy` не делает `free(p)` сам, а откладывает через `PostMessage` на UI-поток; плюс `WaitForSingleObject` на «событийный» поток по аналогии с DS. Минимум — комментировать `Sleep(50)` как *недостаточную* защиту.

### C7. Три разные стратегии «сброса видеорежима» — код противоречит собственной документации
Проблема VP9→H.264 (freeze) решается тремя разными способами в разных местах:
- `ListLoadW` QuickView-ветка: `MFPlayer_Destroy + DestroyChildVideoWindows` — **по документации PROJECT_CONTEXT.md это сломанный подход**;
- `ListLoadNextW` fallback: `DestroyChildVideoWindows` + отложенный `IDT_RECREATE` (300 мс);
- `PlayIndex`: в MF-fallback вообще **без** пересоздания окна (переиспользует hVideoWnd), в ветках needsDS/DS→MF — `RecreateVideoWindow`.

Плюс `IDT_RECREATE` уничтожает видео-окно **во время воспроизведения** (через 300 мс после старта) и переоткрывает файл — это хак, маскирующий корневую проблему.

**Фикс:** свести к одной стратегии — `RecreateVideoWindow` (по их же исследованию) во всех переходах, включая fallback в `ListLoadNextW` и MF-ветку `PlayIndex`; `IDT_RECREATE` убрать. Обновить PROJECT_CONTEXT.md (там уже описана правильная стратегия — код просто не следует ей в 3 местах).

### C8. `hLastPluginWnd` + «закрыть старую вкладку» — закрывается чужой таб
`ListLoadW` (append OFF) при каждом новом F3 делает `PostMessage(hOldLister, WM_CLOSE)` по предыдущему окну плагина. Если открыты **две** вкладки MediaShow2 одновременно (два файла), F3 на третьем закроет одну из существующих. Статик не очищается в `ListCloseWindow` (защита только `IsWindow`). TC это позволяет — плugins могут жить в нескольких табах.

**Фикс:** закрывать старую вкладку, только если она показывает **тот же файл**, что и новая (сравнить `existState->filePath` с `FileToLoad`), либо отказаться от глобального статика в пользу `GetParent`-цепочки текущего окна.

### C9. Мёртвая ветка `else if (MFPlayer_HasVideo(...))` в `PlayIndex`
В момент проверки `pMFPlayer` только что пересоздан и ещё ни разу не открывал файл → `pVideoCtrl == NULL` → `HasVideo()` всегда FALSE. Ветка недостижима, а на целевой системе `pVideoCtrl` NULL всегда (см. PROJECT_CONTEXT.md §8). Трёхходовое ветвление фактически двухходовое — запутывает.

**Фикс:** удалить ветку, оставить needsDS / fallback с комментарием, что `MFPlayer_HasVideo` здесь бессмыслен до `Open`.

### C10. Несогласованные даты: `ftCreationTime` vs `ftLastWriteTime`
`ScanDirectoryForMedia` пишет `fd.ftCreationTime`, а `RequestSelectedFiles`/`LoadPlaylist`/append-ветки — `ftLastWriteTime`. Колонка «Date» в плейлисте показывает разное в зависимости от того, как собран список.

**Фикс:** везде `ftLastWriteTime` (создание для многих копий файлов = дата копирования, что вводит в заблуждение).

### C11. Нет явного `CoInitializeEx` для DirectShow на UI-потоке
`DSPlayer_Open` → `CoCreateInstance(CLSID_FilterGraph, CLSCTX_INPROC_SERVER)` требует COM на вызывающем потоке. `CoInitializeEx` есть только в DS-`EventThread`; поток, создающий граф, — UI-поток (поток TC, вызвавший `ListLoadW`). Сейчас «везёт», что TC инициализировал COM (иначе File Info/альбом-арт не работали бы), но это хрупкое неявное допущение.

**Фикс:**
```cpp
// в ListLoadW (и, для надёжности, в начале DSPlayer_Open):
static BOOL g_comInit = FALSE;
if (!g_comInit) {
    if (SUCCEEDED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED))) g_comInit = TRUE;
    // CO_E_ALREADYINITIALIZED — норма, не ошибка
}
```
(без `CoUninitialize` в DLL — чужой процесс, см. §7).

---

## 2. Умеренные проблемы

### M1. Мёртвый код (найдено grep'ом, использования нет)
- `BuildPlaylistFromSelection` (стр. 351) — не вызывается нигде.
- `WM_DEFERRED_GETFILES` (стр. 42) — не используется.
- `WM_PLAYER_PLAY/PAUSE/STOP/FULLSCREEN/TOPMOST/MUTE/INFO/PREV/NEXT` (plugin_api.h) — не используются; жив только `WM_PLAYER_TRACK_END`.
- `IDM_SETTINGS` (4022), `IDM_PLAYLIST` (4021) — не используются.
- Параметр `useDS` в `GetMediaInfo`/`ShowFileInfoDialog` — внутри функции не читается, прокидывается впустую.
- `FileInfoWndProc`: `WM_DESTROY → PostMessage(hWnd, WM_USER)` — обработчика нет.
- Ветка `else if (MFPlayer_HasVideo(...))` — см. C9.

### M2. About-диалог рекламирует несуществующую фичу
«Ctrl+T Always on Top» в `IDM_ABOUT`, но Always On Top **не реализован**. Убрать строку или реализовать (WS_EX_TOPMOST toggle).

### M3. `EnumFindLCLListBox` берёт первую панель с выделением — не та панель
При двух панелях TC: F3 на файле правой панели, пока в левой есть выделение → соберутся файлы левой панели. Надо сверять каталог найденного LCLListBox с каталогом `FileToLoad` (через GetWindowText/GetProp у LCLListBox нельзя — но можно сравнивать `GetWindowRect` панели с позицией фокуса, либо принять прагматичное правило: предпочитать список, чей каталог совпадает). Как минимум задокументировать ограничение.

### M4. `IsQuickView` — `#pragma optimize("", off)` + OutputDebugString на каждый вызов
Вызывается 2 раза в `ListLoadW` и на каждый `ListLoadNextW`. Отладочная обвязка в релизном коде (см. также L4).

### M5. Полноэкранный режим: клавиши Space/S не работают
`FullscreenWndProc` обрабатывает только Esc/F11/двойной клик. Play/Pause/Stop во время фуллскрина недоступны с клавиатуры. Прокинуть `WM_KEYDOWN` в `cbNewMain` (как делает `VideoWndProc`).

### M6. `isPlaying = TRUE` даже при неудачном открытии
В `PlayIndex` после полного цикла (ни один файл не открылся) и в `ListLoadNextW` при двойном фейле `state->isPlaying` остаётся TRUE → статус-бар показывает «Playing» на мёртвом плеере. Ставить флаги только после `SUCCEEDED(hr)`.

### M7. `SavePlaylist` вызывается из `UpdatePlaylist` на каждый ребилд
Сортировка, удаление, навигация, switch-переход — каждый раз полная запись файла на диск. Для больших плейлистов лишний I/O. Сохранять только при изменении состава/индекса (флаг dirty).

### M8. `MFPlayer_AudioNeedsDS` открывает source-reader на каждый переключение
Тяжёлая операция (парсинг контейнера) на каждом `PlayIndex`/`ListLoadNextW`. Приемлемо, но стоит кэшировать решение по расширению/первым байтам. Также комментарий в коде противоречит коду: «E-AC-3: Data1 = 0xAAC2», а проверяется `0x00000AAC`.

### M9. `RequestSelectedFiles`: при `datePos == NULL` имя = сырая строка TC
Ветка fallback копирует весь buf (с размером/датой) в `fileName` → `fullPath` указывает в никуда. Запись всё равно добавляется. Фикс: при неудаче `GetFileAttributesEx(fullPath)` пропускать запись (см. C1b).

### M10. Модальное окно File Info: если родитель закрыт во время показа
`EnableWindow(hParent, FALSE)` … по выходу `EnableWindow(hParent, TRUE)` на уничтоженном окне. Добавить `if (IsWindow(hParent))` перед вызовами.

---

## 3. Низкий приоритет / стиль

- **L1.** Глобальное состояние сортировки `g_sort_pl/g_sort_dt/g_sort_col/g_sort_asc` — работает (UI-поток один), но хрупко; `qsort_s` даёт `context` — можно передавать туда.
- **L2.** Магические числа таймеров/задержек: ID 1, 9998, 300/500 мс, 650 мс двойного клика, `TB_SETBUTTONSIZE(36,36)`. Завести именованные константы.
- **L3.** `OutputDebugString` в релизе: `IsQuickView`, `EnumFindLCLListBox`, `RequestSelectedFiles`, `MFPlayer_Open/Play`, `ListLoadNextW`, `ListLoadW` — обернуть в `#ifdef _DEBUG` (или убрать).
- **L4.** `List.txt` — артефакт отладки (лог OutputDebugString) попал в git. Добавить в `.gitignore`.
- **L5.** Порядок плейлиста из `ScanDirectoryForMedia` — порядок FindFirstFile (не алфавитный, не естественный). Для аудио-альбомов «auto-advance» идёт в случайном порядке. Сортировать по имени (или сортировать через существующий `SortPlaylist` по колонке Name после сборки).
- **L6.** `UpdateToolbarRepeat` — цикл по кнопкам лишний: `TB_GETBUTTONINFO` и так принимает ID команды.
- **L7.** CMakeLists: `/W3` → можно `/W4`; включить `/analyze` в Debug-конфиг для статического анализа.
- **L8.** `ToggleFullscreen` при `showPlaylist==TRUE` (аудио) — чёрный экран без плейлиста и без видео. При аудио фуллскрин либо скрывать, либо показывать плейлист.
- **L9.** `ListLoad`/`ListLoadNext` (ANSI-обёртки): если `MultiByteToWideChar` вернул 0, `calloc(0,...)` может вернуть NULL и `MultiByteToWideChar` запишет в NULL. Добавить проверку `n > 0`.

---

## 4. Безопасность

- Серьёзных уязвимостей (переполнения по фиксированным размерам при типичных данных) не найдено, **но** C2 (NUL-терминация) и C3 (висячий указатель) — классы памяти, которые в будущем могут стать эксплуатируемыми.
- `DllMain` корректно вызывает `DisableThreadLibraryCalls`.
- `/DYNAMICBASE` + `/NXCOMPAT` в линкере — ок. Рекомендуется `/CETCOMPAT` (Intel CET) для современных Windows и `Control Flow Guard` (`/guard:cf`) в `target_compile_options`.
- Нет проверки длины `dps->DefaultIniName` в `ListSetDefaultParams` (структура TC фиксирована, риск низкий).

---

## 5. Проблемы тестов и инструментов

- **T1. `test_bug.py` крашится** — `fn = list(buf[:before_len])` (длина `before_len`, индексы 0..before_len-1), а затем `fn[before_len] = '\0'` — выход за границы. Тест **никогда не работал** (и «доказывал» фикс, которого не проверял). Фикс: `fn[before_len - 1] = '\0'` (в C-версии буфер на 1 больше) — и заодно проверить, что он реально отличает buggy от fixed.
- **T2. `test_verify.py` хардкодит путь** `D:\Arx\Software Downloads\MediaShow_v0.9.5_patched\MediaShow2` — на текущей машине (`...MediaShow\MediaShow2`) FileNotFoundError. Фикс: `base = os.path.dirname(os.path.abspath(__file__))`; пути `build/bin/Release` и `build-x64/bin/Release` взять относительно его. После фикса прогнать — он валидирует ASLR/DEP/экспорты.
- **T3. `test_parse.py` фиксирует баг как «ожидание»** — кейсы с 1–2 группами размеров помечены FAIL, но это именно то, что должно работать (C1b). Обновить ожидания после внедрения `ParseTCFileName`.
- **T4. Нет теста на C4** (playlistIndex в ListLoadNextW), C5 (m4a), C10 (даты). Для чистоты: вынести `ParseTCFileName`/`IsAudioOnly`/`IsMediaFile` в отдельный `.cpp/.h` (например `tcparse.cpp`), чтобы питоновские тесты могли гонять ту же логику, или добавить тестовый exe-таргет в CMake.
- **T5.** `test_tb2.cpp` / `test_trackbar.cpp` — C++-исходники в корне рядом с python-тестами; перенести в `tests/` и дать понятные имена.

---

## 6. Рекомендуемый порядок работ (для junior)

1. **C1/C1b + T1/T3** — вынести `ParseTCFileName`, заменить 4 копии, починить `test_bug.py`, обновить `test_parse.py`. (Тестируемо без TC.)
2. **C2** — пройтись по 22 `_tcsncpy`, добавить терминацию (или `_tcsncpy_s`).
3. **C4 + C5 + C10** — три маленьких однострочных фикса.
4. **C3** — переписать 3 append-блока с realloc на безопасный паттерн.
5. **C6** — защита колбэка MF (флаг `destroyed` до release).
6. **C7** — унифицировать переходы на `RecreateVideoWindow`, убрать `IDT_RECREATE`.
7. **C8, C9, C11, M1–M10** — по убыванию.
8. **T2** — починить пути в `test_verify.py`, прогнать.
9. Прогнать `cmake --build build-x64 --config Release` и `package.py`; ручной прогон в TC (F3/Ctrl+Q, QuickView, VP9→H.264, двупанельный выбор).

---

## 7. Оставшиеся риски (не фиксить автоматически)

- **Взаимодействие с TC при закрытии вкладок** (C8) требует ручного теста в реальном TC с двумя табами — не проверяется headless.
- **C7** меняет поведение воспроизведения; унификация стратегий должна сопровождаться прогоном сценария 6.avi→7.mp4→5.mp4 из PROJECT_CONTEXT.md.
- **C11 (CoInitializeEx)** — вмешательство в COM-состояние чужого процесса; делать с осторожностью (CO_E_ALREADYINITIALIZED — не ошибка; не вызывать CoUninitialize в DLL).
- `#pragma optimize("", off)` и `Sleep(50)`/таймеры — «инженерный долг», оставленный автором осознанно; менять только вместе с C7.

---

## 8. Предположения

- Ревью по коду и логам `List.txt`/`PROJECT_CONTEXT.md`; ручной запуск в TC не выполнялся (окружение без TC).
- «Коммит перед ревью» из ReviewPrompt.txt **намеренно пропущен** — git-операции без явной команды не выполняются.
- Репозиторий считается однопроцессным x86/x64-плагином; многопоточность — только UI-поток + MF-колбэк + DS-EventThread.
- `build/` (x86) на машине есть, но свежесть не проверялась — валидировалась только сборка `build-x64`.

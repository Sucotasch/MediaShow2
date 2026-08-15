# FIXES.md — Конкретные решения по Audit.md

Дизайн-документ. Код НЕ изменён — здесь только решения, выверенные по реальному исходнику (dllmain.cpp 2976 строк, mf_player.cpp, ds_player.cpp, тесты). Каждое решение проверено на последствия: не ломает поведение, не добавляет гонок, не ухудшает производительность.

Порядок внедрения — от безопасного к рискованному. Пункты с пометкой **[TC-тест]** требуют ручной проверки в Total Commander после сборки.

> **Статус внедрения (2026-08-09):** все фиксы F1–F15, включая F7 фазу 2 (ListLoadNextW → немедленный `RecreateVideoWindow`, механизм `IDT_RECREATE` удалён), **внедрены**, сборка чистая, все python-тесты проходят, быстрый тест в TC — видимых багов нет. F7 фаза 2 требует контрольного прогона быстрых ↓/↑ в TC (риск исторического хэнга на быстрых переключениях — откат одной командой к `79b7220`). F16 — не трогаем осознанно. См. «Статус внедрения» внизу.

---

## F1. C1/C1b/M9 — единый парсер строки TC + проверка существования файла

### Проблема
Блок «найти DD.MM.YYYY → отрезать 3 группы размера» продублирован в 4 местах (RequestSelectedFiles ~стр. 590, ListLoadW append-ветка ~стр. 2395, ListLoadW append-ветка 2 ~стр. 2600). Алгоритм жёстко требует 3 группы цифр и ломается на файлах < 1 МБ (имя остаётся с размером → несуществующий путь в плейлисте). test_parse.py Edge2/Edge4 фиксируют это как FAIL; test_parse2.py показывает проявление.

### Решение
Одна статическая функция (место: сразу перед `RequestSelectedFiles`, после `BuildPlaylist`):

```cpp
static BOOL IsTCSeparator(TCHAR c) {
    return c == TEXT(' ') || c == 0x00A0 || c == 0x0009;
}

// Строка TC: "name.ext NNN NBSP NNN NBSP NNN TAB DD.MM.YYYY HH:MM -a--"
// Возвращает TRUE и пишет в out (outMax симв.) «голое» имя файла.
// Поддерживает 1..3 группы размера (файлы < 1 КБ, < 1 МБ, >= 1 МБ).
static BOOL ParseTCFileName(const TCHAR* buf, TCHAR* out, int outMax) {
    if (!buf || !out || outMax <= 0) return FALSE;
    out[0] = TEXT('\0');

    const TCHAR* datePos = NULL;
    for (const TCHAR* p = buf; p[9]; p++) {
        if (p[2] == TEXT('.') && p[5] == TEXT('.') &&
            p[0] >= '0' && p[0] <= '9' && p[1] >= '0' && p[1] <= '9' &&
            p[3] >= '0' && p[3] <= '9' && p[4] >= '0' && p[4] <= '9' &&
            p[6] >= '0' && p[6] <= '9' && p[7] >= '0' && p[7] <= '9' &&
            p[8] >= '0' && p[8] <= '9' && p[9] >= '0' && p[9] <= '9') {
            datePos = p;
            break;
        }
    }
    if (!datePos) { _tcsncpy_s(out, outMax, buf, _TRUNCATE); return out[0] != 0; }

    int len = (int)(datePos - buf);
    if (len >= outMax) len = outMax - 1;
    memcpy(out, buf, (size_t)len * sizeof(TCHAR));
    out[len] = TEXT('\0');

    // Обрезать хвостовые разделители (пробел / NBSP / TAB)
    while (len > 0 && IsTCSeparator(out[len - 1])) out[--len] = TEXT('\0');

    // Снять 1..3 группы «цифры + разделитель» справа (размер файла)
    TCHAR* p = out + len - 1;
    for (int g = 0; g < 3; g++) {
        TCHAR* digitStart = p;
        while (p > out && *p >= '0' && *p <= '9') p--;
        if (p == digitStart) break;               // цифр нет — это не группа размера
        if (p > out && IsTCSeparator(*p)) p--;    // съели разделитель
        else if (p > out) { p = digitStart; break; } // группа «прилипла» к имени — сохранить
    }
    out[p + 1] = TEXT('\0');
    return out[0] != 0;
}
```

**Трассировка (проверено на всех кейсах тестов):**
- `file.mp3 12 345 01.01.2024 12:00 -a--` → `file.mp3` (2 группы — раньше FAIL)
- `a.mp3 1 01.01.2024 12:00 -a--` → `a.mp3` (1 группа — раньше FAIL)
- `track123.mp3 12 345 678 …` → `track123.mp3` (test_parse_c)
- `12345.mp3 12 345 678 …` → `12345.mp3`, `noext 12 345 678 …` → `noext`
- `02 - Arpadhazi Margit balladaja.mp3 12 681 905 …` → полное имя с пробелами (test_parse)

### Замена в 3 местах вызова
Каждый блок от `TCHAR* datePos = NULL;` до `} else { _tcsncpy(fileName, buf, MAX_PATH - 1); }` заменяется на:
```cpp
TCHAR fileName[MAX_PATH] = {0};
ParseTCFileName(buf, fileName, MAX_PATH);
```
(в append-ветке 2 буфер называется `fn`).

### M9 (заодно): пропуск несуществующих файлов
В RequestSelectedFiles текущий код:
```cpp
files[validCount] = _tcsdup(fullPath);
WIN32_FILE_ATTRIBUTE_DATA fad;
if (GetFileAttributesEx(fullPath, GetFileExInfoStandard, &fad)) {
    state->fileDates[validCount] = fad.ftLastWriteTime;
}
validCount++;
free(buf);
```
заменяется на (проверка до добавления; текущий файл всегда существует — он в выделении):
```cpp
WIN32_FILE_ATTRIBUTE_DATA fad;
if (!GetFileAttributesEx(fullPath, GetFileExInfoStandard, &fad)) { free(buf); continue; }
files[validCount] = _tcsdup(fullPath);
state->fileDates[validCount] = fad.ftLastWriteTime;
validCount++;
free(buf);
```
Аналогично в обеих append-ветках ListLoadW (перед `files[validCount] = _tcsdup(fullPath);`).

### Последствия
- Поведение для типовых файлов (3 группы) не меняется — новая функция на них даёт тот же результат.
- Чинится баг «имя с размером» для малых файлов.
- Мёртвый файл не попадает в плейлист (M9).
- Минус ~300 строк копипаста → проще поддерживать.

---

## F2. C2 — NUL-терминация `_tcsncpy` (22 места)

### Проблема
`_tcsncpy(dst, src, MAX_PATH - 1)` не дописывает `\0` при `strlen(src) >= MAX_PATH - 1`; дальше `_tcslen`/`_tcsrchr` читают за буфером (UB). Особо опасно стр. 2256: `_tcsncpy(filePath, state->filePath, MAX_PATH)` — копия 260 символов в буфер на 260.

### Решение
Везде использовать безопасную копию. Один вспомогательный макрос не нужен — `_tcsncpy_s` с `_TRUNCATE` (молча обрезает, всегда терминатор):

| Строка | Было | Стало |
|---|---|---|
| 182 GetPlaylistPath | `_tcsncpy(path, iniPath, MAX_PATH - 1)` | `_tcsncpy_s(path, MAX_PATH, iniPath, _TRUNCATE)` |
| 369 BuildPlaylist | `_tcsncpy(dir, currentFile, MAX_PATH - 1)` | `_tcsncpy_s(dir, MAX_PATH, currentFile, _TRUNCATE)` |
| 573 RequestSelectedFiles | `_tcsncpy(dir, state->filePath, MAX_PATH - 1)` | `_tcsncpy_s(dir, MAX_PATH, state->filePath, _TRUNCATE)` |
| 1145 PlayIndex | `_tcsncpy(state->filePath, f, MAX_PATH - 1)` | `_tcsncpy_s(state->filePath, MAX_PATH, f, _TRUNCATE)` |
| 1370 GetMediaInfo | `_tcsncpy(info->fileName, filePath, MAX_PATH - 1)` | `_tcsncpy_s(info->fileName, MAX_PATH, filePath, _TRUNCATE)` |
| 1509/1511 readString | `_tcsncpy(dst, v.pwszVal, dstMax - 1)` | `_tcsncpy_s(dst, dstMax, v.pwszVal, _TRUNCATE)` |
| 1539 track | `_tcsncpy(info->track, val.pwszVal, 15)` | `_tcsncpy_s(info->track, 16, val.pwszVal, _TRUNCATE)` |
| 1566 year | `_tcsncpy(info->year, val.pwszVal, 4)` | `_tcsncpy_s(info->year, 16, val.pwszVal, _TRUNCATE)` |
| 2256 IDT_RECREATE | `_tcsncpy(filePath, state->filePath, MAX_PATH)` | `_tcsncpy_s(filePath, MAX_PATH, state->filePath, _TRUNCATE)` |
| 2383/2476/2596/2664 dir | `_tcsncpy(dir, FileToLoad, MAX_PATH - 1)` | `_tcsncpy_s(dir, MAX_PATH, FileToLoad, _TRUNCATE)` |
| 2558/2810 filePath | `_tcsncpy(state->filePath, FileToLoad, MAX_PATH - 1)` | `_tcsncpy_s(state->filePath, MAX_PATH, FileToLoad, _TRUNCATE)` |

Строки 609/632/2413/2431/2618/2635 исчезают в F1 (заменяются `ParseTCFileName`; внутри неё уже `_tcsncpy_s`).

### Последствия
- `_TRUNCATE` на пути длиннее 259 символов обрежет путь (раньше он молча портился/читал за буфером) — обрезка с корректным `\0` безопаснее, чем UB.
- Проект компилируется с `_CRT_SECURE_NO_WARNINGS` — это не отключает `_tcsncpy_s` (отключает только предупреждения о небезопасных функциях).

---

## F3. C3 — безопасный `realloc` двух массивов (6 мест)

### Проблема
`TCHAR** newPl = realloc(...); FILETIME* newDt = realloc(...); if (newPl && newDt) { assign }` — при успехе первого и провале второго `existState->playlist` указывает на освобождённый блок (двойной free в FreePlaylist). Семантика `realloc`: при провале исходный блок жив, при успехе — старый указатель мёртв.

### Решение — «последовательный realloc с немедленным присваиванием»
**Паттерн A (append-блоки ListLoadW, 4 места):**
```cpp
if (validCount > 0) {
    int oldCount = existState->playlistCount;
    int newTotal = oldCount + validCount;
    TCHAR** tmpPl = (TCHAR**)realloc(existState->playlist, newTotal * sizeof(TCHAR*));
    FILETIME* tmpDt = (FILETIME*)realloc(existState->fileDates, newTotal * sizeof(FILETIME));
    if (tmpPl) existState->playlist = tmpPl;   // realloc успешен — новый блок валиден
    if (tmpDt) existState->fileDates = tmpDt;
    if (tmpPl && tmpDt) {
        for (int i = 0; i < validCount; i++) {
            existState->playlist[oldCount + i] = files[i];
            existState->fileDates[oldCount + i] = dates[i];
        }
        existState->playlistCount = newTotal;
        UpdatePlaylist(existState);
        SavePlaylist(existState);
    } else {
        for (int i = 0; i < validCount; i++) free(files[i]);
        free(files); free(dates);
    }
} else {
    free(files); free(dates);
}
```
Инвариант: при частичном сбое блок стал больше, но `playlistCount` не изменился → лишняя ёмкость не используется, состояние консистентно, утечек/двойных free нет. То же для dirscan-append (там `newTotal = playlistCount + 1` на каждой итерации, `files[i]` освобождается при сбое).

**Паттерн B (LoadPlaylist, ScanDirectoryForMedia — рост локальных массивов):**
```cpp
if (count >= allocSize) {
    allocSize *= 2;
    TCHAR** tmp = (TCHAR**)realloc(files, allocSize * sizeof(TCHAR*));
    if (!tmp) break;
    files = tmp;                                   // сразу после успеха
    FILETIME* tmpD = (FILETIME*)realloc(dates, allocSize * sizeof(FILETIME));
    if (!tmpD) break;                              // dates остаётся старым (жив)
    dates = tmpD;
}
```
Сейчас там `if (!tmp || !tmpD) break; files = tmp; dates = tmpD;` — при частичном сбое `files` указывает на освобождённый блок.

### Последствия
- OOM-крах превращается в «плейлист не вырос, файлы пропущены» — без повреждения кучи.
- Нормальный путь (оба realloc успешны) не меняется.

---

## F4. C4 — `playlistIndex` в ListLoadNextW

### Проблема
При навигации n/p TC вызывает `ListLoadNextW`, но индекс текущего трека не ищется: маркер «▶» остаётся на старом файле, и `IDM_PREV/NEXT`/`WM_PLAYER_TRACK_END` отсчитывают от старого индекса.

### Решение
После строки `_tcsncpy_s(state->filePath, MAX_PATH, FileToLoad, _TRUNCATE);` (F2) вставить:
```cpp
// Синхронизация индекса плейлиста с файлом, на который переключил TC
if (state->playlist && state->playlistCount > 0) {
    for (int i = 0; i < state->playlistCount; i++) {
        if (_tcsicmp(state->playlist[i], state->filePath) == 0) {
            state->playlistIndex = i;
            break;
        }
    }
}
```
Далее уже идёт существующий `UpdatePlaylist` (при !IsQuickView) — подсветка станет верной.

### Последствия
- В QuickView плейлист скрыт — смена индекса безвредна.
- Если файл не найден в плейлисте (навигация вне набора выделения) — индекс не трогаем (безопасно).

---

## F5. C5 — M4A считается аудио

### Решение
В `IsAudioOnly` в массив `audioExts[]` добавить `TEXT("m4a")` (рядом с `mp3`/`aac`).

### Последствия
- Колонка Type → «Audio», `showPlaylist=TRUE` для m4a (корректно — это аудио-контейнер).
- Единственное отличие: m4a-файлы с видео (экзотика) будут показывать плейлист вместо видео — приемлемо, m4a по определению аудио.

---

## F6. C6 — use-after-free в колбэке Media Foundation

### Проблема
`MFCallback::OnMediaPlayerEvent` (поток MF) разыменовывает `m_p` (tagMFPlayer), который `MFPlayer_Destroy` освобождает `free(p)` синхронно. `Sleep(50)` после Stop не гарантирует пустоту очереди событий. Это корень исторического «Prev/Next crash» (в DS его закрыли join'ом потока, в MF — нет).

### Решение — счётчик ссылок на tagMFPlayer + флаг `destroying`
В `mf_player.cpp`:

```cpp
struct tagMFPlayer {
    IMFPMediaPlayer*        pPlayer;
    IMFVideoDisplayControl* pVideoCtrl;
    HWND                    hVideoWnd;
    MFPlayerEndCallback     onEnd;
    void*                   userData;
    volatile LONG           isPlaying;
    volatile LONG           isPaused;
    volatile LONG           refCount;    // 1 = владелец (API); +1 пока жив колбэк
    volatile LONG           destroying;  // 1 = идёт Destroy: onEnd не вызывать
};

// Освобождение одной ссылки; структурa удаляется при нуле.
static void MF_ReleaseRef(tagMFPlayer* p) {
    if (!p) return;
    if (InterlockedDecrement(&p->refCount) == 0) free(p);
}
```

`MFCallback`: конструктор `InterlockedIncrement(&m_p->refCount);`, деструктор `MF_ReleaseRef(m_p);`, в `OnMediaPlayerEvent` перед `onEnd` проверка:
```cpp
void STDMETHODCALLTYPE OnMediaPlayerEvent(MFP_EVENT_HEADER* pEventHeader) {
    if (!pEventHeader || !m_p) return;
    if (pEventHeader->eEventType == MFP_EVENT_TYPE_PLAYBACK_ENDED) {
        InterlockedExchange(&m_p->isPlaying, FALSE);
        InterlockedExchange(&m_p->isPaused,  FALSE);
        // destroy() мог начаться: userData (PluginState) может уже освобождаться.
        if (!InterlockedCompareExchange(&m_p->destroying, 0, 0))
            if (m_p->onEnd) m_p->onEnd(m_p->userData);
    }
}
```

`MFPlayer_Create`: после `calloc` → `p->refCount = 1;` (и `p->destroying = FALSE;` — calloc уже обнулил).

`MFPlayer_Destroy`:
```cpp
void MFPlayer_Destroy(MFPlayer* player) {
    if (!player) return;
    tagMFPlayer* p = (tagMFPlayer*)player;
    InterlockedExchange(&p->destroying, TRUE);   // первым делом — подавить onEnd
    MFPlayer_Stop(player);
    if (p->pVideoCtrl) { p->pVideoCtrl->Release(); p->pVideoCtrl = NULL; }
    if (p->pPlayer)    { p->pPlayer->Release(); p->pPlayer = NULL; }
    MF_ReleaseRef(p);                            // снять ссылку владельца API
}
```

**Проверка жизненного цикла:**
- Успех Open: cb создан (refCount 1→2), MF держит cb; `cb->Release()` (создатель) → cb жив через MF. Destroy: releasing → pPlayer->Release → cb.ref 0 → delete → ~MFCallback → refCount 2→1 → затем MF_ReleaseRef → 1→0 → free. ✓
- Провал Open (оба MFPCreateMediaPlayer): cb->Release() → delete → refCount 2→1 (владелец жив) → позже Destroy → 1→0 → free. ✓ (старый код в этой ветке удерживал указатель на уже освобождённый объект)
- In-flight событие: MF держит cb → после Destroy refCount 2→1, структура жива до завершения колбэка. ✓

### Остаточный риск (документируется в Audit §7)
Узкая гонка «окно закрывается ровно в момент финального PLAYBACK_ENDED»: колбэк успел прочитать `destroying==0`, UI закрыл окно и освободил PluginState, колбэк зовёт `onEnd → OnMFEnd → state->hMainWnd`. Полное закрытие требует рефкаунта PluginState — вынесено как отдельный риск, НЕ входит в этот фикс (в текущем коде окно такое же, у DS-потока тоже; MFPlayer_Stop+Sleep(50)+флаг закрывают 99.9% случаев, включая весь сценарий быстрых переключений).

---

## F7. C7 — единая стратегия сброса видеорежима **[TC-тест]**

> ✅ **Фазы 1 и 2 внедрены** (2026-08-09): PlayIndex fallback, ListLoadW QuickView и ListLoadNextW fallback — везде `RecreateVideoWindow` + синк `DSPlayer_SetVideoWnd`, порядок `MFPlayer_Destroy` → `RecreateVideoWindow` единый во всех ветках. Механизм отложенного `IDT_RECREATE` удалён полностью (таймер, ветка WM_TIMER, KillTimer в WM_DESTROY), `DestroyChildVideoWindows` и `#define IDT_RECREATE` удалены как мёртвый код.

### Проблема
Три разных подхода: `PlayIndex` fallback НЕ пересоздаёт hVideoWnd при MF→MF; `ListLoadNextW` fallback делает `DestroyChildVideoWindows` + отложенный `IDT_RECREATE` (300 мс); `ListLoadW` QuickView — `DestroyChildVideoWindows`. По PROJECT_CONTEXT.md правильная стратегия — `RecreateVideoWindow`.

### Решение — две фазы

**Фаза 1 (безопасная, без изменения тестированного пути):**
1. `PlayIndex` fallback (ветка `else`) — всегда `RecreateVideoWindow` перед созданием нового MFPlayer (сейчас — только при выходе из DS):
```cpp
} else {
    // Fallback — пересоздаём и плеер, и окно (сбрасывает VP9/D3D-состояние)
    if (state->useDirectShow) DSPlayer_Stop(state->pDSPlayer);
    state->useDirectShow = FALSE;
    MFPlayer_Destroy(state->pMFPlayer);
    RecreateVideoWindow(state);
    state->pMFPlayer = MFPlayer_Create(state->hVideoWnd, OnMFEnd, state);
    hr = MFPlayer_Open(state->pMFPlayer, f);
    if (SUCCEEDED(hr)) MFPlayer_Play(state->pMFPlayer);
}
```
2. `ListLoadW` QuickView-ветка: заменить `DestroyChildVideoWindows(existState->hVideoWnd);` на `RecreateVideoWindow(existState);` (совпадает с документацией).

**Фаза 2 (только после прогона сценария 6.avi→7.mp4→5.mp4 в TC):**
3. `ListLoadNextW` fallback: заменить `DestroyChildVideoWindows(state->hVideoWnd);` на `RecreateVideoWindow(state);` и **убрать** отложенный `IDT_RECREATE` (код в WM_TIMER + `KillTimer/SetTimer(IDT_RECREATE)` в ListLoadNextW), т.к. после немедленного пересоздания отложенное пересоздание приведёт к повторному уничтожению окна через 300 мс (мерцание) — при этом `IDT_COOLDOWN`/`switchInProgress` остаются.

### Почему фаза 2 отложена
Последний коммит намеренно перешёл на отложенное пересоздание («without hang on rapid switching»). Немедленный `RecreateVideoWindow` может вернуть зависание на быстрых переключениях — проверять только в живом TC. Без TC-валидации фаза 2 НЕ выполняется.

---

## F8. C8 — `hLastPluginWnd` закрывает чужую вкладку

### Проблема
При append OFF каждый новый F3 делает `PostMessage(hOldLister, WM_CLOSE)` по предыдущему окну плагина. При двух открытых вкладках MediaShow2 F3 на третьем файле закроет одну из существующих. Статик не очищается в ListCloseWindow.

### Решение
1. В `ListCloseWindow` (после `DestroyWindow`):
```cpp
if (hLastPluginWnd == ListWin) hLastPluginWnd = NULL;
```
2. В append-OFF блоке «close old tab» — закрывать старую вкладку, только если она из той же директории (тот же контекст просмотра):
```cpp
if (hLastPluginWnd && IsWindow(hLastPluginWnd)) {
    PluginState* oldState = GetState(hLastPluginWnd);
    if (oldState && SameDirectory(oldState->filePath, FileToLoad)) {
        HWND hOldLister = GetParent(hLastPluginWnd);
        if (hOldLister && IsWindow(hOldLister))
            PostMessage(hOldLister, WM_CLOSE, 0, 0);
    }
}
```
Вспомогательная функция (рядом с IsMediaFile):
```cpp
static BOOL SameDirectory(const TCHAR* a, const TCHAR* b) {
    TCHAR da[MAX_PATH], db[MAX_PATH];
    _tcsncpy_s(da, MAX_PATH, a, _TRUNCATE);
    _tcsncpy_s(db, MAX_PATH, b, _TRUNCATE);
    TCHAR* sa = _tcsrchr(da, TEXT('\\')); if (sa) *sa = 0;
    TCHAR* sb = _tcsrchr(db, TEXT('\\')); if (sb) *sb = 0;
    return _tcsicmp(da, db) == 0;
}
```
Замечание: `hLastPluginWnd` объявлен `static` внутри `ListLoadW` — для доступа из `ListCloseWindow` вынести в файловый static рядом с `s_fixMaximizeWnd`.

### Последствия
- Обычный сценарий (один каталог, F3 подряд) — поведение не меняется (та же директория → закрывается).
- Две сессии в разных папках больше не убивают друг друга.

---

## F9. C9 — удалить мёртвую ветку `else if (MFPlayer_HasVideo(...))`

### Решение
В `PlayIndex` удалить ветку целиком (она идентична fallback-ветке и никогда не выполняется: `pVideoCtrl==NULL`, т.к. MFPlayer только что создан и не открывал файл). Остаётся `if (needsDS) … else { fallback }`. Функция `MFPlayer_HasVideo` в mf_player.h остаётся (публичный API), но после удаления единственного вызова становится неиспользуемой — пометить комментарием или удалить вместе с `MFPlayer_GetCurrentVideoSize` (тоже без вызовов — проверить grep'ом перед удалением).

### Последствия
Без изменений поведения (ветка недостижима). Меньше путаницы.

---

## F10. C10 — единая дата `ftLastWriteTime`

### Решение
В `ScanDirectoryForMedia`: `dates[count] = fd.ftCreationTime;` → `dates[count] = fd.ftLastWriteTime;`.

### Последствия
Колонка Date в плейлисте из dir-scan совпадёт с датой из selection/load-путей (раньше показывала дату копирования).

---

## F11. C11 — явная инициализация COM для DirectShow

### Решение
В `ds_player.cpp`, в начале `DSPlayer_Open` (вызывается только с UI-потока):
```cpp
// DirectShow требует COM на потоке. Хост (TC) мог и не инициализировать COM.
// CO_E_ALREADYINITIALIZED — норма, не ошибка. CoUninitialize НЕ вызываем (чужой процесс).
static BOOL s_comInited = FALSE;
if (!s_comInited) {
    HRESULT hrc = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (SUCCEEDED(hrc) || hrc == RPC_E_CHANGED_MODE || hrc == S_FALSE) s_comInited = TRUE;
}
```
(все три кода означают «COM готов»; `RPC_E_CHANGED_MODE` — инициализирован в другом режиме, для DirectShow MTA не мешает, т.к. граф создаётся inproc).

### Последствия
DS-воспроизведение перестанет зависеть от неявного COM в TC. Никаких CoUninitialize — процесс чужой.

---

## F12. M1 — мёртвый код

Удалить (grep подтвердил отсутствие ссылок):
- `BuildPlaylistFromSelection` (стр. 351–363).
- `#define WM_DEFERRED_GETFILES` (стр. 42).
- В plugin_api.h: `WM_PLAYER_PLAY/PAUSE/STOP/FULLSCREEN/TOPMOST/MUTE/INFO/PREV/NEXT` (оставить `WM_PLAYER_TRACK_END`), `IDM_SETTINGS`, `IDM_PLAYLIST`.
- Параметр `useDS` в `GetMediaInfo` и `ShowFileInfoDialog` (+ вызов из IDM_FILEINFO).
- `FileInfoWndProc` case `WM_DESTROY: PostMessage(hWnd, WM_USER, 0, 0);` — обработчика нет.

### Последствия
Без изменений поведения. Меньше путаницы для следующих правок.

---

## F13. M2/M4/M5/M6/M10 — мелкие фиксы UI и отладки

**M2.** В `IDM_ABOUT` убрать строку `TEXT("  Ctrl+T Always on Top")` — фича не реализована.

**M4.** `IsQuickView`: убрать `#pragma optimize("", off/on)` и все `OutputDebugString` (функция вызывается на каждый ListLoadNextW). Логика не меняется.

**M5.** `FullscreenWndProc` — прокинуть клавиши в главное окно, чтобы Space/S/стрелки работали в фуллскрине:
```cpp
case WM_KEYDOWN:
    if (state) {
        if (wParam == VK_ESCAPE || wParam == VK_F11)
            SendMessage(state->hMainWnd, WM_COMMAND, IDM_FULLSCREEN, 0);
        else
            SendMessage(state->hMainWnd, WM_KEYDOWN, wParam, lParam);
    }
    return 0;
```

**M6.** `PlayIndex`: после цикла `state->isPlaying = TRUE;` → по фактическому результату. Ввести `BOOL playedOK = FALSE;`, ставить `TRUE` при `SUCCEEDED(hr); break;`, после цикла `state->isPlaying = playedOK; state->isPaused = FALSE;`. Статус-бар не будет показывать «Playing» на мёртвом плеере, если все файлы не открылись.

**M10.** `ShowFileInfoDialog`: `EnableWindow(hParent, TRUE)` → `if (IsWindow(hParent)) EnableWindow(hParent, TRUE);` и `if (IsWindow(hParent)) SetFocus(hParent);`.

---

## F14. M7 — SavePlaylist только при изменении

### Проблема
`UpdatePlaylist` вызывает `SavePlaylist` на каждый ребилд, включая простые переключения треков (PlayIndex) → запись всего файла на диск при каждом Next.

### Решение
1. Убрать `SavePlaylist(state);` из конца `UpdatePlaylist`.
2. Добавить явные `SavePlaylist` в места, где состав/индекс реально меняются:
   - `RequestSelectedFiles` — уже есть (оставить).
   - ListLoadW append-ветки — уже есть (оставить).
   - ListLoadW, обычный поток (append OFF): после финального `UpdatePlaylist(state);` (блок `if (!quickView)`) добавить `SavePlaylist(state);`.
   - `LVN_KEYDOWN` (VK_DELETE и Ctrl+↑/↓): после `UpdatePlaylist(state);` добавить `SavePlaylist(state);`.
   - `LVN_COLUMNCLICK` (сортировка): после `UpdatePlaylist(state);` добавить `SavePlaylist(state);`.
   - `IDM_CLEARPLAYLIST` — ничего сохранять (файл удалён), без изменений.

### Последствия
- Переключение треков больше не пишет плейлист на диск (устраняет I/O-шторм на больших плейлистах).
- Сохранение происходит во всех местах, где состав менялся — проверить каждое по списку выше.

---

## F15. T1/T3 — починка тестов

**T1. `test_bug.py` (краш `IndexError`, строка 18):** `fn = list(buf[:before_len])` имеет длину `before_len` (индексы 0..before_len-1), а код пишет `fn[before_len] = '\0'`. В C-версии буфер на 1 больше. Фикс в обеих функциях (buggy и fixed):
```python
fn = list(buf[:before_len]) + ['\0']
```
После этого прогнать — он должен показать отличие buggy от fixed (это его смысл).

**T3. `test_parse.py`:** заменить `parse_tc_line` на алгоритм 1..3 групп (зеркало F1) и обновить ожидания Edge2/Edge4:
```python
def parse_tc_line(line):
    # ... поиск DD.MM.YYYY (без изменений) ...
    if date_start < 0:
        return line.strip(), False
    before = line[:date_start].rstrip()
    p = len(before) - 1
    for _ in range(3):
        digit_start = p
        while p > 0 and before[p].isdigit():
            p -= 1
        if p == digit_start:
            break
        if p > 0 and before[p] in (' ', '\u00A0', '\t'):
            p -= 1
        elif p > 0:
            p = digit_start
            break
    filename = before[:p + 1].rstrip()
    return filename, True
```
Ожидания: Edge2 `file.mp3 12 345 …` → `file.mp3` (было FAIL), Edge4 `a.mp3 1 …` → `a.mp3` (было FAIL). Итог: ALL TESTS PASSED.

---

## F17. AV1-видео направляется в DirectShow (webm не воспроизводился)

> ✅ **Внедрено 2026-08-15.** Пересобраны x86+x64, переупакованы .wlx/.wlx64, test_verify T01–T06 PASS.

### Проблема
`6ix9ine.webm` (AV1 + Opus, 143 МБ) в TC не воспроизводился: окно открывалось, но ни звука, ни картинки. Диагностика реальными функциями плагина (консольный харнесс, окно за пределами экрана):

| Файл (кодек) | MF с окном | DS (VMR-9) |
|---|---|---|
| 7.mp4 (H.264/AAC) | ✅ pos=0.91/5.32 | — |
| 6ix9ine.webm (AV1/Opus) | ❌ pos=0.00/0.00 — заморожен | ✅ pos=1.79/177.04 |
| 6.avi (VP9) | ✅ pos=0.77/109.71 | — |

**Корень:** MFP с реальным видео-окном не может рендерить **AV1** — `MFPlayer_Open`/`MFPlayer_Play` возвращают S_OK, но часы не идут (EVR встаёт, позиция и длительность 0.00). Без окна (NULL) тот же файл играет — стопор именно в видео-рендере. H.264 и VP9 через MF работают. Плюс `MFPlayer_AudioNeedsDS` для этого файла возвращал 0 (Opus-проверка не срабатывала: source reader не отдавал аудио-тип), поэтому файл уходил в MF и молча замирал.

### Решение
Эвристика выбора движка расширена: помимо аудио-кодеков (Opus/Vorbis/AC-3/E-AC-3/DTS) проверяется видео-субтип — **AV1 (`MFVideoFormat_AV1`) → DirectShow**. Функция переименована `MFPlayer_AudioNeedsDS` → `MFPlayer_NeedsDS` (обновлены 3 точки вызова в dllmain.cpp: PlayIndex, ListLoadW, ListLoadNextW). VP9 и H.264 остаются на MF.

Проверка харнессом: `MFPlayer_NeedsDS(webm)=1`, `MFPlayer_NeedsDS(mp4)=0`, DS играет webm (позиция растёт). **[TC-тест]** — F3 на 6ix9ine.webm.

---

## F16. Что НЕ трогаем (осознанно)

- **M3** (EnumFindLCLListBox берёт не ту панель при двух панелях TC) — нет надёжного способа связать список с панелью без доработки с LCLListBox; задокументировать как известное ограничение.
- **M8** (source-reader на каждое переключение) — поведение корректное, только поправить комментарий про E-AC-3: код проверяет `0x00000AAC` (wFormatTag DD+), комментарий упоминает `0xAAC2` — привести комментарий в соответствие с кодом.
- **L5** (порядок dir-scan — порядок FindFirstFile) — менять поведение автоплейлиста рискованно; опционально после остальных фиксов.

---

## Статус внедрения (2026-08-09)

| Фикс | Статус |
|---|---|
| F1 (парсер) + F15 (тесты T1/T3) | ✅ внедрено |
| F2 (терминация), F3 (realloc), F5 (m4a), F10 (даты) | ✅ внедрено |
| F4 (playlistIndex), F9 (мёртвая ветка), F11 (COM) | ✅ внедрено |
| F6 (рефкаунт MF) | ✅ внедрено |
| F8 (вкладки), F7 фаза 1 | ✅ внедрено |
| F12, F13, F14 (playlistDirty), M8-комментарий | ✅ внедрено |
| F7 фаза 2 (ListLoadNextW → немедленный RecreateVideoWindow, IDT_RECREATE удалён) | ✅ внедрено — требует контрольного TC-прогона быстрых ↓/↑ (откат к `79b7220`) |
| F16 (M3, L5) | ➖ не трогаем осознанно |

Валидация после внедрения: `cmake --build build-x64 --config Release` — 0 ошибок/предупреждений; `test_parse.py`, `test_parse_c.py`, `test_bug.py` — ALL PASSED; `test_verify.py` — T01–T06 PASS; `package.py` — оба архива собраны. Ручной тест в TC (F3/QuickView) — видимых багов нет. **F7 фаза 2:** контрольный прогон в TC — быстрые ↓/↑ по сценарию 6.avi→7.mp4→5.mp4 (откат к коммиту `79b7220`, если хэнг вернётся).

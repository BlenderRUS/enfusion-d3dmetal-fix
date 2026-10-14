# Arma Reforger на Mac через CrossOver (D3DMetal): исправление графики

`dxgi.dll` для Arma Reforger под CrossOver на Mac с Apple Silicon. Исправляет две проблемы D3DMetal:

1. **Игра не запускается:** `Adapter 0 (AMD Compatibility Mode) failed to provide some output`,
   `Couldn't build device list for any adapter`.
2. **Гигантские вытянутые полигоны** вокруг персонажей и «кислотные» шумные текстуры.
   Причина: D3DMetal неверно вычисляет смещения атрибутов вершин, заданные как
   `D3D12_APPEND_ALIGNED_ELEMENT`, и читает веса и индексы костей и UV не из тех байтов.
   Библиотека сама вычисляет смещения по правилам D3D12 и передаёт D3DMetal явные значения.
   Шейдеры и файлы игры не меняются.

*English version below.*

## Что нужно

- Mac на Apple Silicon (M1 и новее). D3DMetal работает только на нём.
- CrossOver с графическим бэкендом D3DMetal. Проверено на CrossOver 26.2 (D3DMetal 3.0).
- Бутылка (bottle) с Windows-версией Steam, в которой установлена Arma Reforger.

## Установка

1. Скачайте архив `enfusion-d3dmetal-fix-1.0.zip` со страницы
   [Releases](https://github.com/BlenderRUS/enfusion-d3dmetal-fix/releases) и скопируйте `dxgi.dll` в папку игры:
   ```
   ~/Library/Application Support/CrossOver/Bottles/<БУТЫЛКА>/drive_c/Program Files (x86)/Steam/steamapps/common/Arma Reforger/
   ```
2. Включите в бутылке графику **D3DMetal**: CrossOver → бутылка → Settings → Graphics → D3DMetal.
3. Разрешите Wine загружать библиотеку из папки игры. Самый простой способ — запускать игру
   скриптом `run-reforger.sh` из этого архива, он передаёт `--dll dxgi=n,b` сам.
   Другой способ — CrossOver → бутылка → Wine Configuration → Libraries → добавить `dxgi`,
   режим **Native, then Builtin**.

## Запуск

```sh
BOTTLE="Steam" ./run-reforger.sh -window      # оконный режим; без -window на весь экран
```

`BOTTLE` — имя вашей бутылки в CrossOver. Скрипт при необходимости запустит Steam внутри бутылки
и затем саму игру.

**Кнопка Play в Steam не подходит.** Steam запускает игру через лаунчер BattlEye, а он под Wine
не работает. Скрипт запускает `ArmaReforgerSteam.exe` напрямую.

## Ограничения

- Без BattlEye: на серверах с BattlEye будет кик (`GAME_RESTART_REQUIRED`). Работают одиночные
  режимы и серверы без BattlEye.
- Проверено на одной системе: MacBook Pro M5, macOS 27.0.1, CrossOver 26.2 (D3DMetal 3.0).
  Версия macOS здесь почти ни при чём: обе ошибки сидят в D3DMetal. Если CodeWeavers или Apple
  исправят их в новых версиях, библиотека просто станет лишней, ломать она ничего не будет.

## Сборка из исходников

```sh
brew install mingw-w64
./build.sh          # результат: dist/dxgi.dll
```

## Лицензия

[Apache License 2.0](LICENSE). Основано на [enfusion-dxgi-fix](https://github.com/vec715/enfusion-dxgi-fix)
от vec715.

Распространять, в том числе изменённые версии, можно свободно, но вместе с файлами `LICENSE` и
`NOTICE`. В `NOTICE` записаны автор и ссылка на исходный репозиторий:
https://github.com/BlenderRUS/enfusion-d3dmetal-fix

---

# Arma Reforger on Mac with CrossOver (D3DMetal): graphics fix

A `dxgi.dll` for Arma Reforger under CrossOver on Apple Silicon Macs. It fixes two D3DMetal problems:

1. **The game does not start** (`Adapter 0 (AMD Compatibility Mode) failed to provide some output`).
2. **Giant stretched polygons** around characters and noisy textures. D3DMetal resolves
   `D3D12_APPEND_ALIGNED_ELEMENT` vertex input offsets wrongly, so bone weights/indices and UVs are
   fetched from the wrong bytes. The DLL resolves the offsets per D3D12 rules and passes explicit
   values. Game shaders and files are not modified.

**Requirements:** Apple Silicon Mac, CrossOver with the D3DMetal backend (tested: CrossOver 26.2,
D3DMetal 3.0), a bottle with Windows Steam and Arma Reforger.

**Install:** download `enfusion-d3dmetal-fix-1.0.zip` from
[Releases](https://github.com/BlenderRUS/enfusion-d3dmetal-fix/releases), copy `dxgi.dll` into `.../steamapps/common/Arma Reforger/`, set the bottle's graphics
to D3DMetal, and either start the game with `run-reforger.sh` (it passes `--dll dxgi=n,b`) or add
`dxgi` as *Native, then Builtin* in the bottle's Wine Configuration → Libraries.

**Run:** `BOTTLE="Steam" ./run-reforger.sh -window`. Do not use Steam's Play button: it goes through
the BattlEye launcher, which does not work under Wine. Without BattlEye, BattlEye-protected servers
will kick you; single player and non-BattlEye servers work.

**Build:** `brew install mingw-w64 && ./build.sh` produces `dist/dxgi.dll`.

**License:** [Apache License 2.0](LICENSE), based on
[enfusion-dxgi-fix](https://github.com/vec715/enfusion-dxgi-fix) by vec715. You may redistribute it,
modified or not, together with the `LICENSE` and `NOTICE` files. `NOTICE` names the author and links
to the original repository: https://github.com/BlenderRUS/enfusion-d3dmetal-fix
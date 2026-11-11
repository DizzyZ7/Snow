# Snow

Нативный интерактивный снегопад для Windows 10/11.

Проект находится на раннем этапе разработки. Первый этап — прозрачный click-through overlay и GPU-рендеринг снежинок.

## Сборка

Требуются Windows 10/11, Visual Studio 2022 с C++ Desktop Development, Windows SDK и CMake 3.24+.

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
```

Исполняемый файл: `build/Release/Snow.exe`.

## Управление

После запуска Snow работает в системном трее. Через меню трея можно приостановить снегопад или завершить программу.

## Статус

Экспериментальная версия; тестирование графического поведения на Windows обязательно.

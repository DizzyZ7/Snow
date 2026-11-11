# Snow — архитектура

## Текущее состояние
src/main.cpp содержит начальную Win32/GDI реализацию. Это временная вертикальная функциональная заготовка, не production renderer.

## Планируемые подсистемы
- app: lifecycle, tray, settings, power and session events
- platform/windows: WinEvent hooks, DWM window geometry, monitor topology, taskbar tracking
- simulation: deterministic particle updates, wind and collision
- accumulation: height-field for each supported visible surface
- renderer: Direct3D 11/DirectComposition with premultiplied alpha
- interaction: explicit tool mode, cursor and surface interactions
- persistence: versioned JSON config and optional scene state

## Инварианты
- UI и рендеринг не должны блокировать друг друга.
- Все координаты поверхностей приводятся к физическим пикселям и виртуальному desktop space.
- Удаление HWND немедленно инвалидирует связанную поверхность; повторное использование HWND не должно возвращать чужой сугроб.
- Overlay не должен захватывать ввод в обычном режиме.
- Ошибки D3D device removed приводят к восстановлению или безопасной остановке.
- Система работает без административных прав и без инъекций в процессы.
- Данные содержимого чужих окон не считываются.

## Технические риски
- DWM frame bounds не равны полной видимой геометрии в произвольных случаях.
- Точный расчет окклюзии произвольных окон дорогостоящий и может быть приближенным.
- Layered window с GDI не удовлетворяет долгосрочным целям по качеству альфа-композиции.
- CI runner не заменяет ручное тестирование на интерактивном Windows desktop.

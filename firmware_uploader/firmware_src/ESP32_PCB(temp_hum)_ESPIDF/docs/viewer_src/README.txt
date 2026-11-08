Генератор на docs/BLOCK_DIAGRAMS.html (блокови схеми + код ред по ред).

viewer/ann/*.ann      обяснения по редове (един файл за всеки файл от кода; формат "ред: текст")
viewer/principles.txt принципи и речник ([[ключ]] в обясненията)
viewer/external.txt   външни функции/типове (ESP-IDF, FreeRTOS, RadioLib, libc)
viewer/patches.txt    допълнителни поправки на отделни редове
viewer/links.py       връзки блок от схема -> функции в кода
diag/*.py             рисуване на схемите (SVG)

Билд (Python 3.9+): python viewer/build_all.py ..\BLOCK_DIAGRAMS.html
Пътят до кода (SRC) е в viewer/vb.py. Ако промениш кода, номерата на редовете в .ann трябва да се обновят.

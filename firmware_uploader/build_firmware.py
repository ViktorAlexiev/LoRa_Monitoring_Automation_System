"""
build_firmware.py
------------------
Компилира всички .ino скечове в firmware_src/ чрез arduino-cli
и слага готовите .hex (AVR) / .bin (ESP32) файлове в firmware/.

Изисква инсталиран arduino-cli (пътят се взима от config.ini, секция [build]).
Изисква инсталирани cores веднъж (само първия път):
    arduino-cli core install arduino:avr
    arduino-cli core install esp32:esp32

Кои папки да компилира и към кой тип борд се задават в config.ini,
секция [build_targets] (име_на_папка = avr/esp32).

Пускане:  python build_firmware.py
"""

import os
import shutil
import subprocess
import sys
import tempfile
import configparser

# Работната директория трябва да е папката на самия .exe/.py, не откъдето е стартиран -
# иначе config.ini/firmware_src/firmware не се намират при двоен клик от друго място.
if getattr(sys, "frozen", False):
    _BASE_DIR = os.path.dirname(sys.executable)
else:
    _BASE_DIR = os.path.dirname(os.path.abspath(__file__))
os.chdir(_BASE_DIR)


def load_config(path="config.ini"):
    parser = configparser.ConfigParser()
    parser.read(path, encoding="utf-8")
    return parser


def compile_sketch(arduino_cli_path, fqbn, sketch_dir, out_dir):
    os.makedirs(out_dir, exist_ok=True)
    cmd = [arduino_cli_path, "compile", "--fqbn", fqbn, "--export-binaries",
           "--output-dir", out_dir, sketch_dir]
    print(f"  -> {' '.join(cmd)}")
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        print(result.stdout)
        print(result.stderr)
        return False
    return True


def find_output_file(out_dir, sketch_name, extension, board_type):
    if board_type == "esp32_merged":
        # bootloader + partitions + app в едно, flash-ва се на 0x0
        candidate = os.path.join(out_dir, f"{sketch_name}.ino.merged.bin")
        if os.path.exists(candidate):
            return candidate
        for f in os.listdir(out_dir):
            if f.endswith(".merged.bin"):
                return os.path.join(out_dir, f)
        return None

    if board_type == "esp32_app":
        # САМО app партицията (без merged), flash-ва се на 0x10000 - не пипа NVS
        candidate = os.path.join(out_dir, f"{sketch_name}.ino.bin")
        if os.path.exists(candidate) and "merged" not in candidate:
            return candidate
        for f in os.listdir(out_dir):
            if f.endswith(".bin") and "merged" not in f and "bootloader" not in f and "partitions" not in f:
                return os.path.join(out_dir, f)
        return None

    # arduino-cli записва <sketch_name>.ino.<ext> в output-dir (AVR случай)
    candidate = os.path.join(out_dir, f"{sketch_name}.ino.{extension}")
    if os.path.exists(candidate):
        return candidate
    for f in os.listdir(out_dir):
        if f.endswith(f".{extension}"):
            return os.path.join(out_dir, f)
    return None


def main():
    cfg = load_config()

    if "build" not in cfg or "build_targets" not in cfg:
        print("Липсват секции [build] / [build_targets] в config.ini")
        sys.exit(1)

    arduino_cli_path = cfg["build"]["arduino_cli_path"]
    avr_fqbn = cfg["build"]["avr_fqbn"]
    esp32_fqbn = cfg["build"]["esp32_fqbn"]
    src_root = cfg["build"]["firmware_src_dir"]

    # изходната папка "firmware/" - взимаме я от някой съществуващ path в [paths]
    out_root = os.path.dirname(cfg["paths"]["firmware_config_avr"])
    os.makedirs(out_root, exist_ok=True)

    if not os.path.isdir(src_root):
        print(f"Няма папка {src_root}")
        sys.exit(1)

    targets = cfg["build_targets"]
    if not targets:
        print("Няма зададени build_targets в config.ini")
        sys.exit(1)

    ok_count = 0
    fail_count = 0

    for name, board_type in targets.items():
        board_type = board_type.strip().lower()
        sketch_dir = os.path.join(src_root, name)
        ino_file = os.path.join(sketch_dir, f"{name}.ino")

        if not os.path.isfile(ino_file):
            print(f"[ПРОПУСНАТ] {name}: няма {ino_file}")
            continue

        if board_type == "avr":
            fqbn, extension = avr_fqbn, "hex"
        elif board_type in ("esp32_merged", "esp32_app"):
            fqbn, extension = esp32_fqbn, "bin"
        else:
            print(f"[ПРОПУСНАТ] {name}: непознат board_type '{board_type}'")
            continue

        print(f"[КОМПИЛИРАМ] {name} ({board_type}, fqbn={fqbn})")

        with tempfile.TemporaryDirectory() as tmp_out:
            if not compile_sketch(arduino_cli_path, fqbn, sketch_dir, tmp_out):
                print(f"[ГРЕШКА] {name} не се компилира")
                fail_count += 1
                continue

            found = find_output_file(tmp_out, name, extension, board_type)
            if not found:
                print(f"[ГРЕШКА] Не намерих изходен .{extension} файл за {name}")
                fail_count += 1
                continue

            dest = os.path.join(out_root, f"{name}.{extension}")
            shutil.copy2(found, dest)
            print(f"[OK] {name} -> {dest}")
            ok_count += 1

    print(f"\nГотово. Успешни: {ok_count}, Неуспешни: {fail_count}")


if __name__ == "__main__":
    main()
    if getattr(sys, "frozen", False):
        # .exe от двоен клик затваря терминала веднага след края - изчакай, за да се прочете изхода
        input("\nНатисни Enter за изход...")
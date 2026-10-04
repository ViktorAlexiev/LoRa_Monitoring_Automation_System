"""
build_firmware.py
------------------
Компилира всички .ino скечове в firmware_src/ чрез arduino-cli
и слага готовите .hex (AVR) / .bin (ESP32) файлове в firmware/.

Изисква инсталиран arduino-cli (пътят се взима от config.ini, секция [build]).
Изисква инсталирани cores веднъж (само първия път):
    arduino-cli core install arduino:avr
    arduino-cli core install esp32:esp32

Кои папки да компилира и към кой тип борд се задават в config.ini:
  [build_targets]             - ATmega LoRa radio node + TTGO gateway (firmware_src_dir)
  [build_targets_esp32_pcb]   - ESP32 PCB (firmware_src_dir_esp32_pcb, FQBN: esp32_pcb_fqbn)
(име_на_папка = avr / esp32_merged / esp32_app). Резултатите на ESP32 PCB отиват в firmware/esp32_pcb/.

Изисква Arduino библиотеките Crypto, AES_CMAC, PubSubClient, ArduinoJson, LoRa (с public
readRegister/writeRegister - виж cad.h) и Adafruit_SHT31.

Пускане:  python build_firmware.py            (двата набора)
          python build_firmware.py avr         (само ATmega + TTGO)
          python build_firmware.py esp32_pcb   (само ESP32 PCB)
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


def build_set(label, arduino_cli_path, src_root, targets, avr_fqbn, esp32_fqbn, out_root):
    """Компилира един набор скечове (папка src_root) и слага резултатите в out_root.
    Връща (успешни, неуспешни)."""
    os.makedirs(out_root, exist_ok=True)
    if not os.path.isdir(src_root):
        print(f"[{label}] Няма папка {src_root}")
        return 0, 1

    ok_count = 0
    fail_count = 0
    print(f"=== {label}: {src_root} -> {out_root} ===")

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

    return ok_count, fail_count


def main():
    cfg = load_config()

    if "build" not in cfg:
        print("Липсва секция [build] в config.ini")
        sys.exit(1)

    # Кой набор да се компилира: avr (ATmega + TTGO), esp32_pcb (ESP32 платката) или all (по подразбиране)
    which = sys.argv[1].lower() if len(sys.argv) > 1 else "all"
    if which not in ("avr", "esp32_pcb", "all"):
        print("Употреба: python build_firmware.py [avr|esp32_pcb|all]")
        sys.exit(1)

    build = cfg["build"]
    arduino_cli_path = build["arduino_cli_path"]
    avr_fqbn = build["avr_fqbn"]
    esp32_fqbn = build["esp32_fqbn"]

    total_ok = total_fail = 0

    if which in ("avr", "all") and "build_targets" in cfg:
        # изходната папка "firmware/" - взимаме я от някой съществуващ path в [paths]
        out_root = os.path.dirname(cfg["paths"]["firmware_config_avr"])
        ok, fail = build_set("ATmega + TTGO", arduino_cli_path, build["firmware_src_dir"],
                             cfg["build_targets"], avr_fqbn, esp32_fqbn, out_root)
        total_ok += ok
        total_fail += fail

    if which in ("esp32_pcb", "all") and "build_targets_esp32_pcb" in cfg:
        out_root = os.path.dirname(cfg["paths"]["firmware_pcb_config_esp32"])
        pcb_fqbn = build.get("esp32_pcb_fqbn", "esp32:esp32:esp32")
        ok, fail = build_set("ESP32 PCB", arduino_cli_path, build["firmware_src_dir_esp32_pcb"],
                             cfg["build_targets_esp32_pcb"], avr_fqbn, pcb_fqbn, out_root)
        total_ok += ok
        total_fail += fail

    print(f"\nГотово. Успешни: {total_ok}, Неуспешни: {total_fail}")


if __name__ == "__main__":
    main()
    if getattr(sys, "frozen", False):
        # .exe от двоен клик затваря терминала веднага след края - изчакай, за да се прочете изхода
        input("\nНатисни Enter за изход...")
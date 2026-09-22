import subprocess
import json
import time
import urllib.request

try:
    import serial
except ImportError:
    serial = None


def get_coordinates():
    """Опитва автоматично взимане на координати чрез IP геолокация.
    Връща (lat, lon, error_msg). error_msg е None при успех."""
    try:
        with urllib.request.urlopen("http://ip-api.com/json/", timeout=5) as resp:
            data = json.loads(resp.read().decode())
        if data.get("status") == "success":
            return data["lat"], data["lon"], None
        return None, None, "Неуспешно взимане на координати - въведи ръчно"
    except Exception as e:
        return None, None, f"Грешка при взимане на координати ({e}) - въведи ръчно"


def upload_firmware_avr(avrdude_path, firmware_path, port, baud=115200):
    conf_path = avrdude_path.rsplit(".", 1)[0]  # премахва .exe
    conf_path = conf_path.rsplit("/", 1)
    conf_path = (conf_path[0] + "/avrdude.conf") if len(conf_path) > 1 else "avrdude.conf"
    cmd = [avrdude_path, "-C", conf_path, "-c", "arduino", "-p", "atmega328p", "-P", port,
           "-b", str(baud), "-U", f"flash:w:{firmware_path}:i"]
    try:
        result = subprocess.run(cmd, capture_output=True, text=True)
        return result.returncode == 0, result.stdout + result.stderr
    except Exception as e:
        return False, str(e)


def upload_firmware_esp32(esptool_path, firmware_path, port, baud=921600, address="0x0"):
    """esptool_path сочи към самостоятелния tools/esptool.exe (официален build от
    espressif/esptool GitHub Releases) - НЕ разчита на инсталиран Python/pip esptool,
    работи и след PyInstaller freeze на главното приложение.
    address="0x0" за merged.bin (config-firmware, установява bootloader+partitions+app).
    address="0x10000" за app-only bin (реален firmware) - НЕ пипа NVS партицията!"""
    cmd = [esptool_path, "--port", port, "--baud", str(baud),
           "write_flash", address, firmware_path]
    try:
        result = subprocess.run(cmd, capture_output=True, text=True)
        return result.returncode == 0, result.stdout + result.stderr
    except Exception as e:
        return False, str(e)


def send_config_packet(port, baud, packet_dict, timeout=5, retries=2, ready_wait=15, board_type="avr"):
    """Отваря серийния порт, прави контролиран reset (различен за AVR/ESP32),
    чака 'READY' от firmware-а, после праща CFG пакета и чака ACK/NACK."""
    if serial is None:
        return False, "pyserial не е инсталиран (pip install pyserial)"
    # separators без интервали - firmware-ите очакват компактен JSON ("id":"X" не "id": "X")
    payload = "CFG:" + json.dumps(packet_dict, separators=(",", ":")) + "\n"
    last_err = "Неуспех"
    for attempt in range(retries + 1):
        try:
            with serial.Serial(port, baud, timeout=1) as ser:
                _reset_board(ser, board_type)

                # чакаме READY сигнал (firmware-ът го праща на всеки 500ms)
                ready_deadline = time.time() + ready_wait
                got_ready = False
                while time.time() < ready_deadline:
                    line = ser.readline().decode(errors="ignore").strip()
                    if line == "READY":
                        got_ready = True
                        break
                if not got_ready:
                    last_err = "Не се получи READY от устройството (провери reset/окабеляване)"
                    continue

                ser.write(payload.encode())
                ser.timeout = timeout
                line = ser.readline().decode(errors="ignore").strip()
                if line.startswith("ACK"):
                    return True, "ACK получен"
                elif line.startswith("NACK"):
                    last_err = f"NACK: {line}"
                    continue
                else:
                    last_err = "Няма отговор след CFG (timeout)"
                    continue
        except Exception as e:
            last_err = f"Serial грешка: {e}"
            continue
    return False, last_err


def _reset_board(ser, board_type):
    """Изрично, контролирано рестартиране на платката преди да чакаме READY.
    ESP32 (TTGO) има auto-program circuit (RTS->EN, DTR->IO0) - трябва да го
    рестартираме в НОРМАЛЕН run mode, не bootloader mode."""
    try:
        if board_type == "esp32":
            ser.setDTR(False)   # IO0 -> high (нормален boot, не download mode)
            ser.setRTS(True)    # EN -> low (reset asserted)
            time.sleep(0.1)
            ser.setRTS(False)   # EN -> high (release reset)
            time.sleep(0.1)
        else:  # avr
            ser.setDTR(False)
            time.sleep(0.1)
            ser.setDTR(True)
            time.sleep(0.1)
    except Exception:
        pass  # не всички адаптери поддържат DTR/RTS контрол - продължаваме и без това
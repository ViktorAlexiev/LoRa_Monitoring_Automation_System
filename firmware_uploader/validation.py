import re

MODULE_ID_MAX_LEN = 6
CONSUMER_ID_MAX_LEN = 4
MAX_CONSUMERS = 10

# Букви (главни) в началото, задължително поне 1, после цифри, задължително поне 1
ID_PATTERN = re.compile(r'^[A-Z]+[0-9]+$')
PIN_NUMERIC_PATTERN = re.compile(r'^[0-9]{1,2}$')
PIN_LETTER_PATTERN = re.compile(r'^[A-Z][0-9]$')


def validate_module_id(value):
    if not value:
        return False, "Module ID е празно"
    if len(value) > MODULE_ID_MAX_LEN:
        return False, f"Module ID може да е макс {MODULE_ID_MAX_LEN} символа"
    if not ID_PATTERN.match(value):
        return False, "Module ID: главни букви в началото, после цифри (напр. CS001)"
    return True, ""


def validate_consumer_id(value):
    if not value:
        return False, "Consumer ID е празно"
    if len(value) > CONSUMER_ID_MAX_LEN:
        return False, f"Consumer ID може да е макс {CONSUMER_ID_MAX_LEN} символа"
    if not ID_PATTERN.match(value):
        return False, "Consumer ID: главни букви в началото, после цифри"
    return True, ""


def validate_pin(value, reserved_pins=None):
    if not value:
        return False, "Pin е празно"
    if len(value) > 2:
        return False, "Pin може да е макс 2 символа"
    if not (PIN_NUMERIC_PATTERN.match(value) or PIN_LETTER_PATTERN.match(value)):
        return False, "Pin: число (макс 2 цифри) или главна буква + число (напр. A3)"
    if reserved_pins and value in reserved_pins:
        return False, f"Pin '{value}' е резервиран (виж Settings)"
    return True, ""


def validate_consumers_list(consumers, reserved_pins=None, existing_consumer_ids=None):
    """consumers: list of dicts {'id':..., 'pin':...}
    Стар вариант - връща само първата грешка (пазен за съвместимост)."""
    row_errors, general_errors = validate_consumers_detailed(consumers, reserved_pins, existing_consumer_ids)
    if general_errors:
        return False, general_errors[0]
    if row_errors:
        first_idx = min(row_errors.keys())
        return False, f"Консуматор #{first_idx + 1}: {row_errors[first_idx][0]}"
    return True, ""


def validate_consumers_detailed(consumers, reserved_pins=None, existing_consumer_ids=None):
    """consumers: list of dicts {'id':..., 'pin':...}
    Връща (row_errors, general_errors):
      row_errors: dict {index (0-based): [списък от съобщения за тази конкретна консуматорска редица]}
      general_errors: list от съобщения, които не са специфични за конкретна редица (напр. твърде много редове)
    НЕ спира на първата грешка - събира всички наведнъж."""
    general_errors = []
    row_errors = {}

    if len(consumers) > MAX_CONSUMERS:
        general_errors.append(f"Максимум {MAX_CONSUMERS} консуматора")

    seen_ids = {}
    seen_pins = {}

    for i, c in enumerate(consumers):
        msgs = []
        ok, msg = validate_consumer_id(c['id'])
        if not ok:
            msgs.append(msg)
        ok, msg = validate_pin(c['pin'], reserved_pins)
        if not ok:
            msgs.append(msg)
        if existing_consumer_ids and c['id'] in existing_consumer_ids:
            msgs.append(f"ID '{c['id']}' вече се използва от друг активен executor")

        if c['id']:
            seen_ids.setdefault(c['id'], []).append(i)
        if c['pin']:
            seen_pins.setdefault(c['pin'], []).append(i)

        if msgs:
            row_errors[i] = msgs

    for id_val, idxs in seen_ids.items():
        if len(idxs) > 1:
            for i in idxs:
                row_errors.setdefault(i, []).append(f"дублирано ID '{id_val}' в списъка")

    for pin_val, idxs in seen_pins.items():
        if len(idxs) > 1:
            for i in idxs:
                row_errors.setdefault(i, []).append(f"дублиран pin '{pin_val}' в списъка")

    return row_errors, general_errors


# ---------------------------------------------------------------------
# Gateway (TTGO/ESP32) WiFi/MQTT параметри
# ---------------------------------------------------------------------
WIFI_SSID_MAX_LEN = 32       # WPA2 лимит
WIFI_PASSWORD_MAX_LEN = 63   # WPA2 лимит
MQTT_USER_MAX_LEN = 32
MQTT_PASSWORD_MAX_LEN = 63

IPV4_PATTERN = re.compile(r'^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})$')
HOSTNAME_PATTERN = re.compile(r'^[a-zA-Z0-9]([a-zA-Z0-9\-\.]{0,253}[a-zA-Z0-9])?$')


def validate_wifi_ssid(value):
    if not value:
        return False, "WiFi SSID е празно"
    if len(value) > WIFI_SSID_MAX_LEN:
        return False, f"WiFi SSID може да е макс {WIFI_SSID_MAX_LEN} символа"
    return True, ""


def validate_wifi_password(value):
    # По избор на потребителя - БЕЗ validation на съдържанието, само дължина
    if len(value) > WIFI_PASSWORD_MAX_LEN:
        return False, f"WiFi парола може да е макс {WIFI_PASSWORD_MAX_LEN} символа"
    return True, ""


def validate_mqtt_host(value):
    """Позволява IPv4 адрес ИЛИ hostname (DNS име)."""
    if not value:
        return False, "MQTT адрес е празен"
    m = IPV4_PATTERN.match(value)
    if m:
        if all(0 <= int(g) <= 255 for g in m.groups()):
            return True, ""
        return False, "Невалиден IPv4 адрес"
    if len(value) <= 255 and HOSTNAME_PATTERN.match(value):
        return True, ""
    return False, "Невалиден IP адрес или hostname"


def validate_mqtt_port(value):
    """Празно е позволено (default 1883 се прилага другаде)."""
    if not value:
        return True, ""
    if not value.isdigit():
        return False, "MQTT port трябва да е число"
    port = int(value)
    if port < 1 or port > 65535:
        return False, "MQTT port трябва да е между 1 и 65535"
    return True, ""


def validate_mqtt_user(value):
    if len(value) > MQTT_USER_MAX_LEN:
        return False, f"MQTT user може да е макс {MQTT_USER_MAX_LEN} символа"
    return True, ""


def validate_mqtt_password(value):
    if len(value) > MQTT_PASSWORD_MAX_LEN:
        return False, f"MQTT password може да е макс {MQTT_PASSWORD_MAX_LEN} символа"
    return True, ""


# ---------------------------------------------------------------------
# LoRa честота (MHz, въвежда се от Settings таба - без водещи нули, напр. 433 или 433.5)
# ---------------------------------------------------------------------
# Цяло число (2-3 цифри) по избор последвано от десетична точка + 1-3 цифри за междинна честота.
# Умишлено НЕ приема слепени цифри без точка (напр. "4335") - това е честа грешка при опит
# да се въведе "433.5" без точката.
LORA_FREQ_PATTERN = re.compile(r'^\d{2,3}(\.\d{1,3})?$')

# EU LoRa ISM диапазони, в MHz (433.00 вместо official 433.05, за да не отреже 433 - default-а на приложението)
LORA_EU_BANDS_MHZ = (
    (433.00, 434.79),   # EU433
    (863.00, 870.00),   # EU868
)


def validate_lora_frequency_mhz(value):
    """value: текст от потребителя, честота в MHz (напр. '433' или '433.5').
    Връща (ok, mhz_като_float_или_None, съобщение_за_грешка)."""
    if not value:
        return False, None, "Честотата е празна"
    value = value.strip()
    if not LORA_FREQ_PATTERN.match(value):
        return False, None, (
            "Невалиден формат - въведи цяло число в MHz (напр. 433), или с десетична точка "
            "за междинна честота (напр. 433.5). НЕ слепяй цифрите (напр. 4335 вместо 433.5)."
        )
    mhz = float(value)
    if not any(lo <= mhz <= hi for lo, hi in LORA_EU_BANDS_MHZ):
        return False, None, (
            f"{mhz:g} MHz не е валидна EU LoRa честота - трябва да е в диапазон "
            f"433.05-434.79 MHz или 863-870 MHz"
        )
    return True, mhz, ""
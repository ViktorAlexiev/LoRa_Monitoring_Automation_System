# -*- coding: utf-8 -*-
"""Тестове за РЕАЛНИЯ firmware_uploader/validation.py (не мок) - всички публични
валидатори: module ID, consumer ID, pin, консуматорски списък (с дубликати/резервирани
пинове), WiFi/MQTT полета, LoRa честота."""
import os
import sys
import unittest

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..")))
import validation as v


class TestModuleId(unittest.TestCase):
    def test_valid(self):
        ok, msg = v.validate_module_id("CS001")
        self.assertTrue(ok, msg)

    def test_empty_rejected(self):
        ok, msg = v.validate_module_id("")
        self.assertFalse(ok)

    def test_too_long_rejected(self):
        ok, msg = v.validate_module_id("CS00001")  # 7 знака > MODULE_ID_MAX_LEN=6
        self.assertFalse(ok)

    def test_exactly_max_len_ok(self):
        ok, msg = v.validate_module_id("CS0001")  # точно 6
        self.assertTrue(ok, msg)

    def test_lowercase_rejected(self):
        ok, msg = v.validate_module_id("cs001")
        self.assertFalse(ok)

    def test_digits_only_rejected(self):
        ok, msg = v.validate_module_id("12345")  # трябва поне 1 буква в началото
        self.assertFalse(ok)

    def test_letters_only_rejected(self):
        ok, msg = v.validate_module_id("ABCDE")  # трябва поне 1 цифра
        self.assertFalse(ok)

    def test_number_before_letter_rejected(self):
        ok, msg = v.validate_module_id("1CS00")  # цифри преди буквите - невалидно
        self.assertFalse(ok)


class TestConsumerId(unittest.TestCase):
    def test_valid(self):
        # ID_PATTERN (споделен с module ID) е "букви в началото, после цифри" -
        # напр. "AB12", НЕ смесен формат като "B1C2" (виж test_b1c2_style_id_from_
        # config_avr_comment_example_is_actually_rejected по-долу за находката).
        ok, msg = v.validate_consumer_id("AB12")
        self.assertTrue(ok, msg)

    def test_too_long_rejected(self):
        ok, msg = v.validate_consumer_id("ABCDE")  # 5 > CONSUMER_ID_MAX_LEN=4
        self.assertFalse(ok)

    def test_empty_rejected(self):
        ok, msg = v.validate_consumer_id("")
        self.assertFalse(ok)

    def test_b1c2_style_id_from_config_avr_comment_example_is_actually_rejected(self):
        # НАХОДКА: firmware_src/config_avr/config_avr.ino коментарът показва пример
        # CFG:{"id":"CS001","consumers":[{"id":"B1C2","pin":"A3"},{"id":"D4E5","pin":"10"}]}
        # но "B1C2"/"D4E5" (смесени букви/цифри) НЕ минават реалната валидация в
        # приложението - main.py:669 и ID_PATTERN изискват "букви, после цифри"
        # (напр. "AB12"). Примерът в коментара е подвеждащ/технически невалиден спрямо
        # реалната UI валидация - не е бъг във validation.py, а остарял/грешен пример
        # в код коментар (същата категория като другите поправени тази сесия).
        ok, msg = v.validate_consumer_id("B1C2")
        self.assertFalse(ok, "ако този assert някога падне (стане True), "
                             "config_avr.ino примерът вече не е подвеждащ")
        ok, msg = v.validate_consumer_id("D4E5")
        self.assertFalse(ok)


class TestPin(unittest.TestCase):
    def test_numeric_pin_ok(self):
        ok, msg = v.validate_pin("10")
        self.assertTrue(ok, msg)

    def test_single_digit_ok(self):
        ok, msg = v.validate_pin("5")
        self.assertTrue(ok, msg)

    def test_analog_pin_ok(self):
        ok, msg = v.validate_pin("A3")
        self.assertTrue(ok, msg)

    def test_three_digit_rejected(self):
        ok, msg = v.validate_pin("100")
        self.assertFalse(ok)

    def test_lowercase_letter_rejected(self):
        ok, msg = v.validate_pin("a3")
        self.assertFalse(ok)

    def test_letter_without_digit_rejected(self):
        ok, msg = v.validate_pin("A")
        self.assertFalse(ok)

    def test_empty_rejected(self):
        ok, msg = v.validate_pin("")
        self.assertFalse(ok)

    def test_reserved_pin_rejected(self):
        ok, msg = v.validate_pin("10", reserved_pins=["2", "9", "10"])
        self.assertFalse(ok)
        self.assertIn("резервиран", msg)

    def test_non_reserved_pin_ok_with_reserved_list_present(self):
        ok, msg = v.validate_pin("5", reserved_pins=["2", "9", "10"])
        self.assertTrue(ok, msg)

    def test_reserved_pins_11_12_13_are_rejected(self):
        # Точно поправката от тази сесия (config.ini reserved_pins разширен с 11,12,13)
        reserved = ["2", "9", "10", "11", "12", "13"]
        for pin in ("11", "12", "13"):
            ok, msg = v.validate_pin(pin, reserved_pins=reserved)
            self.assertFalse(ok, f"пин {pin} трябва да е резервиран (SPI на LoRa)")


class TestConsumersDetailed(unittest.TestCase):
    def test_empty_list_no_errors(self):
        row_errors, general_errors = v.validate_consumers_detailed([])
        self.assertEqual(row_errors, {})
        self.assertEqual(general_errors, [])

    def test_valid_list_no_errors(self):
        consumers = [{"id": "AB12", "pin": "10"}, {"id": "CD34", "pin": "A3"}]
        row_errors, general_errors = v.validate_consumers_detailed(consumers)
        self.assertEqual(row_errors, {})
        self.assertEqual(general_errors, [])

    def test_too_many_consumers_general_error(self):
        consumers = [{"id": f"C{i:03d}", "pin": str(i)} for i in range(11)]  # 11 > MAX_CONSUMERS=10
        row_errors, general_errors = v.validate_consumers_detailed(consumers)
        self.assertTrue(any("Максимум" in e for e in general_errors))

    def test_exactly_max_consumers_ok(self):
        consumers = [{"id": f"C{i:03d}"[:4].upper(), "pin": str(i)} for i in range(10)]
        # генерирай валидни 4-символни ID-та (букви+цифри) и уникални пинове
        consumers = [{"id": f"AA{ i:02d}", "pin": str(i)} for i in range(10)]
        row_errors, general_errors = v.validate_consumers_detailed(consumers)
        self.assertEqual(general_errors, [])

    def test_duplicate_id_flagged_on_both_rows(self):
        consumers = [{"id": "B1C2", "pin": "10"}, {"id": "B1C2", "pin": "11"}]
        row_errors, general_errors = v.validate_consumers_detailed(consumers)
        self.assertIn(0, row_errors)
        self.assertIn(1, row_errors)
        self.assertTrue(any("дублирано" in m for m in row_errors[0]))
        self.assertTrue(any("дублирано" in m for m in row_errors[1]))

    def test_duplicate_pin_flagged_on_both_rows(self):
        consumers = [{"id": "AAAA", "pin": "10"}, {"id": "BBBB", "pin": "10"}]
        row_errors, general_errors = v.validate_consumers_detailed(consumers)
        self.assertIn(0, row_errors)
        self.assertIn(1, row_errors)
        self.assertTrue(any("дублиран pin" in m for m in row_errors[0]))

    def test_reserved_pin_in_list_flagged(self):
        consumers = [{"id": "AAAA", "pin": "2"}]
        row_errors, general_errors = v.validate_consumers_detailed(consumers, reserved_pins=["2", "9", "10"])
        self.assertIn(0, row_errors)

    def test_existing_consumer_id_conflict_flagged(self):
        consumers = [{"id": "B1C2", "pin": "10"}]
        row_errors, general_errors = v.validate_consumers_detailed(consumers, existing_consumer_ids={"B1C2"})
        self.assertIn(0, row_errors)
        self.assertTrue(any("вече се използва" in m for m in row_errors[0]))

    def test_empty_id_and_pin_do_not_count_as_duplicates(self):
        # празните полета не трябва да се броят като "дублирани" помежду си
        consumers = [{"id": "", "pin": ""}, {"id": "", "pin": ""}]
        row_errors, general_errors = v.validate_consumers_detailed(consumers)
        for i in (0, 1):
            self.assertNotIn("дублирано", " ".join(row_errors.get(i, [])))
            self.assertNotIn("дублиран pin", " ".join(row_errors.get(i, [])))

    def test_validate_consumers_list_returns_first_error_only(self):
        consumers = [{"id": "", "pin": "10"}]
        ok, msg = v.validate_consumers_list(consumers)
        self.assertFalse(ok)
        self.assertIn("Консуматор #1", msg)


class TestWifiMqtt(unittest.TestCase):
    def test_wifi_ssid_valid(self):
        ok, msg = v.validate_wifi_ssid("MyNetwork")
        self.assertTrue(ok, msg)

    def test_wifi_ssid_empty_rejected(self):
        ok, msg = v.validate_wifi_ssid("")
        self.assertFalse(ok)

    def test_wifi_ssid_too_long_rejected(self):
        ok, msg = v.validate_wifi_ssid("A" * 33)
        self.assertFalse(ok)

    def test_wifi_password_empty_allowed(self):
        ok, msg = v.validate_wifi_password("")
        self.assertTrue(ok, msg)

    def test_wifi_password_too_long_rejected(self):
        ok, msg = v.validate_wifi_password("A" * 64)
        self.assertFalse(ok)

    def test_mqtt_host_ipv4_valid(self):
        ok, msg = v.validate_mqtt_host("192.168.4.2")
        self.assertTrue(ok, msg)

    def test_mqtt_host_ipv4_out_of_range_rejected(self):
        ok, msg = v.validate_mqtt_host("192.168.4.999")
        self.assertFalse(ok)

    def test_mqtt_host_hostname_valid(self):
        ok, msg = v.validate_mqtt_host("mqtt.local")
        self.assertTrue(ok, msg)

    def test_mqtt_host_empty_rejected(self):
        ok, msg = v.validate_mqtt_host("")
        self.assertFalse(ok)

    def test_mqtt_port_empty_allowed(self):
        ok, msg = v.validate_mqtt_port("")
        self.assertTrue(ok, msg)

    def test_mqtt_port_valid(self):
        ok, msg = v.validate_mqtt_port("1883")
        self.assertTrue(ok, msg)

    def test_mqtt_port_zero_rejected(self):
        ok, msg = v.validate_mqtt_port("0")
        self.assertFalse(ok)

    def test_mqtt_port_above_65535_rejected(self):
        ok, msg = v.validate_mqtt_port("65536")
        self.assertFalse(ok)

    def test_mqtt_port_non_numeric_rejected(self):
        ok, msg = v.validate_mqtt_port("abc")
        self.assertFalse(ok)


class TestLoraFrequency(unittest.TestCase):
    def test_433_valid(self):
        ok, mhz, msg = v.validate_lora_frequency_mhz("433")
        self.assertTrue(ok, msg)
        self.assertAlmostEqual(mhz, 433.0)

    def test_434_valid(self):
        ok, mhz, msg = v.validate_lora_frequency_mhz("434")
        self.assertTrue(ok, msg)

    def test_intermediate_with_dot_valid(self):
        ok, mhz, msg = v.validate_lora_frequency_mhz("433.5")
        self.assertTrue(ok, msg)
        self.assertAlmostEqual(mhz, 433.5)

    def test_glued_digits_rejected(self):
        # изрично документирано в кода: "4335" трябва да се отхвърли (опит за 433.5 без точка)
        ok, mhz, msg = v.validate_lora_frequency_mhz("4335")
        self.assertFalse(ok)

    def test_empty_rejected(self):
        ok, mhz, msg = v.validate_lora_frequency_mhz("")
        self.assertFalse(ok)

    def test_out_of_eu_band_rejected(self):
        ok, mhz, msg = v.validate_lora_frequency_mhz("440")
        self.assertFalse(ok)

    def test_868_band_valid(self):
        ok, mhz, msg = v.validate_lora_frequency_mhz("868")
        self.assertTrue(ok, msg)

    def test_boundary_433_exact_valid(self):
        ok, mhz, msg = v.validate_lora_frequency_mhz("433.00"[:6])  # "433.0"
        # LORA_FREQ_PATTERN изисква 1-3 цифри след точката - "433.0" е валиден формат
        ok, mhz, msg = v.validate_lora_frequency_mhz("433.0")
        self.assertTrue(ok, msg)

    def test_just_above_upper_eu433_band_rejected(self):
        ok, mhz, msg = v.validate_lora_frequency_mhz("434.8")  # горна граница е 434.79
        self.assertFalse(ok)

    def test_gap_between_bands_rejected(self):
        ok, mhz, msg = v.validate_lora_frequency_mhz("500")  # между EU433 и EU868
        self.assertFalse(ok)


class TestSfBw(unittest.TestCase):
    def test_sf_valid_range(self):
        for sf in range(7, 13):
            ok, val, msg = v.validate_lora_sf(str(sf))
            self.assertTrue(ok, msg)
            self.assertEqual(val, sf)

    def test_sf_invalid(self):
        for bad in ("6", "13", "", "abc", "7.5", "-7"):
            ok, val, msg = v.validate_lora_sf(bad)
            self.assertFalse(ok, bad)
            self.assertIsNone(val)

    def test_bw_valid_converts_to_hz(self):
        self.assertEqual(v.validate_lora_bw_khz("62.5")[1], 62500)
        self.assertEqual(v.validate_lora_bw_khz("125")[1], 125000)
        self.assertEqual(v.validate_lora_bw_khz("250")[1], 250000)
        self.assertEqual(v.validate_lora_bw_khz("62,5")[1], 62500)

    def test_bw_invalid(self):
        for bad in ("500", "100", "", "x", "0"):
            ok, val, msg = v.validate_lora_bw_khz(bad)
            self.assertFalse(ok, bad)


class TestLanes(unittest.TestCase):
    def test_parse_extra_lanes(self):
        self.assertEqual(v.parse_extra_lanes(""), [])
        self.assertEqual(v.parse_extra_lanes(" 434.5 , ,433.5"), ["434.5", "433.5"])

    def test_valid_two_lanes(self):
        ok, lanes, msg = v.validate_lora_lanes(["433", "434"], 125000)
        self.assertTrue(ok, msg)
        self.assertEqual(lanes, [433.0, 434.0])

    def test_less_than_two_rejected(self):
        self.assertFalse(v.validate_lora_lanes(["433"], 125000)[0])

    def test_too_many_rejected(self):
        many = ["433.1", "433.4", "433.7", "434.0", "434.3", "434.6", "434.7"]
        self.assertFalse(v.validate_lora_lanes(many, 125000)[0])

    def test_duplicate_rejected(self):
        self.assertFalse(v.validate_lora_lanes(["433.5", "433.5"], 125000)[0])

    def test_spacing_at_least_two_bw(self):
        self.assertTrue(v.validate_lora_lanes(["433.5", "433.75"], 125000)[0])
        self.assertFalse(v.validate_lora_lanes(["433.5", "433.7"], 125000)[0])
        # при BW 250 kHz минимумът е 0.5 MHz
        self.assertFalse(v.validate_lora_lanes(["433.5", "433.9"], 250000)[0])
        self.assertTrue(v.validate_lora_lanes(["433.5", "434.0"], 250000)[0])

    def test_invalid_frequency_in_lane_rejected(self):
        ok, lanes, msg = v.validate_lora_lanes(["433.5", "500"], 125000)
        self.assertFalse(ok)
        self.assertIn("Лента 1", msg)


if __name__ == "__main__":
    unittest.main(verbosity=2)

"""Reads config/system.ini - infrastructure connection settings (MQTT broker,
future MySQL server). Separate from app/config.py (business placeholder
defaults, editable live from the admin panel) - this file is edited by hand
and only read by the daemons; the web app itself never touches it.

In Docker (see docker-compose.yml), each daemon container gets its MQTT
connection settings via MQTT_HOST/MQTT_PORT/MQTT_USERNAME/MQTT_PASSWORD
environment variables instead of a mounted system.ini - env vars win when
set, so local dev (system.ini, anonymous localhost:1883) is unaffected.
"""

import configparser
import os
from pathlib import Path

INI_PATH = Path(__file__).resolve().parent.parent / "config" / "system.ini"


def _load():
    parser = configparser.ConfigParser()
    parser.read(INI_PATH, encoding="utf-8")
    return parser


_parser = _load()


def mqtt_settings():
    section = _parser["mqtt"] if _parser.has_section("mqtt") else {}
    return {
        "host": os.environ.get("MQTT_HOST") or section.get("host", "localhost"),
        "port": int(os.environ.get("MQTT_PORT") or section.get("port", "1883")),
        "username": os.environ.get("MQTT_USERNAME") or (section.get("username", "") or None),
        "password": os.environ.get("MQTT_PASSWORD") or (section.get("password", "") or None),
    }


def mysql_settings():
    section = _parser["mysql"] if _parser.has_section("mysql") else {}
    return {
        "host": section.get("host", "") or None,
        "port": int(section.get("port", "3306")),
        "username": section.get("username", "") or None,
        "password": section.get("password", "") or None,
        "database": section.get("database", "") or None,
    }

import os
import sys
import json
import time
import queue
import threading
import configparser
import tkinter as tk
from tkinter import ttk, simpledialog, messagebox

# Работната директория трябва да е папката, в която РЕАЛНО седи .exe/.py-то (не откъдето е
# стартирано - двоен клик обикновено я сеща правилно, но desktop shortcut със свой "Start in",
# или стартиране от cmd в друга папка, не). Всички относителни пътища (config.ini, data/,
# firmware/, tools/) разчитат на това. sys.frozen == True само след PyInstaller build.
if getattr(sys, "frozen", False):
    _BASE_DIR = os.path.dirname(sys.executable)
else:
    _BASE_DIR = os.path.dirname(os.path.abspath(__file__))
os.chdir(_BASE_DIR)

from db import (init_db, add_log, get_all_logs, get_registry_entry, get_all_registry,
                 upsert_registry, retire_device, unretire_device, is_module_id_active,
                 get_all_active_consumer_ids, clear_log, clear_registry)
from validation import (validate_module_id, validate_consumers_detailed,
                        validate_wifi_ssid, validate_wifi_password, validate_mqtt_host,
                        validate_mqtt_port, validate_mqtt_user, validate_mqtt_password,
                        validate_lora_frequency_mhz)
from device_ops import get_coordinates, upload_firmware_avr, upload_firmware_esp32, send_config_packet
from network_key import NETWORK_KEY_HEX


def load_config(path="config.ini"):
    parser = configparser.ConfigParser()
    parser.read(path, encoding="utf-8")
    return parser


def list_serial_ports():
    try:
        from serial.tools import list_ports
        return set(p.device for p in list_ports.comports())
    except Exception:
        return set()


# ---------------------------------------------------------------------------
class FilterableSortableTreeFrame(ttk.Frame):
    """Обща логика за сортиране/филтриране на Treeview колони (клик на header)
    и показване на JSON колони в четим детайлен prozorec (двоен клик)."""

    def _init_filter_sort(self, columns, json_columns=None):
        self.columns = columns
        self.json_columns = json_columns or []
        self.all_rows = []       # list of dict (пълните, нефилтрирани данни)
        self.filters = {}        # col_name -> set(разрешени стойности като str)
        self.sort_col = None
        self.sort_reverse = False

    def _bind_tree_events(self):
        self.tree.bind("<Button-1>", self._on_tree_click, add="+")
        self.tree.bind("<Double-1>", self._on_double_click, add="+")

    def _on_tree_click(self, event):
        region = self.tree.identify_region(event.x, event.y)
        if region != "heading":
            return
        col_id = self.tree.identify_column(event.x)
        try:
            idx = int(col_id.replace("#", "")) - 1
            col_name = self.columns[idx]
        except (ValueError, IndexError):
            return
        self._show_header_menu(event, col_name)

    def _show_header_menu(self, event, col_name):
        menu = tk.Menu(self, tearoff=0)
        menu.add_command(label=f"Сортирай \u2191 по '{col_name}'", command=lambda: self._sort_by(col_name, False))
        menu.add_command(label=f"Сортирай \u2193 по '{col_name}'", command=lambda: self._sort_by(col_name, True))
        menu.add_separator()
        menu.add_command(label="Филтър...", command=lambda: self._open_filter_dialog(col_name))
        if col_name in self.filters:
            menu.add_command(label="Изчисти филтъра", command=lambda: self._clear_filter(col_name))
        menu.tk_popup(event.x_root, event.y_root)

    def _sort_by(self, col_name, reverse):
        self.sort_col = col_name
        self.sort_reverse = reverse
        self._render()

    def _open_filter_dialog(self, col_name):
        values = sorted(set(str(r.get(col_name, "")) for r in self.all_rows))
        dialog = tk.Toplevel(self)
        dialog.title(f"Филтър - {col_name}")
        dialog.geometry("300x420")

        listbox = tk.Listbox(dialog, selectmode="multiple", exportselection=False)
        listbox.pack(fill="both", expand=True, padx=8, pady=8)
        for v in values:
            listbox.insert("end", v if v else "(празно)")

        current_filter = self.filters.get(col_name)
        for i, v in enumerate(values):
            if current_filter is None or v in current_filter:
                listbox.selection_set(i)

        def apply_filter():
            selected = [values[i] for i in listbox.curselection()]
            if len(selected) == 0 or len(selected) == len(values):
                self.filters.pop(col_name, None)
            else:
                self.filters[col_name] = set(selected)
            dialog.destroy()
            self._render()

        btn_frame = ttk.Frame(dialog)
        btn_frame.pack(fill="x", padx=8, pady=8)
        ttk.Button(btn_frame, text="Приложи", command=apply_filter).pack(side="left", padx=4)
        ttk.Button(btn_frame, text="Отказ", command=dialog.destroy).pack(side="left", padx=4)

    def _clear_filter(self, col_name):
        self.filters.pop(col_name, None)
        self._render()

    def _apply_filters_and_sort(self):
        rows = self.all_rows
        for col, allowed in self.filters.items():
            rows = [r for r in rows if str(r.get(col, "")) in allowed]
        if self.sort_col:
            try:
                rows = sorted(rows, key=lambda r: (r.get(self.sort_col) is None, r.get(self.sort_col)),
                              reverse=self.sort_reverse)
            except TypeError:
                rows = sorted(rows, key=lambda r: str(r.get(self.sort_col, "")), reverse=self.sort_reverse)
        return rows

    def _on_double_click(self, event):
        region = self.tree.identify_region(event.x, event.y)
        if region != "cell":
            return
        col_id = self.tree.identify_column(event.x)
        try:
            idx = int(col_id.replace("#", "")) - 1
            col_name = self.columns[idx]
        except (ValueError, IndexError):
            return
        if col_name not in self.json_columns:
            return
        item = self.tree.identify_row(event.y)
        if not item:
            return
        values = self.tree.item(item, "values")
        raw_json = values[idx] if idx < len(values) else ""
        self._show_json_detail(raw_json, title=f"Детайли - {col_name}")

    def _show_json_detail(self, raw_json, title="Детайли"):
        ACCENT = "#0F766E"
        TEXT_PRIMARY = "#2B2A25"
        TEXT_SECONDARY = "#57544C"
        BORDER = "#E4E1D8"
        CARD_BG = "#FFFFFF"
        BG = "#F6F5F1"
        PASSWORD_KEYS = {"wifi_password", "mqtt_password"}

        try:
            parsed = json.loads(raw_json) if raw_json else {}
        except Exception:
            parsed = None

        dialog = tk.Toplevel(self)
        dialog.title(title)
        dialog.geometry("440x420")
        dialog.configure(background=BG)

        # Ако не е валиден JSON - fallback на суров текст
        if parsed is None:
            text = tk.Text(dialog, wrap="word", font=("Consolas", 10),
                            background=CARD_BG, foreground=TEXT_PRIMARY, relief="flat", borderwidth=1)
            text.insert("1.0", raw_json or "(няма данни)")
            text.config(state="disabled")
            text.pack(fill="both", expand=True, padx=8, pady=8)
            ttk.Button(dialog, text="Затвори", command=dialog.destroy).pack(pady=6)
            return

        canvas = tk.Canvas(dialog, background=BG, highlightthickness=0)
        vscroll = ttk.Scrollbar(dialog, orient="vertical", command=canvas.yview)
        scroll_frame = ttk.Frame(canvas)
        scroll_frame.bind("<Configure>", lambda e: canvas.configure(scrollregion=canvas.bbox("all")))
        canvas.create_window((0, 0), window=scroll_frame, anchor="nw")
        canvas.configure(yscrollcommand=vscroll.set)
        canvas.pack(side="left", fill="both", expand=True, padx=(10, 0), pady=10)
        vscroll.pack(side="right", fill="y", pady=10, padx=(0, 10))
        canvas.bind("<MouseWheel>", lambda e: canvas.yview_scroll(int(-1 * (e.delta / 120)), "units"))

        def add_kv_row(parent, key, value):
            row = ttk.Frame(parent)
            row.pack(fill="x", pady=3)
            ttk.Label(row, text=key, foreground=TEXT_SECONDARY, width=15, anchor="w").pack(side="left")

            display_val = str(value) if value != "" else "(празно)"
            is_pw = key in PASSWORD_KEYS and value != ""
            masked = "\u2022" * min(len(display_val), 10)

            val_label = ttk.Label(row, text=masked if is_pw else display_val,
                                   foreground=TEXT_PRIMARY, font=("Segoe UI", 10, "bold"))
            val_label.pack(side="left")

            if is_pw:
                state = {"revealed": False}

                def toggle(event=None):
                    state["revealed"] = not state["revealed"]
                    val_label.config(text=display_val if state["revealed"] else masked)

                val_label.bind("<Button-1>", toggle)
                val_label.config(cursor="hand2")
                ttk.Label(row, text="  (клик за показване)", foreground=TEXT_SECONDARY,
                          font=("Segoe UI", 8)).pack(side="left")

        def render(parent, data, level=0):
            if isinstance(data, dict):
                if not data:
                    ttk.Label(parent, text="Няма допълнителни параметри", foreground=TEXT_SECONDARY,
                              font=("Segoe UI", 10, "italic")).pack(anchor="w", pady=8)
                    return
                for k, v in data.items():
                    if isinstance(v, (dict, list)) and v:
                        ttk.Label(parent, text=k, foreground=ACCENT,
                                  font=("Segoe UI", 10, "bold")).pack(
                            anchor="w", pady=(10 if level == 0 else 6, 3))
                        render(parent, v, level + 1)
                    else:
                        add_kv_row(parent, k, v)
            elif isinstance(data, list):
                if not data:
                    ttk.Label(parent, text="(празен списък)", foreground=TEXT_SECONDARY).pack(anchor="w")
                    return
                for item in data:
                    card = tk.Frame(parent, background=CARD_BG, highlightbackground=BORDER,
                                     highlightthickness=1, bd=0)
                    card.pack(fill="x", pady=4, padx=(level * 10, 0))
                    inner = ttk.Frame(card)
                    inner.pack(fill="x", padx=10, pady=8)
                    if isinstance(item, dict) and set(item.keys()) == {"id", "pin"}:
                        # consumer запис - id и pin едно до друго на един ред
                        row = ttk.Frame(inner)
                        row.pack(fill="x")
                        ttk.Label(row, text="id:", foreground=TEXT_SECONDARY).pack(side="left")
                        ttk.Label(row, text=str(item.get("id", "")), foreground=TEXT_PRIMARY,
                                  font=("Segoe UI", 10, "bold")).pack(side="left", padx=(4, 16))
                        ttk.Label(row, text="pin:", foreground=TEXT_SECONDARY).pack(side="left")
                        ttk.Label(row, text=str(item.get("pin", "")), foreground=TEXT_PRIMARY,
                                  font=("Segoe UI", 10, "bold")).pack(side="left", padx=(4, 0))
                    elif isinstance(item, dict):
                        for k, v in item.items():
                            add_kv_row(inner, k, v)
                    else:
                        ttk.Label(inner, text=str(item), foreground=TEXT_PRIMARY).pack(anchor="w")
            else:
                ttk.Label(parent, text=str(data), foreground=TEXT_PRIMARY).pack(anchor="w")

        render(scroll_frame, parsed)

        btn_frame = ttk.Frame(dialog)
        btn_frame.pack(fill="x", padx=10, pady=(0, 10))
        ttk.Button(btn_frame, text="Затвори", command=dialog.destroy).pack(anchor="e")


# ---------------------------------------------------------------------------
class SimpleDeviceFrame(ttk.Frame):
    """Sensor / Repeater (AVR) и Gateway (ESP32) - само module ID, без консуматори."""

    def __init__(self, parent, app, device_type, is_esp32=False):
        super().__init__(parent)
        self.app = app
        self.device_type = device_type
        self.is_esp32 = is_esp32
        self._build()

    def _build(self):
        row = 0
        ttk.Label(self, text="Module ID (напр. CS001):").grid(row=row, column=0, sticky="w", padx=5, pady=5)
        self.module_id_var = tk.StringVar()
        ttk.Entry(self, textvariable=self.module_id_var, width=10).grid(row=row, column=1, sticky="w")
        self.module_id_error_label = ttk.Label(self, text="", foreground="#DC2626")
        self.module_id_error_label.grid(row=row, column=2, columnspan=2, sticky="w", padx=5)
        row += 1

        # Gateway-специфични полета: WiFi + MQTT (само за device_type == "gateway")
        self.is_gateway = (self.device_type == "gateway")
        if self.is_gateway:
            ttk.Label(self, text="WiFi SSID:").grid(row=row, column=0, sticky="w", padx=5, pady=5)
            self.wifi_ssid_var = tk.StringVar()
            ttk.Entry(self, textvariable=self.wifi_ssid_var, width=20).grid(row=row, column=1, sticky="w")
            self.wifi_ssid_error_label = ttk.Label(self, text="", foreground="#DC2626")
            self.wifi_ssid_error_label.grid(row=row, column=2, columnspan=2, sticky="w", padx=5)
            row += 1

            ttk.Label(self, text="WiFi Password (може празно):").grid(row=row, column=0, sticky="w", padx=5, pady=5)
            self.wifi_password_var = tk.StringVar()
            pw_frame1 = ttk.Frame(self)
            pw_frame1.grid(row=row, column=1, sticky="w")
            self.wifi_password_entry = ttk.Entry(pw_frame1, textvariable=self.wifi_password_var, width=17, show="*")
            self.wifi_password_entry.pack(side="left")
            ttk.Button(pw_frame1, text="\U0001F441", width=3,
                       command=lambda: self._toggle_password_visibility(self.wifi_password_entry)).pack(side="left", padx=(2, 0))
            self.wifi_password_error_label = ttk.Label(self, text="", foreground="#DC2626")
            self.wifi_password_error_label.grid(row=row, column=2, columnspan=2, sticky="w", padx=5)
            row += 1

            ttk.Label(self, text="MQTT IP/host:").grid(row=row, column=0, sticky="w", padx=5, pady=5)
            self.mqtt_ip_var = tk.StringVar()
            ttk.Entry(self, textvariable=self.mqtt_ip_var, width=20).grid(row=row, column=1, sticky="w")
            self.mqtt_ip_error_label = ttk.Label(self, text="", foreground="#DC2626")
            self.mqtt_ip_error_label.grid(row=row, column=2, columnspan=2, sticky="w", padx=5)
            row += 1

            ttk.Label(self, text="MQTT Port (default 1883):").grid(row=row, column=0, sticky="w", padx=5, pady=5)
            self.mqtt_port_var = tk.StringVar()
            ttk.Entry(self, textvariable=self.mqtt_port_var, width=8).grid(row=row, column=1, sticky="w")
            self.mqtt_port_error_label = ttk.Label(self, text="", foreground="#DC2626")
            self.mqtt_port_error_label.grid(row=row, column=2, columnspan=2, sticky="w", padx=5)
            row += 1

            ttk.Label(self, text="MQTT User (може празно):").grid(row=row, column=0, sticky="w", padx=5, pady=5)
            self.mqtt_user_var = tk.StringVar()
            ttk.Entry(self, textvariable=self.mqtt_user_var, width=20).grid(row=row, column=1, sticky="w")
            self.mqtt_user_error_label = ttk.Label(self, text="", foreground="#DC2626")
            self.mqtt_user_error_label.grid(row=row, column=2, columnspan=2, sticky="w", padx=5)
            row += 1

            ttk.Label(self, text="MQTT Password (може празно):").grid(row=row, column=0, sticky="w", padx=5, pady=5)
            self.mqtt_password_var = tk.StringVar()
            pw_frame2 = ttk.Frame(self)
            pw_frame2.grid(row=row, column=1, sticky="w")
            self.mqtt_password_entry = ttk.Entry(pw_frame2, textvariable=self.mqtt_password_var, width=17, show="*")
            self.mqtt_password_entry.pack(side="left")
            ttk.Button(pw_frame2, text="\U0001F441", width=3,
                       command=lambda: self._toggle_password_visibility(self.mqtt_password_entry)).pack(side="left", padx=(2, 0))
            self.mqtt_password_error_label = ttk.Label(self, text="", foreground="#DC2626")
            self.mqtt_password_error_label.grid(row=row, column=2, columnspan=2, sticky="w", padx=5)
            row += 1

        # LoRa честота - идва от config.ini [lora] (Settings таб), не се редактира тук.
        # Sensor избира МЕЖДУ двете (директно/repeater), Repeater/Gateway само показват
        # honestно каква честота ще получат (RX/TX за Repeater, единствена за Gateway).
        self.is_sensor = (self.device_type == "sensor")
        self.is_repeater = (self.device_type == "repeater")
        if self.is_sensor or self.is_repeater or self.is_gateway:
            gw_hz, rp_hz = self.app.get_lora_frequencies_hz()
            self.freq_gw_hz = gw_hz
            self.freq_rp_hz = rp_hz

            if self.is_sensor:
                ttk.Label(self, text="LoRa честота:").grid(row=row, column=0, sticky="w", padx=5, pady=5)
                self.freq_choice_var = tk.StringVar(value="gateway")
                freq_frame = ttk.Frame(self)
                freq_frame.grid(row=row, column=1, columnspan=3, sticky="w")
                self.freq_gw_radio = ttk.Radiobutton(
                    freq_frame, value="gateway", variable=self.freq_choice_var,
                    text=f"Директно до Gateway ({gw_hz / 1e6:g} MHz)")
                self.freq_gw_radio.pack(anchor="w")
                self.freq_rp_radio = ttk.Radiobutton(
                    freq_frame, value="repeater", variable=self.freq_choice_var,
                    text=f"През Repeater ({rp_hz / 1e6:g} MHz)")
                self.freq_rp_radio.pack(anchor="w")
            else:
                text = (f"LoRa честота: RX {rp_hz / 1e6:g} MHz (от Sensor) / "
                        f"TX {gw_hz / 1e6:g} MHz (към Gateway)") if self.is_repeater \
                    else f"LoRa честота: {gw_hz / 1e6:g} MHz"
                self.freq_info_label = ttk.Label(self, text=text)
                self.freq_info_label.grid(row=row, column=0, columnspan=4, sticky="w", padx=5, pady=5)
            row += 1

        ttk.Label(self, text="Latitude:").grid(row=row, column=0, sticky="w", padx=5)
        self.lat_var = tk.StringVar()
        ttk.Entry(self, textvariable=self.lat_var, width=15).grid(row=row, column=1, sticky="w")
        ttk.Label(self, text="Longitude:").grid(row=row, column=2, sticky="w")
        self.lon_var = tk.StringVar()
        ttk.Entry(self, textvariable=self.lon_var, width=15).grid(row=row, column=3, sticky="w")
        row += 1

        self.coord_error_label = ttk.Label(self, text="", foreground="#D97706", wraplength=550, justify="left")
        self.coord_error_label.grid(row=row, column=0, columnspan=4, sticky="w", padx=5)
        row += 1

        ttk.Label(self, text="COM Port (общ за всички табове):").grid(row=row, column=0, sticky="w", padx=5)
        self.port_var = self.app.port_var  # СПОДЕЛЕНА променлива между всички device табове
        self.port_combo = ttk.Combobox(self, textvariable=self.port_var, width=12)
        self.port_combo.grid(row=row, column=1, sticky="w")
        ttk.Button(self, text="Refresh", command=self.refresh_ports).grid(row=row, column=2, sticky="w")
        self.port_error_label = ttk.Label(self, text="", foreground="#DC2626")
        self.port_error_label.grid(row=row, column=3, sticky="w", padx=5)
        row += 1

        ttk.Button(self, text="Update / Upload", command=self.on_update, style="Accent.TButton").grid(row=row, column=1, pady=10, sticky="w")
        row += 1

        self.status_label = ttk.Label(self, text="", foreground="#DC2626", wraplength=550, justify="left")
        self.status_label.grid(row=row, column=0, columnspan=4, sticky="w", padx=5)
        row += 1

        rules = "Правила:\n- Module ID: макс 6 символа, главни букви в началото, после цифри (напр. CS001)\n"
        ttk.Label(self, text=rules, foreground="#57544C", justify="left").grid(
            row=row, column=0, columnspan=4, sticky="w", padx=5, pady=10)

        self.refresh_ports()

    def _toggle_password_visibility(self, entry):
        entry.config(show="" if entry.cget("show") == "*" else "*")

    def refresh_ports(self):
        ports = sorted(list_serial_ports())
        self.port_combo["values"] = ports
        if ports and not self.port_var.get():
            self.port_var.set(ports[0])

    def _clear_messages(self):
        """Изчиства всички статус/грешка съобщения - извиква се при връщане на таба."""
        self.module_id_error_label.config(text="")
        self.port_error_label.config(text="")
        self.status_label.config(text="")
        if self.is_gateway:
            self.wifi_ssid_error_label.config(text="")
            self.wifi_password_error_label.config(text="")
            self.mqtt_ip_error_label.config(text="")
            self.mqtt_port_error_label.config(text="")
            self.mqtt_user_error_label.config(text="")
            self.mqtt_password_error_label.config(text="")

    def _refresh_freq_display(self):
        if not (self.is_sensor or self.is_repeater or self.is_gateway):
            return
        gw_hz, rp_hz = self.app.get_lora_frequencies_hz()
        self.freq_gw_hz = gw_hz
        self.freq_rp_hz = rp_hz
        if self.is_sensor:
            self.freq_gw_radio.config(text=f"Директно до Gateway ({gw_hz / 1e6:g} MHz)")
            self.freq_rp_radio.config(text=f"През Repeater ({rp_hz / 1e6:g} MHz)")
        elif self.is_repeater:
            self.freq_info_label.config(
                text=f"LoRa честота: RX {rp_hz / 1e6:g} MHz (от Sensor) / TX {gw_hz / 1e6:g} MHz (към Gateway)")
        else:
            self.freq_info_label.config(text=f"LoRa честота: {gw_hz / 1e6:g} MHz")

    def on_tab_selected(self):
        self._clear_messages()
        self._refresh_freq_display()
        lat, lon, err = get_coordinates()
        if err:
            self.coord_error_label.config(text=err)
        else:
            self.coord_error_label.config(text="")
            self.lat_var.set(str(lat))
            self.lon_var.set(str(lon))

    def on_update(self):
        self._clear_messages()
        has_errors = False

        module_id = self.module_id_var.get().strip().upper()
        ok, msg = validate_module_id(module_id)
        if not ok:
            self.module_id_error_label.config(text=msg)
            has_errors = True
        elif is_module_id_active(self.app.db_path, module_id):
            self.module_id_error_label.config(text="вече е активно (Retire от Registry)")
            has_errors = True

        wifi_ssid = wifi_password = mqtt_ip = mqtt_user = mqtt_password = ""
        mqtt_port = "1883"
        if self.is_gateway:
            wifi_ssid = self.wifi_ssid_var.get().strip()
            ok, m = validate_wifi_ssid(wifi_ssid)
            if not ok:
                self.wifi_ssid_error_label.config(text=m)
                has_errors = True

            wifi_password = self.wifi_password_var.get()
            ok, m = validate_wifi_password(wifi_password)
            if not ok:
                self.wifi_password_error_label.config(text=m)
                has_errors = True

            mqtt_ip = self.mqtt_ip_var.get().strip()
            ok, m = validate_mqtt_host(mqtt_ip)
            if not ok:
                self.mqtt_ip_error_label.config(text=m)
                has_errors = True

            mqtt_port_input = self.mqtt_port_var.get().strip()
            ok, m = validate_mqtt_port(mqtt_port_input)
            if not ok:
                self.mqtt_port_error_label.config(text=m)
                has_errors = True
            else:
                mqtt_port = mqtt_port_input if mqtt_port_input else "1883"

            mqtt_user = self.mqtt_user_var.get().strip()
            ok, m = validate_mqtt_user(mqtt_user)
            if not ok:
                self.mqtt_user_error_label.config(text=m)
                has_errors = True

            mqtt_password = self.mqtt_password_var.get()
            ok, m = validate_mqtt_password(mqtt_password)
            if not ok:
                self.mqtt_password_error_label.config(text=m)
                has_errors = True

        lat, lon = None, None
        try:
            lat = float(self.lat_var.get()) if self.lat_var.get() else None
            lon = float(self.lon_var.get()) if self.lon_var.get() else None
        except ValueError:
            self.coord_error_label.config(text="Невалидни координати")
            has_errors = True

        port = self.port_var.get()
        if not port:
            self.port_error_label.config(text="Избери порт")
            has_errors = True

        if has_errors:
            self.status_label.config(text="Има грешки - виж маркираните полета по-горе.", foreground="#DC2626")
            return

        paths = self.app.cfg["paths"]
        if self.is_esp32:
            ok, out = upload_firmware_esp32(paths["esptool_path"], paths["firmware_config_esp32"], port,
                                             baud=int(self.app.cfg["serial"]["baud_upload_esp32"]))
        else:
            ok, out = upload_firmware_avr(paths["avrdude_path"], paths["firmware_config_avr"], port,
                                       baud=int(self.app.cfg["serial"]["baud_upload_avr"]))
        if not ok:
            self.status_label.config(text=f"Грешка при upload на config-firmware: {out}", foreground="#DC2626")
            return

        packet = {"id": module_id, "key": NETWORK_KEY_HEX}
        if self.is_gateway:
            packet.update({
                "wifi_ssid": wifi_ssid,
                "wifi_password": wifi_password,
                "mqtt_ip": mqtt_ip,
                "mqtt_port": mqtt_port,
                "mqtt_user": mqtt_user,
                "mqtt_password": mqtt_password,
            })
            packet["freq"] = str(self.freq_gw_hz)
        if self.is_sensor:
            freq_hz = self.freq_gw_hz if self.freq_choice_var.get() == "gateway" else self.freq_rp_hz
            packet["freq"] = str(freq_hz)
        if self.is_repeater:
            packet["freq"] = str(self.freq_rp_hz)        # RX - идва от Sensor (EEPROM addr 67)
            packet["freq_tx"] = str(self.freq_gw_hz)      # TX - към Gateway (EEPROM addr 71)
        cfg_baud_key = "baud_config_esp32" if self.is_esp32 else "baud_config_avr"
        ok, msg2 = send_config_packet(port, int(self.app.cfg["serial"][cfg_baud_key]), packet,
                                       timeout=int(self.app.cfg["serial"]["config_timeout"]),
                                       board_type="esp32" if self.is_esp32 else "avr")
        if not ok:
            self.status_label.config(text=f"Грешка при config пакет: {msg2}", foreground="#DC2626")
            return

        real_fw = paths[f"firmware_{self.device_type}"]
        if self.is_esp32:
            # app-only bin на 0x10000 - НЕ пипа NVS партицията (там е config-а, качен току-що)
            ok, out = upload_firmware_esp32(paths["esptool_path"], real_fw, port,
                                             baud=int(self.app.cfg["serial"]["baud_upload_esp32"]),
                                             address="0x10000")
        else:
            ok, out = upload_firmware_avr(paths["avrdude_path"], real_fw, port,
                                       baud=int(self.app.cfg["serial"]["baud_upload_avr"]))
        if not ok:
            self.status_label.config(text=f"Грешка при upload на firmware: {out}", foreground="#DC2626")
            return

        gw_params = {}
        if self.is_gateway:
            gw_params = {
                "wifi_ssid": wifi_ssid,
                "wifi_password": wifi_password,
                "mqtt_ip": mqtt_ip,
                "mqtt_port": mqtt_port,
                "mqtt_user": mqtt_user,
                "mqtt_password": mqtt_password,
                "freq_hz": self.freq_gw_hz,
            }
        if self.is_sensor:
            gw_params = {
                "freq_choice": self.freq_choice_var.get(),
                "freq_hz": self.freq_gw_hz if self.freq_choice_var.get() == "gateway" else self.freq_rp_hz,
            }
        if self.is_repeater:
            gw_params = {"freq_rx_hz": self.freq_rp_hz, "freq_tx_hz": self.freq_gw_hz}
        upsert_registry(self.app.db_path, module_id, self.device_type, gw_params, lat, lon)
        add_log(self.app.db_path, self.device_type, module_id, gw_params, lat, lon, action="upload")
        self.status_label.config(text="Успешно качено!", foreground="#16A34A")
        self.app.refresh_registry()
        self.app.refresh_log()


# ---------------------------------------------------------------------------
class ExecutorFrame(ttk.Frame):
    def __init__(self, parent, app):
        super().__init__(parent)
        self.app = app
        self.device_type = "executor"
        self.consumer_rows = []
        self._build()

    def _build(self):
        row = 0
        ttk.Label(self, text="Module ID (напр. CS001):").grid(row=row, column=0, sticky="w", padx=5, pady=5)
        self.module_id_var = tk.StringVar()
        ttk.Entry(self, textvariable=self.module_id_var, width=10).grid(row=row, column=1, sticky="w")
        self.module_id_error_label = ttk.Label(self, text="", foreground="#DC2626")
        self.module_id_error_label.grid(row=row, column=2, columnspan=2, sticky="w", padx=5)
        row += 1

        # Executor говори директно с Gateway - показва (не редактира) Gateway честотата.
        gw_hz, _rp_hz = self.app.get_lora_frequencies_hz()
        self.freq_gw_hz = gw_hz
        ttk.Label(self, text="LoRa честота:").grid(row=row, column=0, sticky="w", padx=5, pady=5)
        self.freq_info_label = ttk.Label(self, text=f"{gw_hz / 1e6:g} MHz (към Gateway)")
        self.freq_info_label.grid(row=row, column=1, columnspan=3, sticky="w")
        row += 1

        ttk.Label(self, text="Консуматори:").grid(row=row, column=0, sticky="nw", padx=5, pady=5)
        self.consumers_frame = ttk.Frame(self)
        self.consumers_frame.grid(row=row, column=1, columnspan=3, sticky="w")
        row += 1

        ttk.Button(self, text="+ Добави консуматор", command=self.add_consumer_row).grid(
            row=row, column=1, sticky="w", pady=5)
        self.consumers_general_error_label = ttk.Label(self, text="", foreground="#DC2626")
        self.consumers_general_error_label.grid(row=row, column=2, columnspan=2, sticky="w", padx=5)
        row += 1

        ttk.Label(self, text="Latitude:").grid(row=row, column=0, sticky="w", padx=5)
        self.lat_var = tk.StringVar()
        ttk.Entry(self, textvariable=self.lat_var, width=15).grid(row=row, column=1, sticky="w")
        ttk.Label(self, text="Longitude:").grid(row=row, column=2, sticky="w")
        self.lon_var = tk.StringVar()
        ttk.Entry(self, textvariable=self.lon_var, width=15).grid(row=row, column=3, sticky="w")
        row += 1

        self.coord_error_label = ttk.Label(self, text="", foreground="#D97706", wraplength=550, justify="left")
        self.coord_error_label.grid(row=row, column=0, columnspan=4, sticky="w", padx=5)
        row += 1

        ttk.Label(self, text="COM Port (общ за всички табове):").grid(row=row, column=0, sticky="w", padx=5)
        self.port_var = self.app.port_var  # СПОДЕЛЕНА променлива между всички device табове
        self.port_combo = ttk.Combobox(self, textvariable=self.port_var, width=12)
        self.port_combo.grid(row=row, column=1, sticky="w")
        ttk.Button(self, text="Refresh", command=self.refresh_ports).grid(row=row, column=2, sticky="w")
        self.port_error_label = ttk.Label(self, text="", foreground="#DC2626")
        self.port_error_label.grid(row=row, column=3, sticky="w", padx=5)
        row += 1

        ttk.Button(self, text="Update / Upload", command=self.on_update, style="Accent.TButton").grid(row=row, column=1, pady=10, sticky="w")
        row += 1

        self.status_label = ttk.Label(self, text="", foreground="#DC2626", wraplength=550, justify="left")
        self.status_label.grid(row=row, column=0, columnspan=4, sticky="w", padx=5)
        row += 1

        rules = (
            "Правила:\n"
            "- Module ID: макс 6 символа, главни букви в началото, после цифри (напр. CS001)\n"
            "- Consumer ID: макс 4 символа, главни букви в началото, после цифри, глобално уникално\n"
            "- Pin: макс 2 символа - число (макс 2 цифри) или главна буква + число (напр. A3)\n"
            "- Максимум 10 консуматора, без дублирани ID/pin в списъка\n"
        )
        ttk.Label(self, text=rules, foreground="#57544C", justify="left").grid(
            row=row, column=0, columnspan=4, sticky="w", padx=5, pady=10)

        self.add_consumer_row()
        self.refresh_ports()

    def add_consumer_row(self):
        if len(self.consumer_rows) >= 10:
            self.consumers_general_error_label.config(text="Максимум 10 консуматора")
            return
        outer = ttk.Frame(self.consumers_frame)
        outer.pack(fill="x", pady=2)

        input_row = ttk.Frame(outer)
        input_row.pack(fill="x")
        id_var = tk.StringVar()
        pin_var = tk.StringVar()
        ttk.Label(input_row, text="ID:").pack(side="left")
        ttk.Entry(input_row, textvariable=id_var, width=6).pack(side="left", padx=2)
        ttk.Label(input_row, text="Pin:").pack(side="left")
        ttk.Entry(input_row, textvariable=pin_var, width=4).pack(side="left", padx=2)
        remove_btn = ttk.Button(input_row, text="-", width=3)
        remove_btn.pack(side="left", padx=2)

        row_error_label = ttk.Label(outer, text="", foreground="#DC2626")
        row_error_label.pack(anchor="w")

        row_data = {"frame": outer, "id_var": id_var, "pin_var": pin_var, "error_label": row_error_label}
        remove_btn.config(command=lambda: self.remove_consumer_row(row_data))
        self.consumer_rows.append(row_data)

    def remove_consumer_row(self, row_data):
        row_data["frame"].destroy()
        self.consumer_rows.remove(row_data)

    def refresh_ports(self):
        ports = sorted(list_serial_ports())
        self.port_combo["values"] = ports
        if ports and not self.port_var.get():
            self.port_var.set(ports[0])

    def _clear_messages(self):
        """Изчиства всички статус/грешка съобщения - извиква се при връщане на таба."""
        self.module_id_error_label.config(text="")
        self.consumers_general_error_label.config(text="")
        self.port_error_label.config(text="")
        self.status_label.config(text="")
        for r in self.consumer_rows:
            r["error_label"].config(text="")

    def on_tab_selected(self):
        self._clear_messages()
        gw_hz, _rp_hz = self.app.get_lora_frequencies_hz()
        self.freq_gw_hz = gw_hz
        self.freq_info_label.config(text=f"{gw_hz / 1e6:g} MHz (към Gateway)")
        lat, lon, err = get_coordinates()
        if err:
            self.coord_error_label.config(text=err)
        else:
            self.coord_error_label.config(text="")
            self.lat_var.set(str(lat))
            self.lon_var.set(str(lon))

    def on_update(self):
        self._clear_messages()
        has_errors = False

        module_id = self.module_id_var.get().strip().upper()
        ok, msg = validate_module_id(module_id)
        if not ok:
            self.module_id_error_label.config(text=msg)
            has_errors = True
        elif is_module_id_active(self.app.db_path, module_id):
            self.module_id_error_label.config(text="вече е активно (Retire от Registry)")
            has_errors = True

        consumers = []
        for r in self.consumer_rows:
            cid = r["id_var"].get().strip().upper()
            pin = r["pin_var"].get().strip().upper()
            consumers.append({"id": cid, "pin": pin})

        reserved_pins = self.app.get_reserved_pins()
        existing_ids = get_all_active_consumer_ids(self.app.db_path, exclude_module_id=module_id)
        row_errors, general_errors = validate_consumers_detailed(consumers, reserved_pins, existing_ids)

        if general_errors:
            self.consumers_general_error_label.config(text="; ".join(general_errors))
            has_errors = True
        if row_errors:
            has_errors = True
            for idx, msgs in row_errors.items():
                if idx < len(self.consumer_rows):
                    self.consumer_rows[idx]["error_label"].config(text="; ".join(msgs))

        lat, lon = None, None
        try:
            lat = float(self.lat_var.get()) if self.lat_var.get() else None
            lon = float(self.lon_var.get()) if self.lon_var.get() else None
        except ValueError:
            self.coord_error_label.config(text="Невалидни координати")
            has_errors = True

        port = self.port_var.get()
        if not port:
            self.port_error_label.config(text="Избери порт")
            has_errors = True

        if has_errors:
            self.status_label.config(text="Има грешки - виж маркираните полета по-горе.", foreground="#DC2626")
            return

        paths = self.app.cfg["paths"]
        ok, out = upload_firmware_avr(paths["avrdude_path"], paths["firmware_config_avr"], port,
                                       baud=int(self.app.cfg["serial"]["baud_upload_avr"]))
        if not ok:
            self.status_label.config(text=f"Грешка при upload на config-firmware: {out}", foreground="#DC2626")
            return

        packet = {"id": module_id, "consumers": consumers, "freq": str(self.freq_gw_hz),
                  "key": NETWORK_KEY_HEX}
        ok, msg2 = send_config_packet(port, int(self.app.cfg["serial"]["baud_config_avr"]), packet,
                                       timeout=int(self.app.cfg["serial"]["config_timeout"]))
        if not ok:
            self.status_label.config(text=f"Грешка при config пакет: {msg2}", foreground="#DC2626")
            return

        ok, out = upload_firmware_avr(paths["avrdude_path"], paths["firmware_executor"], port,
                                       baud=int(self.app.cfg["serial"]["baud_upload_avr"]))
        if not ok:
            self.status_label.config(text=f"Грешка при upload на executor firmware: {out}", foreground="#DC2626")
            return

        params = {"consumers": consumers, "freq_hz": self.freq_gw_hz}
        upsert_registry(self.app.db_path, module_id, "executor", params, lat, lon)
        add_log(self.app.db_path, "executor", module_id, params, lat, lon, action="upload")
        self.status_label.config(text="Успешно качено!", foreground="#16A34A")
        self.app.refresh_registry()
        self.app.refresh_log()


# ---------------------------------------------------------------------------
class RegistryFrame(FilterableSortableTreeFrame):
    def __init__(self, parent, app):
        super().__init__(parent)
        self.app = app
        cols = ("module_id", "device_type", "status", "parameters", "last_upload", "latitude", "longitude")
        self._init_filter_sort(cols, json_columns=["parameters"])
        self._build()

    def _build(self):
        self.tree = ttk.Treeview(self, columns=self.columns, show="headings")
        for c in self.columns:
            self.tree.heading(c, text=c)
            self.tree.column(c, width=100)
        self.tree.pack(fill="both", expand=True, padx=5, pady=5)
        self._bind_tree_events()
        self.tree.bind("<Button-3>", self._on_right_click)

        btn_frame = ttk.Frame(self)
        btn_frame.pack(fill="x", padx=5, pady=5)
        ttk.Button(btn_frame, text="Retire избрано", command=self.retire_selected).pack(side="left", padx=2)
        ttk.Button(btn_frame, text="Un-retire избрано", command=self.unretire_selected).pack(side="left", padx=2)
        ttk.Button(btn_frame, text="Refresh", command=self.refresh).pack(side="left", padx=2)
        ttk.Button(btn_frame, text="Изтрий целия регистър", command=self.clear_all).pack(side="left", padx=(20, 2))

        ttk.Label(self, text="⚠ 'Изтрий целия регистър' трие ВСИЧКИ устройства (активни и retired) завинаги - "
                              "module ID-тата стават свободни отново, а данните какво е качено на всяко устройство "
                              "(честота/WiFi/консуматори) се губят безвъзвратно.",
                  foreground="#D97706", wraplength=700, justify="left").pack(anchor="w", padx=5, pady=(0, 5))

        self.status_label = ttk.Label(self, text="", foreground="#DC2626")
        self.status_label.pack(fill="x", padx=5)

        ttk.Label(self, text="Клик на заглавие на колона = сортиране/филтриране. "
                              "Двоен клик на 'parameters' = детайли.",
                  foreground="#57544C").pack(anchor="w", padx=5, pady=(0, 5))

        self.refresh()

    def clear_all(self):
        self.status_label.config(text="")
        if not self.all_rows:
            self.status_label.config(text="Регистърът вече е празен.", foreground="#57544C")
            return
        confirmed = messagebox.askyesno(
            "Изтриване на регистъра",
            "Наистина ли искаш да изтриеш ЦЕЛИЯ регистър на устройства?\n\n"
            "Това действие е НЕОБРАТИМО - всички активни и retired записи ще бъдат изтрити завинаги. "
            "Всички module ID-та ще станат отново свободни за употреба, а данните какво е качено на "
            "всяко устройство (честота/WiFi/MQTT/консуматори) ще бъдат загубени.\n\n"
            "Логът (историята на ъпдейтите) НЕ се трие от това действие.",
            icon="warning"
        )
        if not confirmed:
            return
        clear_registry(self.app.db_path)
        self.refresh()
        self.status_label.config(text="Регистърът е изтрит.", foreground="#16A34A")

    def refresh(self):
        rows = get_all_registry(self.app.db_path)
        self.all_rows = [dict(r) for r in rows]
        self._render()

    def _apply_filters_and_sort(self):
        rows = super()._apply_filters_and_sort()
        if self.sort_col is None:
            # default подредба - активните устройства отгоре, retired отдолу
            # (stable sort - запазва module_id подредбата вътре във всяка група)
            rows = sorted(rows, key=lambda r: 0 if r.get("status") == "active" else 1)
        return rows

    def _render(self):
        for row in self.tree.get_children():
            self.tree.delete(row)
        rows = self._apply_filters_and_sort()
        for r in rows:
            tags = ("retired",) if r.get("status") == "retired" else ()
            values = tuple(r.get(c, "") for c in self.columns)
            self.tree.insert("", "end", iid=r["module_id"], values=values, tags=tags)
        self.tree.tag_configure("retired", foreground="#8A8778", background="#EFEDE6")

    def on_tab_selected(self):
        self.status_label.config(text="")
        self.refresh()

    def _on_right_click(self, event):
        item = self.tree.identify_row(event.y)
        if not item:
            return
        self.tree.selection_set(item)
        entry = get_registry_entry(self.app.db_path, item)
        menu = tk.Menu(self, tearoff=0)
        if entry and entry["status"] == "retired":
            menu.add_command(label=f"Un-retire '{item}'", command=self.unretire_selected)
        else:
            menu.add_command(label=f"Retire '{item}'...", command=self.retire_selected)
        menu.tk_popup(event.x_root, event.y_root)

    def retire_selected(self):
        self.status_label.config(text="")
        sel = self.tree.selection()
        if not sel:
            return
        module_id = sel[0]
        notes = simpledialog.askstring("Retire", "Бележка (по избор):") or ""
        retire_device(self.app.db_path, module_id, notes)
        self.refresh()
        self.app.refresh_log()

    def unretire_selected(self):
        self.status_label.config(text="")
        sel = self.tree.selection()
        if not sel:
            return
        module_id = sel[0]
        entry = get_registry_entry(self.app.db_path, module_id)
        if entry and entry["device_type"] == "executor":
            existing_ids = get_all_active_consumer_ids(self.app.db_path, exclude_module_id=module_id)
            try:
                params = json.loads(entry["parameters"])
                for c in params.get("consumers", []):
                    if c["id"] in existing_ids:
                        self.status_label.config(
                            text=f"Не може un-retire: consumer ID '{c['id']}' вече се ползва от друг активен executor.")
                        return
            except Exception:
                pass
        unretire_device(self.app.db_path, module_id)
        self.refresh()
        self.app.refresh_log()


# ---------------------------------------------------------------------------
class LogFrame(FilterableSortableTreeFrame):
    def __init__(self, parent, app):
        super().__init__(parent)
        self.app = app
        cols = ("timestamp", "action", "device_type", "module_id", "parameters", "latitude", "longitude")
        self._init_filter_sort(cols, json_columns=["parameters"])
        self._build()

    def _build(self):
        self.tree = ttk.Treeview(self, columns=self.columns, show="headings")
        for c in self.columns:
            self.tree.heading(c, text=c)
            self.tree.column(c, width=170 if c == "timestamp" else 110)
        self.tree.pack(fill="both", expand=True, padx=5, pady=5)
        self._bind_tree_events()

        ttk.Label(self, text="Клик на заглавие на колона = сортиране/филтриране. "
                              "Двоен клик на 'parameters' = детайли.",
                  foreground="#57544C").pack(anchor="w", padx=5)

        btn_frame = ttk.Frame(self)
        btn_frame.pack(fill="x", padx=5, pady=5)
        ttk.Button(btn_frame, text="Refresh", command=self.refresh).pack(side="left")
        ttk.Button(btn_frame, text="Изтрий лога", command=self.clear_all).pack(side="left", padx=(20, 2))

        ttk.Label(self, text="⚠ 'Изтрий лога' трие ЦЯЛАТА история на upload/retire/unretire действия завинаги. "
                              "Не засяга Registry таба (текущите активни устройства остават).",
                  foreground="#D97706", wraplength=700, justify="left").pack(anchor="w", padx=5, pady=(0, 5))

        self.status_label = ttk.Label(self, text="", foreground="#DC2626")
        self.status_label.pack(fill="x", padx=5)

        self.refresh()

    def refresh(self):
        rows = get_all_logs(self.app.db_path)
        self.all_rows = [dict(r) for r in rows]
        self._render()

    def clear_all(self):
        self.status_label.config(text="")
        if not self.all_rows:
            self.status_label.config(text="Логът вече е празен.", foreground="#57544C")
            return
        confirmed = messagebox.askyesno(
            "Изтриване на лога",
            "Наистина ли искаш да изтриеш ЦЕЛИЯ upload log?\n\n"
            "Това действие е НЕОБРАТИМО - цялата история на upload/retire/unretire действия "
            "ще бъде изтрита завинаги. Registry таба (текущите активни устройства) НЕ се засяга.",
            icon="warning"
        )
        if not confirmed:
            return
        clear_log(self.app.db_path)
        self.refresh()
        self.status_label.config(text="Логът е изтрит.", foreground="#16A34A")

    def _render(self):
        for row in self.tree.get_children():
            self.tree.delete(row)
        rows = self._apply_filters_and_sort()
        for r in rows:
            values = tuple(r.get(c, "") for c in self.columns)
            self.tree.insert("", "end", values=values)

    def on_tab_selected(self):
        self.refresh()


# ---------------------------------------------------------------------------
class SerialMonitorFrame(ttk.Frame):
    """Прост serial monitor - връзка, четене/писане, избор на порт и baud rate."""

    COMMON_BAUDS = ["9600", "19200", "38400", "57600", "115200", "230400", "460800", "921600"]

    def __init__(self, parent, app):
        super().__init__(parent)
        self.app = app
        self.serial_conn = None
        self.read_thread = None
        self.stop_event = threading.Event()
        self.rx_queue = queue.Queue()
        self._build()
        self._poll_queue()

    def _build(self):
        top = ttk.Frame(self)
        top.pack(fill="x", padx=5, pady=5)

        ttk.Label(top, text="COM Port:").pack(side="left")
        self.port_var = tk.StringVar()
        self.port_combo = ttk.Combobox(top, textvariable=self.port_var, width=12)
        self.port_combo.pack(side="left", padx=5)
        ttk.Button(top, text="Refresh", command=self.refresh_ports).pack(side="left", padx=5)

        ttk.Label(top, text="Baud:").pack(side="left", padx=(15, 0))
        self.baud_var = tk.StringVar(value="115200")
        self.baud_combo = ttk.Combobox(top, textvariable=self.baud_var, width=10, values=self.COMMON_BAUDS)
        self.baud_combo.pack(side="left", padx=5)

        self.connect_btn = ttk.Button(top, text="Connect", command=self.toggle_connect)
        self.connect_btn.pack(side="left", padx=(15, 5))
        ttk.Button(top, text="Clear", command=self.clear_output).pack(side="left", padx=5)

        self.status_label = ttk.Label(self, text="Изключен", foreground="#57544C")
        self.status_label.pack(anchor="w", padx=5)

        text_frame = ttk.Frame(self)
        text_frame.pack(fill="both", expand=True, padx=5, pady=5)
        scrollbar = ttk.Scrollbar(text_frame)
        scrollbar.pack(side="right", fill="y")
        self.output_text = tk.Text(text_frame, wrap="word", font=("Consolas", 9),
                                    yscrollcommand=scrollbar.set, state="disabled",
                                    background="#FFFFFF", foreground="#2B2A25",
                                    insertbackground="#2B2A25", relief="flat", borderwidth=1)
        self.output_text.pack(side="left", fill="both", expand=True)
        scrollbar.config(command=self.output_text.yview)

        bottom = ttk.Frame(self)
        bottom.pack(fill="x", padx=5, pady=5)
        self.send_var = tk.StringVar()
        send_entry = ttk.Entry(bottom, textvariable=self.send_var)
        send_entry.pack(side="left", fill="x", expand=True)
        send_entry.bind("<Return>", lambda e: self.send_line())
        ttk.Button(bottom, text="Send", command=self.send_line).pack(side="left", padx=5)

        self.refresh_ports()

    def refresh_ports(self):
        ports = sorted(list_serial_ports())
        self.port_combo["values"] = ports
        if ports and not self.port_var.get():
            self.port_var.set(ports[0])

    def toggle_connect(self):
        if self.serial_conn and self.serial_conn.is_open:
            self.disconnect()
        else:
            self.connect()

    def connect(self):
        try:
            import serial
        except ImportError:
            self.status_label.config(text="pyserial не е инсталиран (pip install pyserial)", foreground="#DC2626")
            return

        port = self.port_var.get()
        if not port:
            self.status_label.config(text="Избери COM порт", foreground="#DC2626")
            return
        try:
            baud = int(self.baud_var.get())
        except ValueError:
            self.status_label.config(text="Невалиден baud rate", foreground="#DC2626")
            return

        try:
            self.serial_conn = serial.Serial(port, baud, timeout=0.2)
        except Exception as e:
            self.status_label.config(text=f"Грешка при връзка: {e}", foreground="#DC2626")
            return

        self.stop_event.clear()
        self.read_thread = threading.Thread(target=self._read_loop, daemon=True)
        self.read_thread.start()
        self.connect_btn.config(text="Disconnect")
        self.status_label.config(text=f"Свързан на {port} @ {baud}", foreground="#16A34A")

    def disconnect(self):
        self.stop_event.set()
        if self.serial_conn:
            try:
                self.serial_conn.close()
            except Exception:
                pass
        self.serial_conn = None
        self.connect_btn.config(text="Connect")
        self.status_label.config(text="Изключен", foreground="#57544C")

    def _read_loop(self):
        while not self.stop_event.is_set():
            try:
                if self.serial_conn and self.serial_conn.in_waiting:
                    data = self.serial_conn.read(self.serial_conn.in_waiting)
                    self.rx_queue.put(data.decode(errors="ignore"))
            except Exception:
                break
            time.sleep(0.05)

    def _poll_queue(self):
        try:
            while True:
                text = self.rx_queue.get_nowait()
                self.output_text.config(state="normal")
                self.output_text.insert("end", text)
                self.output_text.see("end")
                self.output_text.config(state="disabled")
        except queue.Empty:
            pass
        self.after(100, self._poll_queue)

    def send_line(self):
        if not (self.serial_conn and self.serial_conn.is_open):
            self.status_label.config(text="Не си свързан", foreground="#DC2626")
            return
        text = self.send_var.get()
        try:
            self.serial_conn.write((text + "\n").encode())
            self.send_var.set("")
        except Exception as e:
            self.status_label.config(text=f"Грешка при изпращане: {e}", foreground="#DC2626")

    def clear_output(self):
        self.output_text.config(state="normal")
        self.output_text.delete("1.0", "end")
        self.output_text.config(state="disabled")

    def close(self):
        self.stop_event.set()
        if self.serial_conn:
            try:
                self.serial_conn.close()
            except Exception:
                pass


# ---------------------------------------------------------------------------
class SettingsFrame(ttk.Frame):
    def __init__(self, parent, app):
        super().__init__(parent)
        self.app = app
        self._build()

    def _build(self):
        ttk.Label(self, text="\u2699 Settings", font=("", 12, "bold")).pack(anchor="w", padx=5, pady=5)

        ttk.Label(self, text="Резервирани пинове (comma separated, напр. 2,9,10,11,12,13):").pack(anchor="w", padx=5)
        self.reserved_var = tk.StringVar(value=self.app.cfg["reserved_pins"].get("pins", ""))
        ttk.Entry(self, textvariable=self.reserved_var, width=40).pack(anchor="w", padx=5, pady=5)
        ttk.Label(self, text="Пинове 11, 12 и 13 са хардуерният SPI (MOSI/MISO/SCK) за комуникация с LoRa "
                              "чипа на Executor - не може да се ползват за консуматори, дори ако бъдат "
                              "премахнати от този списък.", foreground="#666666", wraplength=420,
                  justify="left").pack(anchor="w", padx=5, pady=(0, 5))

        ttk.Label(self, text="LoRa честоти (MHz, без водещи нули - напр. 433, или 433.5 за междинна):",
                  font=("", 10, "bold")).pack(anchor="w", padx=5, pady=(15, 0))

        freq_frame = ttk.Frame(self)
        freq_frame.pack(anchor="w", padx=5, pady=5)
        ttk.Label(freq_frame, text="Честота до Gateway (Sensor директно / Executor / Repeater->Gateway):").grid(
            row=0, column=0, sticky="w")
        self.freq_gateway_var = tk.StringVar(value=self.app.cfg["lora"]["freq_gateway_mhz"])
        ttk.Entry(freq_frame, textvariable=self.freq_gateway_var, width=10).grid(row=0, column=1, sticky="w", padx=5)
        self.freq_gateway_error_label = ttk.Label(freq_frame, text="", foreground="#DC2626", wraplength=350, justify="left")
        self.freq_gateway_error_label.grid(row=0, column=2, sticky="w", padx=5)

        ttk.Label(freq_frame, text="Честота Sensor -> Repeater:").grid(row=1, column=0, sticky="w", pady=(5, 0))
        self.freq_repeater_var = tk.StringVar(value=self.app.cfg["lora"]["freq_repeater_mhz"])
        ttk.Entry(freq_frame, textvariable=self.freq_repeater_var, width=10).grid(
            row=1, column=1, sticky="w", padx=5, pady=(5, 0))
        self.freq_repeater_error_label = ttk.Label(freq_frame, text="", foreground="#DC2626", wraplength=350, justify="left")
        self.freq_repeater_error_label.grid(row=1, column=2, sticky="w", padx=5, pady=(5, 0))

        ttk.Label(self, text=("EU LoRa диапазони: 433-434.79 MHz или 863-870 MHz. За междинна честота "
                               "ползвай десетична точка (напр. 433.5), не слепени цифри (напр. 4335)."),
                  foreground="#57544C", wraplength=550, justify="left").pack(anchor="w", padx=5)

        self.auto_disconnect_var = tk.BooleanVar(value=self.app.auto_disconnect_monitor)
        ttk.Checkbutton(self, text="Автоматично disconnect на Serial Monitor при смяна на таб",
                        variable=self.auto_disconnect_var,
                        command=self.toggle_auto_disconnect).pack(anchor="w", padx=5, pady=10)

        ttk.Button(self, text="Save", command=self.save).pack(anchor="w", padx=5, pady=5)
        self.status_label = ttk.Label(self, text="")
        self.status_label.pack(anchor="w", padx=5)

    def toggle_auto_disconnect(self):
        self.app.auto_disconnect_monitor = self.auto_disconnect_var.get()
        if "serial_monitor" not in self.app.cfg:
            self.app.cfg["serial_monitor"] = {}
        self.app.cfg["serial_monitor"]["auto_disconnect_on_tab_change"] = \
            "true" if self.app.auto_disconnect_monitor else "false"
        self._write_config()

    def on_tab_selected(self):
        self.status_label.config(text="")

    def save(self):
        self.freq_gateway_error_label.config(text="")
        self.freq_repeater_error_label.config(text="")

        prev_gateway = self.app.cfg["lora"]["freq_gateway_mhz"]
        prev_repeater = self.app.cfg["lora"]["freq_repeater_mhz"]
        gw_str = self.freq_gateway_var.get().strip()
        rp_str = self.freq_repeater_var.get().strip()
        freq_changed = (gw_str != prev_gateway) or (rp_str != prev_repeater)

        ok_gw, gw_mhz, msg_gw = validate_lora_frequency_mhz(gw_str)
        ok_rp, rp_mhz, msg_rp = validate_lora_frequency_mhz(rp_str)
        has_errors = False
        if not ok_gw:
            self.freq_gateway_error_label.config(text=msg_gw)
            has_errors = True
        if not ok_rp:
            self.freq_repeater_error_label.config(text=msg_rp)
            has_errors = True
        if has_errors:
            self.status_label.config(text="Има грешки в честотите - виж маркираните полета по-горе.", foreground="#DC2626")
            return

        if freq_changed:
            confirmed = messagebox.askyesno(
                "Смяна на LoRa честота",
                "Смяната на честотата засяга ЦЯЛАТА система - всички Sensor, Executor, Repeater и "
                "Gateway устройства трябва да бъдат преконфигурирани (нов config + firmware upload) "
                "със същите нови честоти, иначе няма да могат да комуникират помежду си.\n\n"
                f"Честота до Gateway: {gw_mhz:g} MHz\n"
                f"Честота Sensor -> Repeater: {rp_mhz:g} MHz\n\n"
                "Наистина ли искаш да продължиш?"
            )
            if not confirmed:
                self.freq_gateway_var.set(prev_gateway)
                self.freq_repeater_var.set(prev_repeater)
                self.app.cfg["reserved_pins"]["pins"] = self.reserved_var.get()
                self._write_config()
                self.status_label.config(text="Отказано - честотите са върнати към старите стойности.",
                                          foreground="#D97706")
                return

        self.app.cfg["reserved_pins"]["pins"] = self.reserved_var.get()
        self.app.cfg["lora"]["freq_gateway_mhz"] = f"{gw_mhz:g}"
        self.app.cfg["lora"]["freq_repeater_mhz"] = f"{rp_mhz:g}"
        self._write_config()
        self.status_label.config(text="Записано.", foreground="#16A34A")

    def _write_config(self):
        with open(self.app.config_path, "w", encoding="utf-8") as f:
            self.app.cfg.write(f)


# ---------------------------------------------------------------------------
class App:
    def __init__(self, root):
        self.root = root
        self.root.title("Firmware Uploader")

        # ---- Цветова палитра (топла, светла - вместо студено сиво навсякъде) ----
        BG = "#F6F5F1"          # фон на приложението
        SURFACE = "#FFFFFF"      # карти/полета
        ACCENT = "#0F766E"       # teal accent - бутони, фокус
        ACCENT_HOVER = "#0B5F58"
        ACCENT_TINT = "#E6F5F3"  # light accent фон
        TEXT_PRIMARY = "#2B2A25"
        TEXT_SECONDARY = "#57544C"
        BORDER = "#E4E1D8"

        self.root.configure(background=BG)

        # 'vista' темата на Windows игнорира tag background/foreground цветовете
        # на Treeview редове (напр. сивия цвят за retired устройства) - 'clam' ги спазва коректно.
        style = ttk.Style(self.root)
        try:
            style.theme_use("clam")
        except tk.TclError:
            pass  # ако 'clam' не е налична, продължи с default темата

        style.configure(".", background=BG, foreground=TEXT_PRIMARY, font=("Segoe UI", 10))
        style.configure("TFrame", background=BG)
        style.configure("TLabel", background=BG, foreground=TEXT_PRIMARY)
        style.configure("TCheckbutton", background=BG, foreground=TEXT_PRIMARY)
        style.configure("TNotebook", background=BG, borderwidth=0)
        style.configure("TNotebook.Tab", background=BG, foreground=TEXT_SECONDARY,
                         padding=[12, 6], borderwidth=0)
        style.map("TNotebook.Tab",
                  background=[("selected", SURFACE)],
                  foreground=[("selected", ACCENT)])

        style.configure("TButton", background=SURFACE, foreground=TEXT_PRIMARY,
                         borderwidth=1, focuscolor=ACCENT, padding=[10, 5])
        style.map("TButton",
                  background=[("active", ACCENT_TINT)],
                  bordercolor=[("focus", ACCENT)])

        style.configure("Accent.TButton", background=ACCENT, foreground="white",
                         borderwidth=0, padding=[14, 7], font=("Segoe UI", 10, "bold"))
        style.map("Accent.TButton",
                  background=[("active", ACCENT_HOVER), ("pressed", ACCENT_HOVER)])

        style.configure("TEntry", fieldbackground=SURFACE, foreground=TEXT_PRIMARY,
                         bordercolor=BORDER, lightcolor=BORDER, darkcolor=BORDER)
        style.map("TEntry", bordercolor=[("focus", ACCENT)])

        style.configure("TCombobox", fieldbackground=SURFACE, foreground=TEXT_PRIMARY,
                         bordercolor=BORDER)

        style.configure("Treeview", background=SURFACE, fieldbackground=SURFACE,
                         foreground=TEXT_PRIMARY, bordercolor=BORDER, borderwidth=1, rowheight=24)
        style.configure("Treeview.Heading", background=BG, foreground=TEXT_SECONDARY,
                         borderwidth=1, relief="flat")
        style.map("Treeview.Heading", background=[("active", ACCENT_TINT)])

        # 'clam' темата дефинира собствен style.map за Treeview, който override-ва
        # tag background/foreground за незиз-редовете - explicit override само за 'selected'
        # състояние оставя tag цветовете (напр. retired-сивото) видими за нормалните редове.
        style.map("Treeview",
                  background=[("selected", ACCENT)],
                  foreground=[("selected", "white")])

        self.config_path = "config.ini"
        self.cfg = load_config(self.config_path)
        self.db_path = self.cfg["paths"]["db_path"]
        os.makedirs(os.path.dirname(self.db_path) or ".", exist_ok=True)
        init_db(self.db_path)

        self.known_ports = list_serial_ports()
        self.port_var = tk.StringVar()  # СПОДЕЛЕНА между sensor/executor/repeater/gateway табовете

        if "serial_monitor" not in self.cfg:
            self.cfg["serial_monitor"] = {}
        self.auto_disconnect_monitor = self.cfg["serial_monitor"].getboolean(
            "auto_disconnect_on_tab_change", fallback=True)
        self.current_tab_widget = None

        self.notebook = ttk.Notebook(root)
        self.notebook.pack(fill="both", expand=True)

        self.sensor_frame = SimpleDeviceFrame(self.notebook, self, "sensor", is_esp32=False)
        self.executor_frame = ExecutorFrame(self.notebook, self)
        self.repeater_frame = SimpleDeviceFrame(self.notebook, self, "repeater", is_esp32=False)
        self.gateway_frame = SimpleDeviceFrame(self.notebook, self, "gateway", is_esp32=True)
        self.registry_frame = RegistryFrame(self.notebook, self)
        self.log_frame = LogFrame(self.notebook, self)
        self.serial_monitor_frame = SerialMonitorFrame(self.notebook, self)
        self.settings_frame = SettingsFrame(self.notebook, self)

        # табове с COM порт избор - за auto-refresh функцията (Serial Monitor има own избор, не се синхронизира)
        self.device_frames = [self.sensor_frame, self.executor_frame,
                               self.repeater_frame, self.gateway_frame]

        self.notebook.add(self.sensor_frame, text="Sensor")
        self.notebook.add(self.executor_frame, text="Executor")
        self.notebook.add(self.repeater_frame, text="Repeater")
        self.notebook.add(self.gateway_frame, text="Gateway")
        self.notebook.add(self.registry_frame, text="Registry")
        self.notebook.add(self.log_frame, text="Log")
        self.notebook.add(self.serial_monitor_frame, text="Serial Monitor")
        self.notebook.add(self.settings_frame, text="Settings \u2699")

        self.notebook.bind("<<NotebookTabChanged>>", self.on_tab_changed)
        self.root.protocol("WM_DELETE_WINDOW", self.on_close)

        self.poll_ports()  # auto-refresh на COM портове - винаги активно

    def on_close(self):
        self.serial_monitor_frame.close()
        self.root.destroy()

    def on_tab_changed(self, event):
        new_tab = self.notebook.nametowidget(self.notebook.select())

        # напускаме Serial Monitor -> disconnect (ако е включена настройката, default = включена)
        if self.current_tab_widget is self.serial_monitor_frame and new_tab is not self.serial_monitor_frame:
            if self.auto_disconnect_monitor:
                self.serial_monitor_frame.disconnect()

        self.current_tab_widget = new_tab
        if hasattr(new_tab, "on_tab_selected"):
            new_tab.on_tab_selected()

    def poll_ports(self):
        """Периодично следи за нови COM портове (включени устройства). Винаги активно.
        Портът е споделен (self.port_var) - не се ресетва при смяна на таб.
        Serial Monitor-ът има собствен избор и се auto-selectва само ако НЕ е свързан в момента."""
        current = list_serial_ports()
        if current != self.known_ports:
            new_ports = current - self.known_ports
            for frame in self.device_frames:
                frame.port_combo["values"] = sorted(current)
            self.serial_monitor_frame.port_combo["values"] = sorted(current)

            if new_ports:
                self.port_var.set(sorted(new_ports)[0])

                sm = self.serial_monitor_frame
                sm_connected = sm.serial_conn is not None and sm.serial_conn.is_open
                if not sm_connected:
                    sm.port_var.set(sorted(new_ports)[0])

            self.known_ports = current
        self.root.after(2000, self.poll_ports)

    def get_reserved_pins(self):
        raw = self.cfg["reserved_pins"].get("pins", "")
        return set(p.strip().upper() for p in raw.split(",") if p.strip())

    def get_lora_frequencies_hz(self):
        """Чете текущите LoRa честоти от config.ini [lora] (MHz) и ги връща в Hz.
        Извиква се наново при всяко влизане в таб, за да хване промени от Settings."""
        gw_mhz = float(self.cfg["lora"]["freq_gateway_mhz"])
        rp_mhz = float(self.cfg["lora"]["freq_repeater_mhz"])
        return int(round(gw_mhz * 1e6)), int(round(rp_mhz * 1e6))

    def refresh_registry(self):
        self.registry_frame.refresh()

    def refresh_log(self):
        self.log_frame.refresh()


if __name__ == "__main__":
    root = tk.Tk()
    app = App(root)
    root.geometry("800x680")
    root.mainloop()
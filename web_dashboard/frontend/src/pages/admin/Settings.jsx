import { useEffect, useState } from "react";
import { api } from "../../api.js";

const FIELDS = [
  {
    section: "pump_defaults",
    title: "Помпи — стойности по подразбиране за нова помпа",
    items: [
      { key: "max_simultaneous_valves", label: "Макс. едновременни клапани", suffix: "" },
      { key: "startup_time_s", label: "Време за стартиране на помпата", suffix: "сек" },
      { key: "shutdown_time_s", label: "Време за изключване на помпата", suffix: "сек" },
    ],
  },
  {
    section: "valve_defaults",
    title: "Клапани — стойности по подразбиране за нов клапан",
    items: [
      { key: "opening_time_s", label: "Време за отваряне на клапана", suffix: "сек" },
      { key: "closing_time_s", label: "Време за затваряне на клапана", suffix: "сек" },
    ],
  },
  {
    section: "threshold_defaults",
    title: "Прагови правила — стойности по подразбиране за ново правило",
    items: [
      { key: "irrigation_duration_s", label: "Продължителност на поливане", suffix: "сек" },
      { key: "infiltration_wait_s", label: "Изчакване преди прецена", suffix: "сек" },
    ],
  },
  {
    section: "sensor_readings",
    title: "Сензорни данни",
    items: [
      { key: "averaging_window_minutes", label: "Прозорец за осредняване на показанията", suffix: "мин" },
      { key: "history_limit", label: "Брой запазени точки в графиките", suffix: "" },
    ],
  },
  {
    section: "health_checks",
    title: "Диагностика на системата",
    items: [
      { key: "sensor_offline_minutes", label: "Сензор без ново показание за (SENSOR_OFFLINE)", suffix: "мин" },
      { key: "device_offline_minutes", label: "Устройство без heartbeat за (GATEWAY/REPEATER/EXECUTOR_OFFLINE)", suffix: "мин" },
      { key: "no_effect_after_minutes", label: "Клапан включен без промяна на влажността за (VALVE_NO_EFFECT)", suffix: "мин" },
      { key: "sensor_stuck_minutes", label: "Сензор с напълно непроменена стойност за (SENSOR_STUCK_VALUE)", suffix: "мин" },
      { key: "frequent_restart_count", label: "Брой рестартирания, за да се вдигне EXECUTOR_FREQUENT_RESTART", suffix: "пъти" },
      { key: "frequent_restart_window_minutes", label: "…в рамките на (EXECUTOR_FREQUENT_RESTART)", suffix: "мин" },
    ],
  },
  {
    section: "refresh_intervals",
    title: "Обновяване на интерфейса",
    items: [
      { key: "dashboard_seconds", label: "Табло (общ преглед)", suffix: "сек" },
      { key: "zone_detail_seconds", label: "Детайлен изглед на зона", suffix: "сек" },
    ],
  },
  {
    section: "battery",
    title: "Батерии",
    items: [
      { key: "default_has_battery_sensor", label: "Нов сензор има батерия по подразбиране", type: "bool" },
      { key: "default_has_battery_executor", label: "Нов модул за управление има батерия по подразбиране", type: "bool" },
      { key: "default_has_battery_repeater", label: "Нов повторител има батерия по подразбиране", type: "bool" },
      { key: "curve", label: "Тип батерия (как се смята процентът)", type: "select",
        options: [["liion_18650", "Литиево-йонна 18650 (по подразбиране)"], ["linear", "Друга — линейно между празна и пълна"]] },
      { key: "full_mv", label: "Напрежение на пълна батерия (100%) — само за линейно", suffix: "mV" },
      { key: "empty_mv", label: "Напрежение на празна батерия (0%) — само за линейно", suffix: "mV" },
      { key: "warn_percent", label: "Предупреждение „ниска батерия“ при", suffix: "%" },
      { key: "critical_percent", label: "Проблем „почти изтощена“ при", suffix: "%" },
      { key: "no_data_hours", label: "Няма данни за батерията (устройството работи, но не праща напрежение) за", suffix: "часа" },
    ],
  },
  {
    section: "security",
    title: "Сигурност",
    items: [
      { key: "session_timeout_minutes", label: "Изтичане на сесия при неактивност", suffix: "мин" },
    ],
  },
  {
    section: "data_retention",
    title: "Триене на стари данни",
    items: [
      { key: "sensor_readings_retention_days", label: "Пазене на показания на сензори за", suffix: "дни" },
    ],
  },
];

export default function Settings() {
  const [config, setConfig] = useState(null);
  const [saving, setSaving] = useState(false);
  const [saved, setSaved] = useState(false);
  const [error, setError] = useState(null);

  async function load() {
    try {
      setConfig(await api.config.get());
    } catch (err) {
      setError(err.message);
    }
  }

  useEffect(() => {
    load();
  }, []);

  function setValue(section, key, value) {
    setConfig((c) => ({ ...c, [section]: { ...c[section], [key]: value } }));
    setSaved(false);
  }

  async function save() {
    setSaving(true);
    setError(null);
    try {
      const payload = {};
      for (const { section, items } of FIELDS) {
        payload[section] = {};
        for (const { key, type } of items) {
          payload[section][key] = type === "bool" ? !!config[section][key] : type === "select" ? String(config[section][key]) : Number(config[section][key]);
        }
      }
      const updated = await api.config.update(payload);
      setConfig(updated);
      setSaved(true);
    } catch (err) {
      setError(err.message);
    } finally {
      setSaving(false);
    }
  }

  if (!config) {
    return (
      <section>
        <div className="admin-head"><div><h1>Настройки</h1></div></div>
        {error ? <div className="error-note">{error}</div> : <p className="muted">Зареждане…</p>}
      </section>
    );
  }

  return (
    <section>
      <div className="admin-head">
        <div>
          <h1>Настройки</h1>
          <p>Placeholder стойности по подразбиране за системата — виж config/defaults.json. Промените се записват веднага, без рестарт на backend-а.</p>
        </div>
        <button className="btn btn-primary" onClick={save} disabled={saving}>{saving ? "Записва се…" : "Запази промените"}</button>
      </div>

      {saved && <div className="card-note">Настройките са записани.</div>}
      {error && <div className="error-note">{error}</div>}

      {FIELDS.map(({ section, title, items }) => (
        <div className="table-card" key={section}>
          <div className="table-toolbar"><h2>{title}</h2></div>
          <div className="form-grid settings-grid">
            {items.map(({ key, label, suffix, type, options }) => (
              <div className="field" key={key}>
                {type === "select" ? (
                  <>
                    <label>{label}</label>
                    <select value={config[section][key]} onChange={(e) => setValue(section, key, e.target.value)}>
                      {options.map(([v, l]) => <option key={v} value={v}>{l}</option>)}
                    </select>
                  </>
                ) : type === "bool" ? (
                  <label className="snap-toggle">
                    <input type="checkbox" checked={!!config[section][key]} onChange={(e) => setValue(section, key, e.target.checked)} />
                    {label}
                  </label>
                ) : (
                  <>
                    <label>{label}{suffix && ` (${suffix})`}</label>
                    <input
                      type="number"
                      min="0"
                      value={config[section][key]}
                      onChange={(e) => setValue(section, key, e.target.value)}
                    />
                  </>
                )}
              </div>
            ))}
          </div>
        </div>
      ))}
    </section>
  );
}

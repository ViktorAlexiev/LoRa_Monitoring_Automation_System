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
        for (const { key } of items) {
          payload[section][key] = Number(config[section][key]);
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
            {items.map(({ key, label, suffix }) => (
              <div className="field" key={key}>
                <label>{label}{suffix && ` (${suffix})`}</label>
                <input
                  type="number"
                  min="0"
                  value={config[section][key]}
                  onChange={(e) => setValue(section, key, e.target.value)}
                />
              </div>
            ))}
          </div>
        </div>
      ))}
    </section>
  );
}

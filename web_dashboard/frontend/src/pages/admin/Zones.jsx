import { useEffect, useState } from "react";
import { api } from "../../api.js";
import Modal from "../../components/Modal.jsx";
import ConfirmDialog from "../../components/ConfirmDialog.jsx";
import ZoneSettingsModal from "../../components/ZoneSettingsModal.jsx";
import ZoneModulesModal from "../../components/ZoneModulesModal.jsx";
import { SearchBox } from "../../components/TableControls.jsx";
import { useTable } from "../../utils/useTable.js";

const MODE_LABEL = { manual: "Ръчен", clock: "По часовник", threshold: "По прагове" };
const MODE_CLASS = { manual: "pill-manual", clock: "pill-clock", threshold: "pill-threshold" };

function NewZoneModal({ onClose, onCreated }) {
  const [form, setForm] = useState({ name: "", description: "", regime: "manual", humidity_warn_min: "", humidity_warn_max: "" });
  const [error, setError] = useState(null);

  async function submit(e) {
    e.preventDefault();
    if (form.humidity_warn_min !== "" && form.humidity_warn_max !== "" && Number(form.humidity_warn_min) >= Number(form.humidity_warn_max)) {
      setError("Минималната влажност трябва да е по-малка от максималната.");
      return;
    }
    try {
      await api.zones.create({
        ...form,
        humidity_warn_min: form.humidity_warn_min === "" ? null : Number(form.humidity_warn_min),
        humidity_warn_max: form.humidity_warn_max === "" ? null : Number(form.humidity_warn_max),
      });
      onCreated();
      onClose();
    } catch (err) {
      setError(err.message);
    }
  }

  return (
    <Modal title="Нова зона" onClose={onClose} width="440px">
      <form className="form-grid" onSubmit={submit}>
        <div className="field full"><label>Заглавие</label><input required autoFocus value={form.name} onChange={(e) => setForm({ ...form, name: e.target.value })} /></div>
        <div className="field full"><label>Описание</label><input value={form.description} onChange={(e) => setForm({ ...form, description: e.target.value })} /></div>
        <div className="field full">
          <label>Режим</label>
          <select value={form.regime} onChange={(e) => setForm({ ...form, regime: e.target.value })}>
            <option value="manual">Ръчен</option>
            <option value="clock">По часовник</option>
            <option value="threshold">По прагове</option>
          </select>
        </div>
        <div className="field"><label>Мин. влажност на почвата, % (по избор)</label><input type="number" min="0" max="100" value={form.humidity_warn_min} onChange={(e) => setForm({ ...form, humidity_warn_min: e.target.value })} /></div>
        <div className="field"><label>Макс. влажност на почвата, % (по избор)</label><input type="number" min="0" max="100" value={form.humidity_warn_max} onChange={(e) => setForm({ ...form, humidity_warn_max: e.target.value })} /></div>
        {error && <div className="error-note field full">{error}</div>}
        <div className="field full">
          <button className="btn" type="button" onClick={onClose}>Отказ</button>
          <button className="btn btn-primary" type="submit">Създай</button>
        </div>
      </form>
    </Modal>
  );
}

export default function Zones() {
  const [zones, setZones] = useState([]);
  const [sensors, setSensors] = useState([]);
  const [valves, setValves] = useState([]);
  const [pumps, setPumps] = useState([]);
  const [executors, setExecutors] = useState([]);
  const [repeaters, setRepeaters] = useState([]);

  const [newZoneOpen, setNewZoneOpen] = useState(false);
  const [modulesModal, setModulesModal] = useState(null); // { zoneId, kind: 'sensor'|'valve' }
  const [settingsZone, setSettingsZone] = useState(null); // zone object
  const [confirmToggle, setConfirmToggle] = useState(null); // zone object
  const [toggleError, setToggleError] = useState(null);
  const [confirmDeleteZone, setConfirmDeleteZone] = useState(null); // zone object
  const [confirmRegime, setConfirmRegime] = useState(null); // { zone, newRegime }
  const [regimeError, setRegimeError] = useState(null);
  const [transitionErrors, setTransitionErrors] = useState({}); // zoneId -> ZoneErrorOut[]

  async function loadAll() {
    const [z, s, v, p, e, r] = await Promise.all([
      api.zones.list(),
      api.sensors.list(),
      api.valves.list(),
      api.pumps.list(),
      api.executors.list(),
      api.repeaters.list(),
    ]);
    setZones(z);
    setSensors(s);
    setValves(v);
    setPumps(p);
    setExecutors(e);
    setRepeaters(r);

    const stuck = z.filter((zone) => zone.transition_status !== "none");
    const pairs = await Promise.all(stuck.map(async (zone) => [zone.id, await api.zones.errors(zone.id)]));
    setTransitionErrors(Object.fromEntries(pairs));
  }

  async function doContinueTransition(zoneId) {
    await api.zones.continueTransition(zoneId);
    loadAll();
  }

  async function doDeactivateTransition(zoneId) {
    await api.zones.deactivateFromTransition(zoneId);
    loadAll();
  }

  useEffect(() => {
    loadAll();
  }, []);

  async function changeRegime(zoneId, regime) {
    await api.zones.update(zoneId, { regime });
    loadAll();
  }

  function requestRegimeChange(zone, newRegime) {
    if (newRegime === zone.regime) return;
    if (zone.is_active) {
      setConfirmRegime({ zone, newRegime });
      setRegimeError(null);
    } else {
      changeRegime(zone.id, newRegime);
    }
  }

  async function doConfirmRegimeChange() {
    try {
      await changeRegime(confirmRegime.zone.id, confirmRegime.newRegime);
      setConfirmRegime(null);
      setRegimeError(null);
    } catch (err) {
      setRegimeError(err.message);
    }
  }

  async function doToggleActive() {
    try {
      await api.zones.update(confirmToggle.id, { is_active: !confirmToggle.is_active });
      setConfirmToggle(null);
      setToggleError(null);
      loadAll();
    } catch (err) {
      setToggleError(err.message);
    }
  }

  async function doDeleteZone() {
    await api.zones.remove(confirmDeleteZone.id);
    setConfirmDeleteZone(null);
    loadAll();
  }

  const modulesModalZone = modulesModal ? zones.find((z) => z.id === modulesModal.zoneId) : null;
  const zoneTable = useTable(zones, { searchFields: ["name", "description"] });

  return (
    <section>
      <div className="admin-head">
        <div>
          <h1>Зони</h1>
          <p>Логическо групиране на сензори и консуматори, режим на управление.</p>
        </div>
        <div className="admin-head-actions">
          <SearchBox value={zoneTable.search} onChange={zoneTable.setSearch} options={zoneTable.searchOptions} placeholder="Търси зона по име…" />
          <button className="btn btn-primary" onClick={() => setNewZoneOpen(true)}>+ Нова зона</button>
        </div>
      </div>

      <div className="zones-admin-grid">
        {zoneTable.rows.map((z) => {
          const zoneSensors = sensors.filter((s) => s.zone_id === z.id);
          const zoneValves = valves.filter((v) => v.zone_id === z.id);
          const locked = z.is_active;
          const transitioning = z.transition_status !== "none";
          const errs = transitionErrors[z.id] || [];
          return (
            <div className="zone-admin-card" key={z.id}>
              <div>
                <h3>{z.name}</h3>
                <span className={`pill ${MODE_CLASS[z.regime]}`}>{MODE_LABEL[z.regime]}</span>
              </div>
              <div className="zone-admin-desc">{z.description}</div>

              {transitioning && (
                <div className={`transition-banner ${z.transition_status}`}>
                  {z.transition_status === "waiting" ? (
                    <div>⏳ Изчаква се изключване на всички консуматори...</div>
                  ) : (
                    <>
                      <div>✖ Не всички консуматори успяха да се изключат:</div>
                      <ul>
                        {errs.map((e) => <li key={e.id}>{e.description}</li>)}
                      </ul>
                      <div>
                        <button className="btn btn-sm btn-primary" onClick={() => doContinueTransition(z.id)}>
                          Продължи с работещите
                        </button>
                        <button className="btn btn-sm" onClick={() => doDeactivateTransition(z.id)}>
                          Деактивирай зоната
                        </button>
                      </div>
                    </>
                  )}
                </div>
              )}

              <fieldset disabled={transitioning}>
                <div>
                  <button
                    className="btn"
                    
                    onClick={() => setModulesModal({ zoneId: z.id, kind: "sensor" })}
                  >
                    Сензори ({zoneSensors.length})
                  </button>
                  <button
                    className="btn"
                    
                    onClick={() => setModulesModal({ zoneId: z.id, kind: "valve" })}
                  >
                    Клапани ({zoneValves.length})
                  </button>
                </div>

                <div className="field">
                  <label>Режим на управление</label>
                  <select value={z.regime} onChange={(e) => requestRegimeChange(z, e.target.value)}>
                    <option value="manual">Ръчен</option>
                    <option value="clock">По часовник</option>
                    <option value="threshold">По прагове</option>
                  </select>
                </div>
                <button className="btn btn-sm" onClick={() => setSettingsZone(z)}>
                  {z.regime === "manual" ? "Настройки" : `Настройки на ${MODE_LABEL[z.regime].toLowerCase()}`}
                </button>

                {locked && (
                  <div className="locked-note">
                    Зоната е активна — деактивирай я, за да добавяш/махаш сензори или клапани.
                  </div>
                )}

                <div>
                  <span className={`pill ${z.is_active ? "pill-clock" : "pill-inactive"}`}>{z.is_active ? "Активна" : "Неактивна"}</span>
                  <div>
                    <button className="btn btn-sm" onClick={() => { setConfirmToggle(z); setToggleError(null); }}>
                      {z.is_active ? "Деактивирай" : "Активирай"}
                    </button>
                    <button className="btn-link" onClick={() => setConfirmDeleteZone(z)}>Изтрий</button>
                  </div>
                </div>
              </fieldset>
            </div>
          );
        })}
      </div>

      {newZoneOpen && <NewZoneModal onClose={() => setNewZoneOpen(false)} onCreated={loadAll} />}

      {modulesModal && modulesModalZone && (
        <ZoneModulesModal
          zone={modulesModalZone}
          kind={modulesModal.kind}
          allItems={modulesModal.kind === "sensor" ? sensors : valves}
          pumps={pumps}
          executors={executors}
          repeaters={repeaters}
          locked={modulesModalZone.is_active}
          onClose={() => setModulesModal(null)}
          onSaved={loadAll}
        />
      )}

      {settingsZone && (
        <ZoneSettingsModal zone={settingsZone} valves={valves} onClose={() => setSettingsZone(null)} onSaved={loadAll} />
      )}

      {confirmToggle && (
        <ConfirmDialog
          title={confirmToggle.is_active ? "Деактивиране на зона" : "Активиране на зона"}
          message={
            confirmToggle.is_active
              ? `Ще деактивираш „${confirmToggle.name}“. Ще опитаме да изключим всички нейни клапани и помпа; ако някой модул не отговори, зоната пак ще се деактивира, но ще остане отворена грешка за него.`
              : `Ще активираш „${confirmToggle.name}“. Всички нейни клапани/помпа ще бъдат изведени в изключено състояние преди да поеме автоматичното управление.`
          }
          confirmLabel={confirmToggle.is_active ? "Деактивирай" : "Активирай"}
          error={toggleError}
          onConfirm={doToggleActive}
          onCancel={() => { setConfirmToggle(null); setToggleError(null); }}
        />
      )}

      {confirmRegime && (
        <ConfirmDialog
          title="Смяна на режим на активна зона"
          message={`„${confirmRegime.zone.name}“ е активна — всички нейни клапани и помпи ще бъдат изведени в изключено състояние, преди да премине на режим „${MODE_LABEL[confirmRegime.newRegime]}“.`}
          confirmLabel="Смени режима"
          error={regimeError}
          onConfirm={doConfirmRegimeChange}
          onCancel={() => { setConfirmRegime(null); setRegimeError(null); }}
        />
      )}

      {confirmDeleteZone && (() => {
        const affectedSensors = sensors.filter((s) => s.zone_id === confirmDeleteZone.id).length;
        const affectedValves = valves.filter((v) => v.zone_id === confirmDeleteZone.id).length;
        const note = (affectedSensors || affectedValves)
          ? ` ${affectedSensors} сензор(и) и ${affectedValves} клапан(и) ще останат без зона (не се трият), заедно с всички графици/прагове на зоната.`
          : "";
        return (
          <ConfirmDialog
            title="Изтриване на зона"
            message={`Сигурен ли си, че искаш да изтриеш „${confirmDeleteZone.name}“?${note} Това е необратимо.`}
            confirmLabel="Изтрий"
            danger
            onConfirm={doDeleteZone}
            onCancel={() => setConfirmDeleteZone(null)}
          />
        );
      })()}
    </section>
  );
}

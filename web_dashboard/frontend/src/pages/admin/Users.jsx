import { useEffect, useState } from "react";
import { api } from "../../api.js";
import Modal from "../../components/Modal.jsx";
import ConfirmDialog from "../../components/ConfirmDialog.jsx";
import { SearchBox, Th } from "../../components/TableControls.jsx";
import { useTable } from "../../utils/useTable.js";

const ROLE_LABEL = { admin: "Администратор", agronomist: "Агроном", viewer: "Наблюдател" };

function UserFormModal({ zones, editing, onClose, onSaved }) {
  const [form, setForm] = useState(
    editing
      ? {
          username: editing.username,
          password: "",
          role: editing.role,
          full_name: editing.full_name,
          email: editing.email,
          zone_ids: editing.zones.map((z) => z.zone_id),
        }
      : { username: "", password: "", role: "viewer", full_name: "", email: "", zone_ids: [] }
  );
  const [error, setError] = useState(null);

  function toggleZone(id) {
    setForm((f) => ({
      ...f,
      zone_ids: f.zone_ids.includes(id) ? f.zone_ids.filter((z) => z !== id) : [...f.zone_ids, id],
    }));
  }

  async function submit(e) {
    e.preventDefault();
    try {
      if (editing) {
        const payload = {
          role: form.role,
          full_name: form.full_name,
          email: form.email,
          zone_ids: form.zone_ids,
        };
        if (form.password) payload.password = form.password;
        await api.users.update(editing.id, payload);
      } else {
        await api.users.create(form);
      }
      onSaved();
      onClose();
    } catch (err) {
      setError(err.message);
    }
  }

  return (
    <Modal title={editing ? "Редакция на потребител" : "Нов потребител"} onClose={onClose} width="460px">
      <form className="form-grid" onSubmit={submit}>
        <div className="field">
          <label>Потребителско име</label>
          <input required autoFocus disabled={!!editing} value={form.username} onChange={(e) => setForm({ ...form, username: e.target.value })} />
        </div>
        <div className="field"><label>Име</label><input value={form.full_name} onChange={(e) => setForm({ ...form, full_name: e.target.value })} placeholder="напр. Мария Иванова" /></div>
        <div className="field">
          <label>{editing ? "Нова парола (по избор)" : "Парола"}</label>
          <input required={!editing} type="password" value={form.password} onChange={(e) => setForm({ ...form, password: e.target.value })} placeholder={editing ? "остави празно, за да не се сменя" : "••••••••"} />
        </div>
        <div className="field"><label>Имейл</label><input type="email" value={form.email} onChange={(e) => setForm({ ...form, email: e.target.value })} /></div>
        <div className="field full">
          <label>Роля</label>
          <select value={form.role} onChange={(e) => setForm({ ...form, role: e.target.value })}>
            <option value="viewer">Наблюдател</option>
            <option value="agronomist">Агроном</option>
            <option value="admin">Администратор</option>
          </select>
        </div>
        <div className="field full">
          <label>Отговаря за зони</label>
          <div className="checks">
            {zones.map((z) => (
              <label className="check-chip" key={z.id}>
                <input type="checkbox" checked={form.zone_ids.includes(z.id)} onChange={() => toggleZone(z.id)} />
                {z.name}
              </label>
            ))}
          </div>
        </div>
        {error && <div className="error-note field full">{error}</div>}
        <div className="field full">
          <button className="btn" type="button" onClick={onClose}>Отказ</button>
          <button className="btn btn-primary" type="submit">{editing ? "Запази" : "Създай потребител"}</button>
        </div>
      </form>
    </Modal>
  );
}

export default function Users() {
  const [users, setUsers] = useState([]);
  const [zones, setZones] = useState([]);
  const [formUser, setFormUser] = useState(undefined); // undefined = closed, null = add, object = edit
  const [confirmDelete, setConfirmDelete] = useState(null);

  const usersTable = useTable(users, { searchFields: ["full_name", "username", "email", "role"] });

  async function loadAll() {
    const [u, z] = await Promise.all([api.users.list(), api.zones.list()]);
    setUsers(u);
    setZones(z);
  }

  useEffect(() => {
    loadAll();
  }, []);

  async function doDelete() {
    await api.users.remove(confirmDelete.id);
    setConfirmDelete(null);
    loadAll();
  }

  return (
    <section>
      <div className="admin-head">
        <div>
          <h1>Потребители</h1>
          <p>Акаунти и достъп по зони.</p>
        </div>
        <button className="btn btn-primary" onClick={() => setFormUser(null)}>+ Нов потребител</button>
      </div>

      <div className="card-note">
        Освен изброените тук, винаги съществува и един скрит системен администратор
        (потребител „admin“) — не се показва в тая таблица и не може да се редактира/трие оттук,
        за да остане гарантиран начин за влизане в системата.
      </div>

      <div className="table-card">
        <div className="table-toolbar">
          <h2>Съществуващи</h2>
          <SearchBox value={usersTable.search} onChange={usersTable.setSearch} options={usersTable.searchOptions} placeholder="Търси по име, имейл, роля…" />
        </div>
        <div className="table-wrap">
          <table>
            <thead><tr>
              <Th label="Име" sortKey="full_name" sort={usersTable.sort} onSort={usersTable.toggleSort} />
              <Th label="Роля" sortKey="role" sort={usersTable.sort} onSort={usersTable.toggleSort} />
              <Th label="Имейл" sortKey="email" sort={usersTable.sort} onSort={usersTable.toggleSort} />
              <th>Зони</th>
              <th></th>
            </tr></thead>
            <tbody>
              {usersTable.rows.map((u) => (
                <tr key={u.id}>
                  <td>{u.full_name || u.username}</td>
                  <td><span className={`role-pill ${u.role === "admin" ? "role-admin" : ""}`}>{ROLE_LABEL[u.role]}</span></td>
                  <td className="muted">{u.email}</td>
                  <td className="muted">
                    {u.role === "admin" ? "Всички зони" : (u.zones.map((z) => z.zone_name).join(", ") || "—")}
                  </td>
                  <td>
                    <button className="btn btn-sm" onClick={() => setFormUser(u)}>Редактирай</button>
                    <button className="btn btn-sm" onClick={() => setConfirmDelete({ id: u.id, label: u.full_name || u.username })}>Изтрий</button>
                  </td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      </div>

      {formUser !== undefined && (
        <UserFormModal zones={zones} editing={formUser} onClose={() => setFormUser(undefined)} onSaved={loadAll} />
      )}

      {confirmDelete && (
        <ConfirmDialog
          title="Изтриване на потребител"
          message={`Сигурен ли си, че искаш да изтриеш „${confirmDelete.label}“?`}
          confirmLabel="Изтрий"
          danger
          onConfirm={doDelete}
          onCancel={() => setConfirmDelete(null)}
        />
      )}
    </section>
  );
}

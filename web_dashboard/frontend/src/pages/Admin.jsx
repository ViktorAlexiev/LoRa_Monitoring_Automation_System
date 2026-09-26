import { useState } from "react";
import { NavLink, Route, Routes } from "react-router-dom";
import PhysicalModules from "./admin/PhysicalModules.jsx";
import Zones from "./admin/Zones.jsx";
import Users from "./admin/Users.jsx";
import Settings from "./admin/Settings.jsx";
import History from "./admin/History.jsx";

export default function Admin() {
  // Sidebar becomes a collapsible drawer below the "sidebar-breakpoint" width
  // (styling removed) - this only controls whether it's open there; on wider
  // screens the sidebar is always visible regardless of this state.
  const [menuOpen, setMenuOpen] = useState(false);

  return (
    <div className="view admin-shell">
      <button className="admin-hamburger" onClick={() => setMenuOpen((v) => !v)} aria-label="Меню">
        <span className="hamburger-icon">&#9776;</span> Администрация
      </button>
      <aside className={`sidebar ${menuOpen ? "sidebar-open" : ""}`}>
        <div className="sidebar-label">Администрация</div>
        <NavLink to="/admin/devices" className={({ isActive }) => `nav-item ${isActive ? "active" : ""}`} onClick={() => setMenuOpen(false)}>
          <span className="nav-icon">&#9881;</span> Физически модули
        </NavLink>
        <NavLink to="/admin/zones" className={({ isActive }) => `nav-item ${isActive ? "active" : ""}`} onClick={() => setMenuOpen(false)}>
          <span className="nav-icon">&#9635;</span> Зони
        </NavLink>
        <NavLink to="/admin/users" className={({ isActive }) => `nav-item ${isActive ? "active" : ""}`} onClick={() => setMenuOpen(false)}>
          <span className="nav-icon">&#128101;</span> Потребители
        </NavLink>
        <NavLink to="/admin/history" className={({ isActive }) => `nav-item ${isActive ? "active" : ""}`} onClick={() => setMenuOpen(false)}>
          <span className="nav-icon">&#128339;</span> История
        </NavLink>
        <NavLink to="/admin/settings" className={({ isActive }) => `nav-item ${isActive ? "active" : ""}`} onClick={() => setMenuOpen(false)}>
          <span className="nav-icon">&#128295;</span> Настройки
        </NavLink>
      </aside>
      <main className="admin-main">
        <Routes>
          <Route index element={<PhysicalModules />} />
          <Route path="devices" element={<PhysicalModules />} />
          <Route path="zones" element={<Zones />} />
          <Route path="users" element={<Users />} />
          <Route path="history" element={<History />} />
          <Route path="settings" element={<Settings />} />
        </Routes>
      </main>
    </div>
  );
}

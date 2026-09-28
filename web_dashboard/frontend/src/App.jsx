import { Link, Navigate, Route, Routes, useLocation } from "react-router-dom";
import Dashboard from "./pages/Dashboard.jsx";
import Admin from "./pages/Admin.jsx";
import ZoneDetail from "./pages/ZoneDetail.jsx";
import Login from "./pages/Login.jsx";
import Diagnostics from "./pages/Diagnostics.jsx";
import { useEffect, useState } from "react";
import { api } from "./api.js";
import { useAuth } from "./AuthContext.jsx";

const ROLE_LABEL = { admin: "Администратор", agronomist: "Агроном", viewer: "Наблюдател" };

export default function App() {
  const location = useLocation();
  const inAdmin = location.pathname.startsWith("/admin");
  const { user, logout } = useAuth();
  const [openCount, setOpenCount] = useState(0);

  // number of open problems shown on the admin's "Диагностика" button
  useEffect(() => {
    if (!user || user.role !== "admin") return undefined;
    let alive = true;
    const poll = () => api.errors.open().then((list) => alive && setOpenCount(list.length)).catch(() => {});
    poll();
    const t = setInterval(poll, 30000);
    return () => { alive = false; clearInterval(t); };
  }, [user]);

  if (user === undefined) {
    return <main className="view"><p className="muted">Зареждане…</p></main>;
  }
  if (user === null) {
    return <Login />;
  }

  return (
    <div className="app">
      <header className="topbar">
        <div className="brand">
          <span className="brand-mark">A</span>
          <span>АгроМонитор</span>
          {inAdmin && <span className="crumb">/ Admin</span>}
        </div>
        <div className="topbar-right">
          <span className="muted">{user.full_name || user.username} · {ROLE_LABEL[user.role]}</span>
          {location.pathname !== "/" && <Link className="btn btn-back" to="/">&#8592; Към таблото</Link>}
          {user.role === "admin" && location.pathname !== "/diagnostics" && (
            <Link className="btn" to="/diagnostics">
              Диагностика{openCount > 0 && <span className="diag-badge">{openCount}</span>}
            </Link>
          )}
          {location.pathname === "/" && user.role === "admin" && <Link className="btn" to="/admin">Admin панел</Link>}
          <button className="btn btn-sm" onClick={logout}>Изход</button>
        </div>
      </header>

      <Routes>
        <Route path="/" element={<Dashboard />} />
        <Route path="/zones/:id" element={<ZoneDetail />} />
        <Route path="/diagnostics" element={user.role === "admin" ? <Diagnostics /> : <Navigate to="/" replace />} />
        <Route path="/admin/*" element={user.role === "admin" ? <Admin /> : <Navigate to="/" replace />} />
      </Routes>
    </div>
  );
}

import { createContext, useContext, useEffect, useState } from "react";
import { api } from "./api.js";

const AuthContext = createContext(null);

export function AuthProvider({ children }) {
  const [user, setUser] = useState(undefined); // undefined = still checking, null = logged out
  const [error, setError] = useState(null);

  async function refresh() {
    const u = await api.auth.me();
    setUser(u);
  }

  useEffect(() => {
    refresh();
  }, []);

  async function login(username, password) {
    setError(null);
    try {
      const u = await api.auth.login(username, password);
      setUser(u);
      return true;
    } catch (err) {
      setError(err.message);
      return false;
    }
  }

  async function logout() {
    await api.auth.logout();
    setUser(null);
  }

  return (
    <AuthContext.Provider value={{ user, error, login, logout, refresh }}>
      {children}
    </AuthContext.Provider>
  );
}

export function useAuth() {
  return useContext(AuthContext);
}

// A zone's access_level for the current user, or null if they have none.
// Admins implicitly have "control" on every zone (not listed in .zones).
export function zoneAccessLevel(user, zoneId) {
  if (!user) return null;
  if (user.role === "admin") return "control";
  const link = user.zones.find((z) => z.zone_id === zoneId);
  return link ? link.access_level : null;
}

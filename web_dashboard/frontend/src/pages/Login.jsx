import { useState } from "react";
import { useAuth } from "../AuthContext.jsx";

export default function Login() {
  const { login, error } = useAuth();
  const [username, setUsername] = useState("");
  const [password, setPassword] = useState("");
  const [busy, setBusy] = useState(false);

  async function submit(e) {
    e.preventDefault();
    setBusy(true);
    await login(username, password);
    setBusy(false);
  }

  return (
    <main className="view login-view">
      <form className="login-card" onSubmit={submit}>
        <div className="brand login-brand">
          <span className="brand-mark">A</span>
          <span>АгроМонитор</span>
        </div>
        <div className="field">
          <label>Потребител</label>
          <input required autoFocus value={username} onChange={(e) => setUsername(e.target.value)} />
        </div>
        <div className="field">
          <label>Парола</label>
          <input required type="password" value={password} onChange={(e) => setPassword(e.target.value)} />
        </div>
        {error && <div className="error-note">{error}</div>}
        <button className="btn btn-primary" type="submit" disabled={busy}>Вход</button>
      </form>
    </main>
  );
}

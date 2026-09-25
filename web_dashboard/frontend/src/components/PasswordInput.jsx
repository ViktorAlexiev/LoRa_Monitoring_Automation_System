import { useState } from "react";

// Password field with a "show / hide" toggle - so a typo on a phone
// keyboard can be spotted instead of guessed.
export default function PasswordInput(props) {
  const [visible, setVisible] = useState(false);
  return (
    <div className="password-field">
      <input {...props} type={visible ? "text" : "password"} />
      <button
        type="button" className="btn btn-sm password-toggle"
        onClick={() => setVisible((v) => !v)}
        aria-pressed={visible}
        aria-label={visible ? "Скрий паролата" : "Покажи паролата"}
      >
        {visible ? "Скрий" : "Покажи"}
      </button>
    </div>
  );
}

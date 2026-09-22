import Modal from "./Modal.jsx";

export default function ConfirmDialog({ title, message, confirmLabel = "Потвърди", danger, error, onConfirm, onCancel }) {
  return (
    <Modal title={title} onClose={onCancel} width="380px">
      <p className="muted">{message}</p>
      {error && <div className="error-note">{error}</div>}
      <div>
        <button className="btn" onClick={onCancel}>Отказ</button>
        <button className={`btn ${danger ? "btn-danger" : "btn-primary"}`} onClick={onConfirm}>{confirmLabel}</button>
      </div>
    </Modal>
  );
}

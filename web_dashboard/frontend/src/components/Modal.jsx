export default function Modal({ title, onClose, children, width }) {
  return (
    <div className="modal-backdrop" onClick={onClose}>
      <div
        className="modal-panel"
        onClick={(e) => e.stopPropagation()}
      >
        <div className="modal-header">
          <h2>{title}</h2>
          <button className="modal-close" onClick={onClose} aria-label="Затвори">&#10005;</button>
        </div>
        <div className="modal-body">{children}</div>
      </div>
    </div>
  );
}

import AuditTable from "../../components/AuditTable.jsx";

export default function History() {
  return (
    <section>
      <div className="admin-head">
        <div>
          <h1>История на действията</h1>
          <p>Кой, кога и какво е правил: ръчно включване и изключване, смяна на режим, промени на интервали и прагове, аварийно спиране. Виждаш всички зони; агрономите виждат само историята на своите зони в детайлите на зоната.</p>
        </div>
      </div>
      <AuditTable />
    </section>
  );
}

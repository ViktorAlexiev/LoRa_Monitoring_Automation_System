import io, sys, os, json, importlib
HERE = os.path.dirname(os.path.abspath(__file__))
DIAG = os.path.join(HERE, "..", "diag")
sys.path.insert(0, HERE); sys.path.insert(0, DIAG)
import vb
from engine import LEGEND
import importlib
LINKS = importlib.import_module("links").LINKS
titles = {
  "sensor": "Сензор: събужда се, мери два SHT31, праща 22-байтов криптиран пакет по LoRa и заспива дълбоко за ~15 минути. Една линейна последователност, без задачи.",
  "executor": "Изпълнител: приема команди по LoRa, управлява консуматори, потвърждава, праща heartbeat и състояние след рестарт.",
  "gateway": "Мост LoRa ↔ MQTT: две задачи на две ядра, две опашки, команди в полет с повторение, публикуване към сървъра.",
  "repeater": "Препраща пакети от RX честотата на TX честотата: две задачи делят едно радио през mutex.",
  "avr_sensor": "AVR сензор (ATmega328P): събужда се от watchdog, мери SHT31 и SHT21 през I2C, праща 22-байтов криптиран пакет и спи в PWR_DOWN ~15 минути.",
  "avr_repeater": "AVR repeater: приема на RX честотата, пази dedup, препраща на TX честотата с CAD и backoff; спи до пакет (PCINT) или тик на watchdog.",
  "avr_executor": "AVR executor: приема команди и state request, включва консуматори, праща ACK, heartbeat и state response след рестарт.",
}
out = sys.argv[1]
D, missing = vb.build(LINKS)
secs, nav = [], []
for n in ["sensor", "executor", "gateway", "repeater", "avr_sensor", "avr_executor", "avr_repeater"]:
    d = importlib.import_module(n).build()
    svg, H = d.svg()
    secs.append("<section class='dev' id='%s'><h2>%s</h2><p>%s</p><div class='scroll'>%s</div></section>" % (n, n, titles[n], svg))
    nav.append("<a href='#%s'>%s</a>" % (n, n))
tpl = io.open(os.path.join(HERE, "tpl.html"), encoding="utf-8").read()
data = json.dumps(D, ensure_ascii=False, separators=(",", ":")).replace("</", "<\\/")
html = (tpl.replace("/*NAV*/", "".join(nav)).replace("/*LEGEND*/", LEGEND)
        .replace("/*DIAGRAMS*/", "\n".join(secs)).replace("/*DATA*/", data))
io.open(out, "w", encoding="utf-8").write(html)
print("ok", out, len(html)//1024, "KB; без обяснение:", len(missing))

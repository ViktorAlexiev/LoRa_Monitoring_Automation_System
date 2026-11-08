"""Строи данните за code viewer: файлове, анотации, индекс на дефинициите, връзки схема <-> код."""
import io, os, re, json, sys

SRC = r"D:\monitoring systen\firmware_uploader\firmware_src\ESP32_PCB(temp_hum)_ESPIDF"
AVR_SRC = r"D:\monitoring systen\firmware_uploader\firmware_src\LoRa_radio_node+TTGO"
ROOTS = {"avr/": AVR_SRC}      # префикс на пътя във viewer-а -> истинска папка (AVR прошивките)
HERE = os.path.dirname(os.path.abspath(__file__))
ANN = os.path.join(HERE, "ann")

GROUP_ORDER = ["components/agro_board", "components/agro_hal", "components/agro_core", "components/agro_radio",
               "components/agro_cfg", "components/agro_net", "apps/sensor", "apps/repeater", "apps/executor",
               "apps/gateway", "apps/executor_wifi", "apps/config",
               "avr/sensor", "avr/repeater", "avr/executor", "avr/config_avr"]
EXTS = (".cpp", ".h", ".c", ".ino", ".txt", ".defaults", ".yml", ".cmake")
CODE_EXT = (".cpp", ".h", ".c", ".ino")


def real_path(rel):
    """Път във viewer-а (напр. avr/sensor/cad.cpp) -> път на диска."""
    for pre, root in ROOTS.items():
        if rel.startswith(pre):
            return os.path.join(root, *rel[len(pre):].split("/"))
    return os.path.join(SRC, *rel.split("/"))

KEYWORDS = set("""if else for while do switch case default break continue return goto sizeof static const volatile extern inline
struct class enum union typedef namespace using template typename public private protected new delete true false nullptr void bool
char short int long float double unsigned signed auto constexpr static_assert operator this noexcept override final explicit
virtual friend mutable register alignas alignof decltype try catch throw""".split())


def list_files():
    out = []
    for g in GROUP_ORDER:
        base = real_path(g)
        pre = next((p for p in ROOTS if g.startswith(p)), "")
        found = []
        for dp, dn, fn in os.walk(base):
            dn[:] = [d for d in dn if d not in ("build", "managed_components")]
            for f in fn:
                if f.endswith(EXTS) and f != "dependencies.lock":
                    rel = pre + os.path.relpath(os.path.join(dp, f), ROOTS[pre] if pre else SRC).replace("\\", "/")
                    found.append(rel)

        def key(r):
            n = r.split("/")[-1]
            e = 0 if n == "CMakeLists.txt" else 1 if n.endswith(".yml") else 2 if n.endswith(".defaults") else 3 if "/include/" in r or n.endswith(".h") else 4
            return (e, r)
        found.sort(key=key)
        out.extend((g, r) for r in found)
    return out


def read_lines(rel):
    with io.open(real_path(rel), encoding="utf-8", newline="") as f:
        t = f.read()
    return t.replace("\r\n", "\n").replace("\r", "\n").split("\n")[:-1] if t.endswith("\n") else t.replace("\r\n", "\n").split("\n")


# --------------------------------------------------------------------------- сканиране на C/C++
IDENT = r"[A-Za-z_~][A-Za-z0-9_]*"


def scan(rel, lines):
    funcs, defs, fields = [], [], []
    stack = []            # (type, name, extra)
    stmt, stmt_line = "", None
    inblk = False
    pending = None        # (kind, line) за typedef struct {...} Name;
    cont = False

    def top():
        return stack[-1][0] if stack else "top"

    def owner():
        for t, n, _ in reversed(stack):
            if t == "class":
                return n
        return None

    def add(kind, name, line, extra=None):
        if name and name not in KEYWORDS:
            defs.append(dict(kind=kind, name=name, line=line, **(extra or {})))

    def finish_stmt(s, line0, ln):
        s = re.sub(r"__attribute__\s*\(\(.*?\)\)", "", s).strip()
        if not s:
            return
        t = top()
        if t in ("top", "ns"):
            if s.startswith(("static_assert", "friend", "return")):
                return
            if s.startswith("typedef"):
                m = re.search(r"\(\s*\*\s*(\w+)\s*\)", s) or re.search(r"(\w+)\s*(\[[^\]]*\])?\s*$", s)
                if m:
                    add("тип (typedef)", m.group(1), line0)
                return
            m = re.match(r"using\s+(\w+)\s*=", s)
            if m:
                add("тип (using)", m.group(1), line0)
                return
            if "(" in s and not re.search(r"=[^=()]*\(", s.split("(")[0] + "(") and "=" not in s.split("(")[0]:
                m = re.match(r"(?:.*?\s|\*|&)?([\w:~]+)\s*\(", s)
                if m:
                    add("функция (декларация)", m.group(1), line0, dict(decl=True))
                return
            m = re.match(r"(.*?)(?:=|\[|$)", s, re.S)
            head = m.group(1) if m else s
            ids = re.findall(r"[A-Za-z_]\w*", head)
            if ids:
                add("глобална променлива", ids[-1], line0)
        elif t == "class":
            if s.startswith(("friend", "using", "static_assert", "public", "private", "protected")):
                return
            if "(" in s:
                m = re.match(r"(?:.*?\s|\*|&)?([\w:~]+)\s*\(", s)
                if m:
                    fields.append(dict(name=m.group(1), owner=owner(), line=line0, kind="метод"))
                return
            head = re.split(r"[=\[{]", s)[0]
            for part in head.split(","):
                ids = re.findall(r"[A-Za-z_]\w*", part)
                if ids:
                    fields.append(dict(name=ids[-1], owner=owner(), line=line0, kind="поле"))

    for idx, raw in enumerate(lines):
        ln = idx + 1
        line = raw
        sline = line.strip()
        if cont:
            cont = line.rstrip().endswith("\\")
            continue
        if not inblk and sline.startswith("#"):
            m = re.match(r"#\s*define\s+(\w+)", sline)
            if m:
                add("макрос", m.group(1), ln)
            cont = line.rstrip().endswith("\\")
            continue
        i, n = 0, len(line)
        while i < n:
            c = line[i]
            if inblk:
                j = line.find("*/", i)
                if j < 0:
                    i = n
                else:
                    inblk = False
                    i = j + 2
                continue
            if c == "/" and i + 1 < n and line[i + 1] == "*":
                inblk = True
                i += 2
                continue
            if c == "/" and i + 1 < n and line[i + 1] == "/":
                break
            if c in "\"'":
                q = c
                j = i + 1
                while j < n and line[j] != q:
                    j += 2 if line[j] == "\\" else 1
                if stmt_line is None:
                    stmt_line = ln
                stmt += q + q
                i = j + 1
                continue
            if c == ";":
                if pending and stmt.strip():
                    for nm in re.findall(r"[A-Za-z_]\w*", stmt):
                        add("тип (typedef struct)", nm, pending[1])
                    pending = None
                else:
                    finish_stmt(stmt, stmt_line or ln, ln)
                stmt, stmt_line = "", None
            elif c == "{":
                s = re.sub(r"__attribute__\s*\(\(.*?\)\)", "", stmt).strip()
                s = re.sub(r"^(public|private|protected)\s*:\s*", "", s)
                s = re.sub(r"^template\s*<[^{}]*?>\s*", "", s)
                t = top()
                head = re.split(r"\)\s*:\s*(?!:)", s, maxsplit=1)
                sig = (head[0] + ")") if len(head) > 1 else s
                if re.match(r'^namespace', s) or (s.startswith('extern') and '(' not in s):
                    stack.append(("ns", None, None))
                elif re.search(r"\benum\b", s) and "(" not in s:
                    m = re.search(r"\benum\s+(?:class\s+)?(\w+)", s)
                    nm = m.group(1) if m else None
                    if nm:
                        add("enum (тип)", nm, stmt_line or ln)
                    stack.append(("enum", nm, stmt_line or ln))
                elif re.match(r"^(typedef\s+)?(struct|class|union)\b", s) and "(" not in s:
                    m = re.match(r"^(?:typedef\s+)?(?:struct|class|union)\s+(\w+)", s)
                    nm = m.group(1) if m else None
                    kind = re.match(r"^(?:typedef\s+)?(struct|class|union)", s).group(1)
                    if nm:
                        add(kind, nm, stmt_line or ln)
                    stack.append(("class", nm, (stmt_line or ln, s.startswith("typedef") or nm is None)))
                elif t in ("top", "ns", "class") and re.search(r"\)\s*(const|noexcept|override|final|\s)*$", sig) and "(" in sig and not re.match(r"^(if|for|while|switch|else|do|catch)\b", sig):
                    m = re.match(r"(?:.*?[\s\*&>])?([\w:~]+)\s*\(", sig)
                    nm = m.group(1) if m else "?"
                    stack.append(("func", nm, (stmt_line or ln, owner())))
                else:
                    stack.append(("other", None, (stmt, stmt_line or ln)))
                    if t in ("top", "ns", "class"):
                        i += 1
                        continue
                stmt, stmt_line = "", None
            elif c == "}":
                if stack:
                    t, nm, ex = stack.pop()
                    if t == "func":
                        funcs.append(dict(name=nm, start=ex[0], end=ln, owner=ex[1]))
                        short = nm.split("::")[-1]
                        add("функция", nm, ex[0], dict(end=ln))
                        if ex[1] or "::" in nm:
                            own = ex[1] or nm.split("::")[0]
                            fields.append(dict(name=short, owner=own, line=ex[0], kind="метод", end=ln))
                    elif t == "enum":
                        if stmt.strip():
                            m = re.match(r"\s*(\w+)", stmt)
                            if m:
                                add("enum константа", m.group(1), stmt_line or ln)
                    elif t == "other" and ex and top() in ("top", "ns", "class"):
                        stmt, stmt_line = ex[0] + " {} ", ex[1]
                        i += 1
                        continue
                    elif t == "class":
                        if ex[1] and nm is None or ex[1]:
                            pending = ("class", ex[0])
                            if nm is not None:
                                pending = None
                stmt, stmt_line = "", None
            elif c == "," and top() == "enum":
                m = re.match(r"\s*(\w+)", stmt)
                if m:
                    add("enum константа", m.group(1), stmt_line or ln)
                stmt, stmt_line = "", None
            else:
                if not c.isspace() and stmt_line is None:
                    stmt_line = ln
                stmt += c
            i += 1
        stmt += " "
    # prune: функциите, които се виждат като вложени
    return funcs, defs, fields


# --------------------------------------------------------------------------- анотации
def load_ann(rel):
    p = os.path.join(ANN, rel.replace("/", "__") + ".ann")
    a, cont = {}, {}
    pp = os.path.join(HERE, "patches.txt")
    patches = {}
    if os.path.exists(pp):
        for raw in io.open(pp, encoding="utf-8"):
            raw = raw.rstrip("\n")
            parts = raw.split("|", 2)
            if len(parts) == 3 and parts[0].strip() == rel:
                patches[int(parts[1])] = parts[2].strip()
    if not os.path.exists(p):
        a.update(patches)
        return a, cont
    for raw in io.open(p, encoding="utf-8"):
        raw = raw.rstrip("\n")
        if not raw.strip() or raw.startswith("#"):
            continue
        m = re.match(r"^(\d+)(?:\.\.(\d+))?:\s?(.*)$", raw)
        if not m:
            print("ГРЕШЕН РЕД В", rel, ":", raw[:80])
            continue
        n = int(m.group(1))
        a[n] = m.group(3)
        if m.group(2):
            for k in range(n + 1, int(m.group(2)) + 1):
                cont[k] = n
    a.update(patches)
    return a, cont


CAT_ORDER = ["Компилация и препроцесор", "Типове и данни в C/C++", "Указатели и управление на паметта",
             "Битове и числа", "Памет на микроконтролера", "Процесор и тактове", "RTOS и многозадачност",
             "Прекъсвания и време", "Хардуерни интерфейси и GPIO", "Аналогов вход и сензори", "Захранване и сън",
             "Радио и LoRa", "Протоколи и данни", "Сигурност и цялост",
             "Система, зареждане и отстраняване на грешки", "Проектиране на embedded код", "AVR и Arduino",
             "Външни: стандартни типове и libc", "Външни: FreeRTOS", "Външни: ESP-IDF", "Външни: Wi-Fi, MQTT, JSON, mbedTLS",
             "Външни: RadioLib и SX1262", "Външни: Arduino и AVR"]
_PC = {
 "Компилация и препроцесор": "header-guard pragma include define-macro ifdef declaration static_assert inline extern extern-c noreturn",
 "Типове и данни в C/C++": "static const volatile naming uint struct class enum typedef sizeof packed magic kw",
 "Указатели и управление на паметта": "pointer reference cast nullptr func-pointer raii delete-copy chaining template lambda global-ctor ternary",
 "Битове и числа": "bit-ops endian twos-complement wraparound",
 "Памет на микроконтролера": "iram rtc-data nvs",
 "RTOS и многозадачност": "freertos-task tick queue notify mutex atomic watchdog",
 "Прекъсвания и време": "isr dio1",
 "Хардуерни интерфейси и GPIO": "spi i2c",
 "Захранване и сън": "lightsleep deepsleep",
 "Радио и LoRa": "sf bw lora-timing cad backoff jitter radiolib",
 "Протоколи и данни": "wire json mqtt wifi",
 "Сигурност и цялост": "crc crypto nonce ctr cmac constant-time ceiling dedup",
 "Система, зареждане и отстраняване на грешки": "component sdkconfig esp_log esp_err",
 "Проектиране на embedded код": "fsm executor gateway repeater",
}
PRINC_CAT = {k: c for c, ks in _PC.items() for k in ks.split()}


def ext_cat(k):
    import re
    if re.match(r"^(u?int\d+_t|size_t|NULL|mem\w+|str\w+|snprintf|printf|fflush|getchar|stdout)$", k):
        return "Външни: стандартни типове и libc"
    if re.match(r"^(x(Task|Queue|Semaphore|Event|Timer|Port)|v(Task|Queue|Semaphore)|ul(Task)|ux|pc(Task)|pd|port|task|tsk|config|err[A-Z]|eTask|TickType_t|UBaseType_t|BaseType_t|QueueHandle_t|SemaphoreHandle_t|TaskHandle_t|EventGroupHandle_t|TimerHandle_t)", k):
        return "Външни: FreeRTOS"
    if re.match(r"^(esp_get_(minimum_)?free_heap)", k):
        return "Външни: ESP-IDF"
    if re.match(r"^(wifi_|WIFI_|IP_EVENT|esp_wifi|esp_netif|esp_event|ESP_EVENT|esp_mqtt|MQTT_|mbedtls|MBEDTLS|cJSON|valuestring)", k):
        return "Външни: Wi-Fi, MQTT, JSON, mbedTLS"
    if re.match(r"^(RADIOLIB|SX1262|Module|EspHal|ConfigLoRa|ChannelScan|startReceive|readData|startTransmit|finishTransmit|standby|sleep|setFrequency|getIrqFlags|clearIrqFlags|getPacketLength|getRSSI|getSNR|getTimeOnAir|startChannelScan|getChannelScanResult|setCurrentLimit|setDio2AsRfSwitch|setRfSwitchPins|tcxoVoltage|useRegulatorLDO|resetOnStartup|spiConfig)", k):
        return "Външни: RadioLib и SX1262"
    return "Външни: ESP-IDF"


def load_sections(fn):
    p = os.path.join(HERE, fn)
    out = []
    if not os.path.exists(p):
        return out
    cur = None
    for raw in io.open(p, encoding="utf-8"):
        raw = raw.rstrip("\n")
        if raw.startswith("## "):
            parts = [x.strip() for x in raw[3:].split("|")]
            cur = dict(key=parts[0], title=parts[1] if len(parts) > 1 else parts[0], text=[])
            if len(parts) > 2:
                cur["cat"] = parts[2]
            out.append(cur)
        elif cur is not None:
            cur["text"].append(raw)
    for c in out:
        c["text"] = "\n".join(c["text"]).strip()
    return out



def brace_match(lines):
    """close_line -> (open_line, текст на реда на отварянето)"""
    st, res, inblk = [], {}, False
    for idx, l in enumerate(lines):
        ln = idx + 1
        s = l.strip()
        if not inblk and s.startswith("#"):
            continue
        i, n = 0, len(l)
        while i < n:
            c = l[i]
            if inblk:
                j = l.find("*/", i)
                if j < 0: i = n
                else: inblk = False; i = j + 2
                continue
            if c == "/" and i + 1 < n and l[i+1] == "*": inblk = True; i += 2; continue
            if c == "/" and i + 1 < n and l[i+1] == "/": break
            if c in "\"'":
                q = c; j = i + 1
                while j < n and l[j] != q: j += 2 if l[j] == "\\" else 1
                i = j + 1; continue
            if c == "{": st.append(ln)
            elif c == "}":
                if st:
                    o = st.pop()
                    res.setdefault(ln, o)
            i += 1
    return res


def is_weak(a):
    t = re.sub(r"`[^`]*`", "", a)
    t = re.sub(r"\[\[[^\]]*\]\]", "", t)
    t = re.sub(r"[.\s,;:()\-]+", " ", t).strip()
    return len(t) < 6



LOGLV = {"E": "грешка", "W": "предупреждение", "I": "информация", "D": "отстраняване", "V": "подробен"}


def rule_hint(s, funcs, ln):
    m = re.match(r'^ESP_LOG([EWIDV])\(\s*\w+\s*,\s*"((?:\\.|[^"\\])*)"', s)
    if m:
        return "записва лог на ниво %s: „%s“." % (LOGLV[m.group(1)], m.group(2).replace("\\n", "")[:90])
    m = re.match(r"^(\S+?)\.store\((.+)\);$", s)
    if m:
        return "записва атомарно `%s` в `%s` (безопасно между задачи)." % (m.group(2), m.group(1))
    if s.startswith("} else {"):
        return "в противен случай (когато условието по-горе не е вярно)."
    m = re.match(r"^case\s+(.+):", s)
    if m:
        return "блок за случая `%s`." % m.group(1)
    m = re.match(r"^switch\s*\((.+)\)\s*\{$", s)
    if m:
        return "избира блок според стойността на `%s`." % m.group(1)
    m = re.match(r"^(struct|class)\s+(\w+)\s*\{", s)
    if m:
        return "началото на %s `%s`." % ("структурата" if m.group(1) == "struct" else "класа", m.group(2))
    m = re.match(r"^(?:static\s+|inline\s+|extern\s+)*(?:const\s+)?[A-Za-z_][\w:<>]*[\s\*&]+(\w+)\s*\([^;{]*\)\s*(?:const)?\s*;", s)
    if m and "=" not in s.split("(")[0]:
        return "декларация на функцията `%s`: тялото ѝ е в .cpp файла." % m.group(1)
    m = re.match(r"^(?:static\s+)?(?:const\s+)?(?:unsigned\s+)?[A-Za-z_][\w:<>]*[\s\*&]+(\w+)(?:\[[^\]]*\])*\s*=\s*(.+);$", s)
    if m and not re.match(r"^(return|else|delete)\b", s):
        return "декларира `%s` и я инициализира с `%s`." % (m.group(1), m.group(2)[:60])
    m = re.match(r"^(?:static\s+)?(?:const\s+)?(?:unsigned\s+)?[A-Za-z_][\w:<>]*[\s\*&]+(\w+)(?:\[[^\]]*\])*\s*;$", s)
    if m and not re.match(r"^(return|break|continue)\b", s):
        return "декларира `%s` (още без стойност; ще се попълни по-късно)." % m.group(1)
    m = re.match(r"^([\w\.\->\[\]\*\(\)]+?)\s*(\+|-|\|)?=\s*(.+);$", s)
    if m and not s.startswith("return"):
        op = {"+": "добавя към", "-": "изважда от", "|": "добавя (побитово ИЛИ) към"}.get(m.group(2))
        if op:
            return "%s `%s` стойността `%s`." % (op, m.group(1), m.group(3)[:60])
        return "присвоява на `%s` стойността `%s`." % (m.group(1), m.group(3)[:60])
    m = re.match(r"^(\S+?)(\+\+|--);$", s)
    if m:
        return "%s `%s` с 1." % ("увеличава" if m.group(2) == "++" else "намалява", m.group(1))
    if s.startswith("for (;;)"):
        return "безкраен цикъл: изпълнява се, докато работи системата; излиза се само с `return` или `break`."
    m = re.match(r"^project\((\w+)\)$", s)
    if m:
        return "задава името на проекта: `%s` (име на бинарния файл)." % m.group(1)
    if s.startswith("cmake_minimum_required"):
        return "изисква CMake версия 3.16 или по-нова."
    m = re.match(r"^([A-Za-z_][\w:\.\->]*)\((.*)\);$", s)
    if m:
        return "извиква `%s`; кликни върху името за описание." % m.group(1)
    return None

def enrich(line, a, funcs, ln):
    """Допълва твърде кратки обяснения с типово пояснение по вида на реда."""
    if not is_weak(a):
        return a
    s = line.strip()
    hint = None
    m = re.match(r"^return\s*;$", s)
    if m:
        hint = "излиза от функцията без да връща стойност."
    m = re.match(r"^return\s+(.+);$", s)
    if m:
        hint = "излиза от функцията и връща към извикващия `%s`." % m.group(1)[:50]
    if s == "break;":
        hint = "излиза от най-вътрешния `switch` или цикъл."
    if s == "continue;":
        hint = "пропуска остатъка от тази итерация и минава към следващата."
    if s == "public:":
        hint = "всичко по-долу в класа е достъпно отвън (публичен интерфейс)."
    if s == "private:":
        hint = "всичко по-долу в класа е скрито: достъпно само за методите на класа."
    if s == "#endif":
        hint = "краят на условния блок (`#ifndef` или `#ifdef`), отворен по-горе."
    for f in funcs:
        if f["start"] == ln and not hint:
            hint = "дефиниция на функцията `%s`: тялото ѝ е на редове %d-%d." % (f["name"], f["start"], f["end"])
    if not hint:
        hint = rule_hint(s, funcs, ln)
    if hint:
        return (a + " " + hint) if a else hint
    return a


def classify_auto(line, state, lines=None, ln=0, bm=None):
    """Автоматично обяснение за тривиални редове. state: dict(inblk)"""
    s = line.strip()
    if state["inblk"]:
        if "*/" in s:
            state["inblk"] = False
        return "коментар на автора (компилаторът го пропуска)"
    if not s:
        return ""
    if s.startswith("/*"):
        if "*/" not in s:
            state["inblk"] = True
        return "коментар на автора (компилаторът го пропуска)"
    if s.startswith("//"):
        return "коментар на автора (компилаторът го пропуска)"
    if re.fullmatch(r"[{}]+;?", s):
        if s.startswith("}"):
            o = bm.get(ln) if bm else None
            if o:
                head = lines[o-1].strip()
                if head == "{" and o > 1:
                    head = lines[o-2].strip() + " {"
                if len(head) > 60: head = head[:57] + "..."
                return "затваря блока, отворен на ред %d: `%s`" % (o, head)
            return "затваря блока `}`"
        return "отваря блок `{`"
    return None


def build(links_spec, files=None):
    flist = list_files()
    D = dict(files=[], defs={}, fields={}, ext=[], princ=[], links={}, rev={})
    missing = []
    allf = {}
    for gi, (g, rel) in enumerate(flist):
        lines = read_lines(rel)
        code = rel.endswith(CODE_EXT)
        funcs, defs, fields = scan(rel, lines) if code else ([], [], [])
        ann, cont = load_ann(rel)
        st = dict(inblk=False)
        bm = brace_match(lines) if code else None
        ex = []
        for i, l in enumerate(lines):
            n = i + 1
            a = classify_auto(l, st, lines, n, bm) if code else None
            if not code:
                s = l.strip()
                if not s:
                    a = ""
                elif s.startswith("#"):
                    a = "коментар на автора"
            if n in ann:
                ex.append(enrich(l, ann[n], funcs, n))
            elif n in cont:
                if a is not None and a != "":
                    ex.append(a)
                elif l.strip() == "" or l.strip().startswith("//"):
                    ex.append(a or "")
                else:
                    ex.append("↳ продължава от ред %d" % cont[n])
            elif a is not None:
                ex.append(a)
            else:
                ex.append("")
                missing.append((rel, n, l.strip()[:90]))
        D["files"].append(dict(path=rel, group=g, lines=lines, ann=ex, funcs=funcs, code=code))
        allf[rel] = dict(idx=gi, funcs=funcs, lines=lines)
        for d in defs:
            e = dict(kind=d["kind"], file=gi, line=d["line"], end=d.get("end"))
            old = D["defs"].get(d["name"])
            if old is None:
                D["defs"][d["name"]] = e
            else:
                # предпочита дефиницията пред декларацията; запазва декларацията като втора позиция
                if d.get("decl"):
                    old.setdefault("decl", [gi, d["line"]])
                elif old["kind"].endswith("(декларация)"):
                    e["decl"] = [old["file"], old["line"]]
                    D["defs"][d["name"]] = e
        for f in fields:
            D["fields"].setdefault(f["name"], []).append(dict(owner=f["owner"], file=gi, line=f["line"], kind=f["kind"], end=f.get("end")))
    # методи "Клас::метод" -> кратко име
    for name in list(D["defs"]):
        if "::" in name:
            short = name.split("::")[-1]
            D["defs"].setdefault(short, D["defs"][name])
    D["princ"] = load_sections("principles.txt")
    D["ext"] = []
    for fn in sorted(os.listdir(HERE)):
        if fn.startswith("external") and fn.endswith(".txt"):
            D["ext"] += load_sections(fn)
    D["catorder"] = CAT_ORDER
    D["conc"] = []
    for fn in sorted(os.listdir(HERE)):
        if fn.startswith("concepts_") and fn.endswith(".txt"):
            D["conc"] += load_sections(fn)
    for e in D["princ"]:
        e["cat"] = PRINC_CAT.get(e["key"], "Проектиране на embedded код")
    for e in D["ext"]:
        e["cat"] = e.get("cat") or ext_cat(e["key"])
    # --- автоматични връзки от кода към понятията ---
    import autolinks
    rules = [(re.compile(rx), ks) for rx, ks in autolinks.RULES]
    titles = {}
    for e in D["princ"] + D["conc"] + D["ext"]:
        titles.setdefault(e["key"], e["title"])
    def label(k):
        t = titles[k].split(" (")[0].split(":")[0].split("(")[0].strip()
        return t if len(t) <= 38 else t[:36].rsplit(" ", 1)[0] + "…"
    for k in {k for _, ks in autolinks.RULES for k in ks}:
        assert k in titles, "няма понятие за автоматична връзка: " + k
    nlinks = 0
    for f in D["files"]:
        if not f["code"]:
            continue
        used = {}
        for i, src in enumerate(f["lines"]):
            ex = f["ann"][i]
            s = src.strip()
            if not ex or ex.startswith("↳") or s.startswith(("//", "/*", "*", "#")):
                continue
            code = s.split("//")[0]
            got = []
            for rx, ks in rules:
                if rx.search(code):
                    for k in ks:
                        if k not in got and ("[[" + k) not in ex and used.get(k, 0) < autolinks.MAX_PER_FILE:
                            got.append(k)
            got = got[:autolinks.MAX_PER_LINE]
            if got:
                for k in got:
                    used[k] = used.get(k, 0) + 1
                f["ann"][i] = ex.rstrip() + " Виж: " + ", ".join("[[%s|%s]]" % (k, label(k)) for k in got) + "."
                nlinks += len(got)
    print("автоматични връзки:", nlinks)
    # връзки
    for line in links_spec.strip().splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split(None, 2)
        diag, box, targets = parts[0], parts[1], parts[2]
        res = []
        for t in targets.split(";"):
            t = t.strip()
            if ":" in t:
                rel, what = t.split(":", 1)
            else:
                rel, what = t, ""
            if rel not in allf:
                print("ЛИПСВА ФАЙЛ във връзка:", diag, box, rel)
                continue
            fi = allf[rel]
            if not what:
                res.append([fi["idx"], 1, len(fi["lines"]), rel.split("/")[-1]])
            elif what.startswith("L"):
                a, b = what[1:].split("-")
                res.append([fi["idx"], int(a), int(b), "%s:%s-%s" % (rel.split("/")[-1], a, b)])
            else:
                for fn in what.split(","):
                    cands = [f for f in fi["funcs"] if f["name"] == fn or f["name"].endswith("::" + fn)]
                    if not cands:
                        print("ЛИПСВА ФУНКЦИЯ във връзка:", diag, box, rel, fn)
                        continue
                    for f in cands:
                        res.append([fi["idx"], f["start"], f["end"], "%s: %s()" % (rel.split("/")[-1], fn)])
        D["links"]["%s/%s" % (diag, box)] = res
        for r in res:
            D["rev"].setdefault(str(r[0]), []).append([r[1], r[2], diag, box])
    return D, missing


if __name__ == "__main__":
    sys.path.insert(0, HERE)
    from links import LINKS
    D, missing = build(LINKS)
    print("файлове:", len(D["files"]), " дефиниции:", len(D["defs"]), " полета/методи:", len(D["fields"]))
    print("редове без обяснение:", len(missing))
    if len(sys.argv) > 1 and sys.argv[1] == "missing":
        for m in missing[:int(sys.argv[2]) if len(sys.argv) > 2 else 60]:
            print("%s:%d  %s" % m)

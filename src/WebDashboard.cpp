#include "WebDashboard.h"
#include "Settings.h"
#include "config.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ElegantOTA.h>
#include <cstring>

namespace {

WebServer server(80);

PubSubClient* localMqttClient = nullptr;
PubSubClient* mainMqttClient = nullptr;

CachedState* cachedStates = nullptr;
size_t cachedStateCapacity = 0;
bool isApMode = false;
bool serverStarted = false;

const char DASHBOARD_HTML[] PROGMEM = R"HTML(
<!doctype html>
<html lang="ru">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Cottage Gateway</title>
<style>
:root{color-scheme:dark;--bg:#101116;--panel:#181a22;--card:#111319;--border:#30333d;--text:#e2e4eb;--muted:#9297a5;--green:#78d594;--red:#f27d83}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--text);font:14px Arial,sans-serif}
header{padding:14px 20px;background:#0d0e12;border-bottom:1px solid var(--border);display:flex;justify-content:space-between}
main{max-width:1100px;margin:20px auto;padding:0 14px}
nav{display:flex;gap:5px;flex-wrap:wrap;border-bottom:1px solid var(--border);margin-bottom:14px}
nav button,.action{background:#2a6140;color:white;border:0;border-radius:5px;padding:10px 14px;cursor:pointer}
nav button{background:transparent;color:var(--muted);border-radius:0}
nav button.active{color:var(--text);border-bottom:2px solid var(--green)}
.panel{background:var(--panel);border:1px solid var(--border);border-radius:8px;padding:16px;margin:12px 0}
.panel h2{font-size:13px;text-transform:uppercase;color:#a9adb9;margin:0 0 14px}
.tab{display:none}.tab.active{display:block}
.grid{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:10px}
@media(max-width:750px){.grid{grid-template-columns:repeat(2,minmax(0,1fr))}}
.card{background:var(--card);border:1px solid var(--border);border-radius:6px;padding:13px;min-width:0}
.label{font-size:11px;color:var(--muted);text-transform:uppercase;overflow-wrap:anywhere}
.value{font-size:20px;font-weight:bold;margin-top:8px;overflow-wrap:anywhere}
.small{font-size:14px}
.muted{color:var(--muted)}
input,select,textarea{display:block;width:100%;padding:9px;margin:5px 0 12px;background:var(--card);color:var(--text);border:1px solid #454956;border-radius:5px;font:inherit}
label{font-size:12px;color:#b9bdc8}
textarea{min-height:70px}
.action{margin:4px 3px 4px 0}
.danger{background:#8b2626}
.switch{margin-top:10px;width:100%;padding:8px;background:#333;color:white;border:1px solid #555;border-radius:4px;cursor:pointer}
.switch.on{background:#2a6140}
table{width:100%;border-collapse:collapse;table-layout:fixed}
th,td{text-align:left;padding:8px;border-bottom:1px solid var(--border);overflow-wrap:anywhere;vertical-align:top}
th{color:var(--muted);font-size:11px}
.widget-row{display:flex;gap:8px;align-items:center;padding:8px;border:1px solid var(--border);margin:7px 0;border-radius:5px}
.widget-row span{flex:1;overflow-wrap:anywhere}
.result{white-space:pre-wrap;margin-top:8px}
</style>
</head>
<body>
<header>
  <div><b>MQTT Gateway</b> <span id="version" class="muted"></span></div>
  <a href="/update" style="color:#78d594">OTA</a>
</header>
<main>
<nav>
  <button class="active" data-tab="dashboard">Дашборд</button>
  <button data-tab="mqtt">Последние значения</button>
  <button data-tab="control">Управление</button>
  <button data-tab="widgets">Виджеты</button>
  <button data-tab="settings">Настройки</button>
</nav>

<section id="tab-dashboard" class="tab active">
  <div class="panel">
    <h2>Состояние шлюза</h2>
    <div class="grid">
      <div class="card"><div class="label">Wi-Fi</div><div id="wifi" class="value small">—</div></div>
      <div class="card"><div class="label">Локальный MQTT</div><div id="local" class="value small">—</div></div>
      <div class="card"><div class="label">Главный MQTT</div><div id="remote" class="value small">—</div></div>
      <div class="card"><div class="label">Свободная память</div><div id="heap" class="value small">—</div></div>
    </div>
  </div>
  <div class="panel">
    <h2>Пользовательские виджеты</h2>
    <div id="widgetGrid" class="grid"></div>
  </div>
</section>

<section id="tab-mqtt" class="tab">
  <div class="panel">
    <h2>Последние значения настроенных топиков</h2>
    <div style="overflow-x:auto">
      <table>
        <thead><tr><th>Топик</th><th>Последнее сообщение</th><th>Возраст</th></tr></thead>
        <tbody id="states"></tbody>
      </table>
    </div>
  </div>
</section>

<section id="tab-control" class="tab">
  <div class="panel">
    <h2>Отправить MQTT-команду</h2>
    <form id="commandForm">
      <label>Топик</label>
      <input id="commandTopic" placeholder="cmnd/device/POWER" required>
      <label>Payload</label>
      <textarea id="commandPayload" placeholder="ON"></textarea>
      <button class="action" type="submit">Отправить</button>
    </form>
    <div id="commandResult" class="result"></div>
  </div>
</section>

<section id="tab-widgets" class="tab">
  <div class="panel">
    <h2 id="widgetFormTitle">Добавить виджет</h2>
    <form id="widgetForm">
      <input type="hidden" id="editIndex" value="-1">

      <label>Название</label>
      <input id="wLabel" maxlength="64" required placeholder="Температура в доме">

      <label>Тип</label>
      <select id="wType">
        <option value="sensor">Датчик</option>
        <option value="switch">Переключатель</option>
      </select>

      <label>Топик состояния</label>
      <input id="wTopic" maxlength="128" required placeholder="sensor/temperature или tele/device/SENSOR#Temperature">

      <div id="sensorFields">
        <label>Единица измерения</label>
        <input id="wUnit" maxlength="24" placeholder="°C, V, W, %">
      </div>

      <div id="switchFields" style="display:none">
        <label>Топик команды</label>
        <input id="wCmdTopic" maxlength="128" placeholder="cmnd/device/POWER">
        <label>Payload включения</label>
        <input id="wPayloadOn" maxlength="512" placeholder="ON">
        <label>Payload выключения</label>
        <input id="wPayloadOff" maxlength="512" placeholder="OFF">
      </div>

      <button class="action" id="widgetSubmit" type="submit">Добавить</button>
      <button class="action danger" id="cancelEdit" type="button" style="display:none">Отмена</button>
    </form>
    <div id="widgetResult" class="result"></div>
  </div>

  <div class="panel">
    <h2>Сохранённые виджеты</h2>
    <div id="widgetList"></div>
  </div>
</section>

<section id="tab-settings" class="tab">
  <div class="panel">
    <h2>Wi-Fi и главный MQTT-брокер</h2>
    <form id="settingsForm">
      <label>Wi-Fi SSID</label><input id="ssid" required>
      <label>Пароль Wi-Fi</label><input id="wifiPass" type="password">
      <label>Хост MQTT</label><input id="mqttHost" required>
      <label>Порт MQTT</label><input id="mqttPort" type="number" min="1" max="65535" required>
      <label>Пользователь MQTT</label><input id="mqttUser">
      <label>Пароль MQTT</label><input id="mqttPass" type="password">
      <button class="action" type="submit">Сохранить и перезагрузить</button>
    </form>
    <div id="settingsResult" class="result"></div>
  </div>
</section>
</main>

<script>
let widgets = [];
let statesByTopic = {};

function api(url, options) {
  return fetch(url, options || {cache:"no-store"});
}
function setText(id, value) {
  document.getElementById(id).textContent = value;
}
function activateTab(name) {
  document.querySelectorAll("nav button").forEach(b =>
    b.classList.toggle("active", b.dataset.tab === name));
  document.querySelectorAll(".tab").forEach(s =>
    s.classList.toggle("active", s.id === "tab-" + name));
}
document.querySelectorAll("nav button").forEach(b =>
  b.addEventListener("click", () => activateTab(b.dataset.tab)));

function getWidgetValue(widget) {
  const parts = String(widget.topic || "").split("#");
  const base = parts[0].trim();
  let value = statesByTopic[base];

  if (value === undefined) return {found:false, value:"—"};

  if (parts.length > 1) {
    try {
      const obj = JSON.parse(value);
      const wanted = parts.slice(1).join("#").trim().toLowerCase();
      value = findJsonKey(obj, wanted);
      if (value === undefined || value === null || typeof value === "object")
        return {found:false, value:"—"};
    } catch (_) {
      return {found:false, value:"—"};
    }
  }
  return {found:true, value:String(value)};
}
function findJsonKey(obj, wanted) {
  if (!obj || typeof obj !== "object") return undefined;
  for (const key of Object.keys(obj)) {
    if (key.toLowerCase() === wanted) return obj[key];
    const nested = findJsonKey(obj[key], wanted);
    if (nested !== undefined) return nested;
  }
  return undefined;
}
function addText(parent, className, text) {
  const el = document.createElement("div");
  el.className = className;
  el.textContent = text;
  parent.appendChild(el);
  return el;
}
function sendCommand(topic, payload, resultElement) {
  return api("/api/command", {
    method:"POST",
    headers:{"Content-Type":"application/json"},
    body:JSON.stringify({topic:topic, payload:payload})
  }).then(async r => {
    const text = await r.text();
    if (resultElement) resultElement.textContent = text;
    if (!r.ok) throw new Error(text);
  });
}
function renderWidgets() {
  const grid = document.getElementById("widgetGrid");
  grid.replaceChildren();

  if (!widgets.length) {
    addText(grid, "muted", "Виджетов пока нет. Добавьте датчик или переключатель во вкладке «Виджеты».");
    return;
  }

  widgets.forEach(w => {
    const card = document.createElement("div");
    card.className = "card";
    addText(card, "label", w.label || w.topic);

    const state = getWidgetValue(w);

    if (w.type === "switch") {
      const text = state.found ? state.value.trim().toLowerCase() : "";
      const isOn = ["on","1","true","yes"].includes(text);
      addText(card, "value small", state.found ? (isOn ? "ВКЛ" : "ВЫКЛ") : "—");

      const button = document.createElement("button");
      button.className = "switch" + (isOn ? " on" : "");
      button.textContent = isOn ? "Выключить" : "Включить";
      button.onclick = () => sendCommand(
        w.cmdTopic || w.topic,
        isOn ? (w.payloadOff || "OFF") : (w.payloadOn || "ON"),
        document.getElementById("commandResult")
      ).catch(e => alert("Команда не отправлена: " + e.message));
      card.appendChild(button);
    } else {
      let value = state.value;
      if (state.found && value !== "" && !isNaN(Number(value)))
        value = Number(value).toFixed(1);
      addText(card, "value", state.found ? value : "—");
      if (w.unit) addText(card, "muted", w.unit);
    }

    grid.appendChild(card);
  });
}
function renderWidgetList() {
  const list = document.getElementById("widgetList");
  list.replaceChildren();

  if (!widgets.length) {
    addText(list, "muted", "Виджетов пока нет.");
    return;
  }

  widgets.forEach((w, i) => {
    const row = document.createElement("div");
    row.className = "widget-row";
    const title = document.createElement("span");
    title.textContent = `${w.label} [${w.type}] — ${w.topic}`;
    row.appendChild(title);

    const edit = document.createElement("button");
    edit.className = "action";
    edit.textContent = "Изменить";
    edit.onclick = () => editWidget(i);
    row.appendChild(edit);

    const remove = document.createElement("button");
    remove.className = "action danger";
    remove.textContent = "Удалить";
    remove.onclick = () => removeWidget(i);
    row.appendChild(remove);

    list.appendChild(row);
  });
}
function renderStates(states) {
  statesByTopic = {};
  const body = document.getElementById("states");
  body.replaceChildren();

  states.forEach(s => {
    statesByTopic[s.topic] = s.payload;
    const row = document.createElement("tr");
    [s.topic, s.payload, s.age_seconds + " с"].forEach(value => {
      const cell = document.createElement("td");
      cell.textContent = value;
      row.appendChild(cell);
    });
    body.appendChild(row);
  });

  renderWidgets();
}
async function loadWidgets() {
  const r = await api("/api/widgets");
  if (!r.ok) throw new Error(await r.text());
  widgets = await r.json();
  renderWidgetList();
}
async function updateDashboard() {
  try {
    const r = await api("/api/status");
    if (!r.ok) return;
    const d = await r.json();
    setText("wifi", d.wifi ? `${d.ip} (${d.rssi} dBm)` : "Нет связи");
    setText("local", d.local_mqtt ? "Подключён" : "Нет связи");
    setText("remote", d.main_mqtt ? "Подключён" : "Нет связи");
    setText("heap", Math.round(d.free_heap / 1024) + " КБ");
    setText("version", d.version || "");
    renderStates(d.states || []);
  } catch (_) {}
}
function toggleWidgetFields() {
  const isSwitch = document.getElementById("wType").value === "switch";
  document.getElementById("switchFields").style.display = isSwitch ? "block" : "none";
  document.getElementById("sensorFields").style.display = isSwitch ? "none" : "block";
}
document.getElementById("wType").addEventListener("change", toggleWidgetFields);

function resetWidgetForm() {
  document.getElementById("widgetForm").reset();
  document.getElementById("editIndex").value = "-1";
  document.getElementById("widgetFormTitle").textContent = "Добавить виджет";
  document.getElementById("widgetSubmit").textContent = "Добавить";
  document.getElementById("cancelEdit").style.display = "none";
  toggleWidgetFields();
}
function editWidget(index) {
  const w = widgets[index];
  document.getElementById("editIndex").value = index;
  document.getElementById("wLabel").value = w.label || "";
  document.getElementById("wType").value = w.type || "sensor";
  document.getElementById("wTopic").value = w.topic || "";
  document.getElementById("wUnit").value = w.unit || "";
  document.getElementById("wCmdTopic").value = w.cmdTopic || "";
  document.getElementById("wPayloadOn").value = w.payloadOn || "";
  document.getElementById("wPayloadOff").value = w.payloadOff || "";
  document.getElementById("widgetFormTitle").textContent = "Изменить виджет";
  document.getElementById("widgetSubmit").textContent = "Сохранить";
  document.getElementById("cancelEdit").style.display = "inline-block";
  toggleWidgetFields();
  activateTab("widgets");
}
async function saveWidgets() {
  const r = await api("/api/widgets", {
    method:"POST",
    headers:{"Content-Type":"application/json"},
    body:JSON.stringify(widgets)
  });
  if (!r.ok) throw new Error(await r.text());
  renderWidgetList();
  renderWidgets();
}
async function removeWidget(index) {
  if (!confirm("Удалить виджет?")) return;
  widgets.splice(index, 1);
  try {
    await saveWidgets();
  } catch(e) {
    alert("Не удалось сохранить: " + e.message);
    await loadWidgets();
  }
}
document.getElementById("cancelEdit").onclick = resetWidgetForm;
document.getElementById("widgetForm").addEventListener("submit", async e => {
  e.preventDefault();
  const type = document.getElementById("wType").value;
  const w = {
    label:document.getElementById("wLabel").value.trim(),
    type:type,
    topic:document.getElementById("wTopic").value.trim(),
    unit:type === "sensor" ? document.getElementById("wUnit").value.trim() : "",
    cmdTopic:type === "switch" ? document.getElementById("wCmdTopic").value.trim() : "",
    payloadOn:type === "switch" ? document.getElementById("wPayloadOn").value : "",
    payloadOff:type === "switch" ? document.getElementById("wPayloadOff").value : ""
  };
  const index = Number(document.getElementById("editIndex").value);
  if (index >= 0) widgets[index] = w;
  else widgets.push(w);

  try {
    await saveWidgets();
    resetWidgetForm();
    setText("widgetResult", "Виджет сохранён.");
  } catch(err) {
    setText("widgetResult", "Ошибка: " + err.message);
    await loadWidgets();
  }
});

document.getElementById("commandForm").addEventListener("submit", async e => {
  e.preventDefault();
  try {
    await sendCommand(
      document.getElementById("commandTopic").value.trim(),
      document.getElementById("commandPayload").value,
      document.getElementById("commandResult")
    );
  } catch(err) {
    setText("commandResult", "Ошибка: " + err.message);
  }
});

async function loadSettings() {
  const r = await api("/api/settings");
  if (!r.ok) return;
  const s = await r.json();
  document.getElementById("ssid").value = s.ssid || "";
  document.getElementById("wifiPass").value = s.pass || "";
  document.getElementById("mqttHost").value = s.host || "";
  document.getElementById("mqttPort").value = s.port || 1883;
  document.getElementById("mqttUser").value = s.user || "";
  document.getElementById("mqttPass").value = s.pass_mqtt || "";
}
document.getElementById("settingsForm").addEventListener("submit", async e => {
  e.preventDefault();
  const body = new URLSearchParams({
    ssid:document.getElementById("ssid").value,
    pass:document.getElementById("wifiPass").value,
    host:document.getElementById("mqttHost").value,
    port:document.getElementById("mqttPort").value,
    user:document.getElementById("mqttUser").value,
    pass_mqtt:document.getElementById("mqttPass").value
  });
  try {
    const r = await api("/api/settings", {method:"POST", body:body});
    setText("settingsResult", await r.text());
  } catch(err) {
    setText("settingsResult", "Ошибка: " + err);
  }
});

async function init() {
  try { await loadSettings(); } catch (_) {}
  try { await loadWidgets(); } catch (_) {}
  await updateDashboard();
  setInterval(updateDashboard, 5000);
}
init();
</script>
</body>
</html>
)HTML";

bool validStateTopic(String topic) {
    topic.trim();

    if (topic.isEmpty() || topic.length() > MAX_TOPIC_LENGTH) {
        return false;
    }

    const int hash = topic.indexOf('#');

    String base = hash >= 0 ? topic.substring(0, hash) : topic;
    base.trim();

    if (base.isEmpty() ||
        base.indexOf('#') >= 0 ||
        base.indexOf('+') >= 0) {
        return false;
    }

    if (hash >= 0) {
        String key = topic.substring(hash + 1);
        key.trim();

        if (key.isEmpty() || key.indexOf('#') >= 0) {
            return false;
        }
    }

    return true;
}

bool validCommandTopic(const String& topic) {
    return !topic.isEmpty() &&
           topic.length() <= MAX_TOPIC_LENGTH &&
           topic.indexOf('#') < 0 &&
           topic.indexOf('+') < 0;
}

void handleRoot() {
    server.send_P(200, "text/html; charset=utf-8", DASHBOARD_HTML);
}

void handleStatus() {
    JsonDocument doc;

    const String ip = isApMode
        ? WiFi.softAPIP().toString()
        : WiFi.localIP().toString();

    doc["ip"] = ip;
    doc["wifi"] = isApMode || WiFi.status() == WL_CONNECTED;
    doc["rssi"] = isApMode ? 0 : WiFi.RSSI();
    doc["free_heap"] = ESP.getFreeHeap();
    doc["local_mqtt"] = localMqttClient && localMqttClient->connected();
    doc["main_mqtt"] = mainMqttClient && mainMqttClient->connected();
    doc["version"] = FIRMWARE_VERSION;

    JsonArray states = doc["states"].to<JsonArray>();
    const unsigned long now = millis();

    for (size_t i = 0; i < cachedStateCapacity; ++i) {
        if (!cachedStates[i].used) {
            continue;
        }

        JsonObject state = states.add<JsonObject>();
        state["topic"] = cachedStates[i].topic;
        state["payload"] = cachedStates[i].payload;
        state["age_seconds"] =
            (now - cachedStates[i].updatedAt) / 1000UL;
    }

    String response;
    response.reserve(4096);
    serializeJson(doc, response);
    server.send(200, "application/json; charset=utf-8", response);
}

void handleGetSettings() {
    const SystemSettings& s = Settings::get();

    JsonDocument doc;
    doc["ssid"] = s.wifiSsid;
    doc["pass"] = s.wifiPassword;
    doc["host"] = s.mqttHost;
    doc["port"] = s.mqttPort;
    doc["user"] = s.mqttUser;
    doc["pass_mqtt"] = s.mqttPassword;

    String response;
    serializeJson(doc, response);
    server.send(200, "application/json; charset=utf-8", response);
}

void handlePostSettings() {
    if (!server.hasArg("ssid") ||
        !server.hasArg("host") ||
        !server.hasArg("port")) {
        server.send(400, "text/plain; charset=utf-8", "Не заполнены обязательные поля");
        return;
    }

    String host = server.arg("host");
    host.trim();

    const long port = server.arg("port").toInt();

    if (server.arg("ssid").isEmpty() ||
        host.isEmpty() ||
        port < 1 ||
        port > 65535) {
        server.send(400, "text/plain; charset=utf-8", "Проверьте SSID, MQTT-хост и порт");
        return;
    }

    SystemSettings settings = Settings::get();
    settings.wifiSsid = server.arg("ssid");
    settings.wifiPassword = server.arg("pass");
    settings.mqttHost = host;
    settings.mqttPort = static_cast<uint16_t>(port);
    settings.mqttUser = server.arg("user");
    settings.mqttPassword = server.arg("pass_mqtt");

    if (!Settings::save(settings)) {
        server.send(500, "text/plain; charset=utf-8", "Не удалось сохранить настройки");
        return;
    }

    server.send(200, "text/plain; charset=utf-8", "Настройки сохранены, перезагрузка...");
    delay(500);
    ESP.restart();
}

void handleGetWidgets() {
    server.send(
        200,
        "application/json; charset=utf-8",
        Settings::widgetsJson()
    );
}

void handlePostWidgets() {
    if (!server.hasArg("plain")) {
        server.send(400, "text/plain", "Missing body");
        return;
    }

    const String body = server.arg("plain");

    constexpr size_t MAX_WIDGETS_JSON_BYTES = 8192;

    if (body.length() > MAX_WIDGETS_JSON_BYTES) {
        server.send(
            413,
            "text/plain; charset=utf-8",
            "Конфигурация виджетов превышает 8192 байта"
        );
        return;
    }

    DynamicJsonDocument doc(4096);
    DeserializationError error = deserializeJson(doc, body);

    if (error || !doc.is<JsonArray>()) {
        server.send(
            400,
            "text/plain; charset=utf-8",
            "Ожидается корректный JSON-массив виджетов"
        );
        return;
    }

    Settings::saveWidgets(body);
    server.send(200, "text/plain", "OK");
}

void handleCommand() {
    if (!server.hasArg("plain")) {
        server.send(400, "text/plain; charset=utf-8", "Нет тела запроса");
        return;
    }

    JsonDocument doc;
    const DeserializationError error =
        deserializeJson(doc, server.arg("plain"));

    if (error) {
        server.send(400, "text/plain; charset=utf-8", "Некорректный JSON");
        return;
    }

    String topic = doc["topic"] | "";
    String payload = doc["payload"] | "";
    topic.trim();

    if (!validCommandTopic(topic) ||
        payload.length() > MAX_COMMAND_PAYLOAD_LENGTH) {
        server.send(400, "text/plain; charset=utf-8", "Некорректный топик или слишком длинный payload");
        return;
    }

    if (!localMqttClient || !localMqttClient->connected()) {
        server.send(503, "text/plain; charset=utf-8", "Локальный MQTT-клиент не подключён");
        return;
    }

    const bool ok = localMqttClient->publish(
        topic.c_str(),
        reinterpret_cast<const uint8_t*>(payload.c_str()),
        payload.length(),
        false
    );

    if (ok) {
        server.send(200, "text/plain; charset=utf-8", "Команда отправлена");
    } else {
        server.send(500, "text/plain; charset=utf-8", "Ошибка публикации команды");
    }
}

} // namespace

namespace WebDashboard {

void begin(
    PubSubClient& localMqtt,
    PubSubClient& mainMqtt,
    CachedState* states,
    size_t stateCapacity,
    bool apMode
) {
    localMqttClient = &localMqtt;
    mainMqttClient = &mainMqtt;
    cachedStates = states;
    cachedStateCapacity = stateCapacity;
    isApMode = apMode;

    if (serverStarted) {
        return;
    }

    server.on("/", HTTP_GET, handleRoot);
    server.on("/api/status", HTTP_GET, handleStatus);
    server.on("/api/settings", HTTP_GET, handleGetSettings);
    server.on("/api/settings", HTTP_POST, handlePostSettings);
    server.on("/api/widgets", HTTP_GET, handleGetWidgets);
    server.on("/api/widgets", HTTP_POST, handlePostWidgets);
    server.on("/api/command", HTTP_POST, handleCommand);

    server.onNotFound([]() {
        server.send(404, "text/plain; charset=utf-8", "Not found");
    });

    ElegantOTA.begin(&server);
    server.begin();
    serverStarted = true;
}

void handleClient() {
    server.handleClient();
    ElegantOTA.loop();
}

} // namespace WebDashboard
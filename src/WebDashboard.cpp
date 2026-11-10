#include "WebDashboard.h"
#include "Settings.h"
#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <cstring>
#include <ElegantOTA.h>

namespace {

WebServer server(80);
PubSubClient* localMqttClient = nullptr;
PubSubClient* mainMqttClient = nullptr;
std::vector<CachedState>* cachedStates = nullptr;
size_t cachedStateCount = 0;
bool isConfigMode = false;

// HTML настройки (AP)
const char SETUP_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html>
<html lang="ru">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <title>Настройка шлюза</title>
    <style>
        :root { color-scheme: dark; --background: #101116; --panel: #181a22; --card: #111319; --border: #242731; --text: #e2e4eb; --accent: #78d594; --error: #f27d83; }
        body { margin: 0; background: var(--background); color: var(--text); font-family: Arial, sans-serif; display: flex; justify-content: center; align-items: center; min-height: 100vh; padding: 16px; box-sizing: border-box; }
        .panel { background: var(--panel); border: 1px solid var(--border); border-radius: 8px; padding: 24px; width: 100%; max-width: 400px; }
        h2 { margin-top: 0; font-size: 18px; color: var(--accent); text-align: center; }
        input, button { width: 100%; border-radius: 5px; font: inherit; box-sizing: border-box; }
        input { display: block; padding: 10px; margin: 6px 0 14px; color: var(--text); background: var(--card); border: 1px solid #353945; }
        button { padding: 12px; background: #2a6140; color: white; border: none; cursor: pointer; font-weight: bold; }
        button:hover { background: #34774d; }
        label { color: #b9bdc8; font-size: 12px; }
        #result { margin-top: 12px; text-align: center; font-size: 13px; }
        #result.ok { color: var(--accent); }
        #result.error { color: var(--error); }
    </style>
</head>
<body>
<div class="panel">
    <h2>Настройка шлюза</h2>
    <form id="settingsForm">
        <label>Имя Wi-Fi сети (SSID)</label><input id="setSsid" required>
        <label>Пароль Wi-Fi</label><input id="setPass" type="password">
        <label>Хост главного MQTT</label><input id="setMqttHost" required>
        <label>Порт главного MQTT</label><input id="setMqttPort" type="number" value="1883" required>
        <label>Пользователь MQTT</label><input id="setMqttUser">
        <label>Пароль MQTT</label><input id="setMqttPass" type="password">
        <button type="submit">Сохранить и перезагрузить</button>
    </form>
    <div id="result"></div>
</div>
<script>
async function loadSettings() {
    try {
        const res = await fetch("/api/settings");
        const data = await res.json();
        document.getElementById("setSsid").value = data.ssid || "";
        document.getElementById("setPass").value = data.pass || "";
        document.getElementById("setMqttHost").value = data.host || "";
        document.getElementById("setMqttPort").value = data.port || 1883;
        document.getElementById("setMqttUser").value = data.user || "";
        document.getElementById("setMqttPass").value = data.pass_mqtt || "";
    } catch(e) {}
}
document.getElementById("settingsForm").addEventListener("submit", async function(e) {
    e.preventDefault();
    const res = document.getElementById("result");
    const body = new URLSearchParams({
        ssid: document.getElementById("setSsid").value, pass: document.getElementById("setPass").value,
        host: document.getElementById("setMqttHost").value, port: document.getElementById("setMqttPort").value,
        user: document.getElementById("setMqttUser").value, pass_mqtt: document.getElementById("setMqttPass").value
    });
    try {
        const response = await fetch("/api/settings", { method: "POST", body });
        res.textContent = await response.text();
        res.className = "ok";
        setTimeout(() => location.reload(), 4000);
    } catch(err) { res.textContent = "Ошибка: " + err; res.className = "error"; }
});
</script>
</body>
</html>
)HTML";

// Основной дашборд с кастомизацией виджетов
const char DASHBOARD_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html>
<html lang="ru">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <title>MQTT Gateway</title>
    <style>
        :root { color-scheme: dark; --background: #101116; --header: #0d0e12; --panel: #181a22; --card: #111319; --border: #242731; --text: #e2e4eb; --muted: #848996; --accent: #78d594; --error: #f27d83; --blue: #4a90e2; }
        * { box-sizing: border-box; }
        body { margin: 0; background: var(--background); color: var(--text); font-family: Arial, sans-serif; font-size: 14px; }
        header { min-height: 54px; display: flex; align-items: center; justify-content: space-between; padding: 10px 22px; background: var(--header); border-bottom: 1px solid var(--border); }
        .brand { font-size: 12px; font-weight: bold; text-transform: uppercase; }
        .header-status { color: var(--muted); font-size: 12px; }
        .header-status.ok { color: var(--accent); }
        .header-status.error { color: var(--error); }
        main { max-width: 1160px; margin: 26px auto; padding: 0 16px 30px; }
        .tabs { display: flex; gap: 6px; margin-bottom: 14px; border-bottom: 1px solid var(--border); }
        .tab-button { background: transparent; border: 0; border-bottom: 2px solid transparent; color: var(--muted); padding: 12px 16px; cursor: pointer; font-size: 13px; }
        .tab-button.active { color: var(--text); border-bottom-color: var(--accent); }
        .tab-panel { display: none; }
        .tab-panel.active { display: block; }
        .panel { background: var(--panel); border: 1px solid var(--border); border-radius: 8px; padding: 16px; margin-bottom: 16px; }
        .panel-title { margin: 0 0 14px; color: #a9adb9; font-size: 11px; font-weight: bold; text-transform: uppercase; display: flex; justify-content: space-between; align-items: center; }
        
        .grid { display: grid; grid-template-columns: repeat(4, minmax(0, 1fr)); gap: 10px; }
        @media(max-width: 900px) { .grid { grid-template-columns: repeat(2, minmax(0, 1fr)); } }
        
        .card { padding: 14px; background: var(--card); border: 1px solid var(--border); border-radius: 6px; position: relative; }
        .card-label { color: var(--muted); font-size: 11px; text-transform: uppercase; }
        .card-value { margin-top: 8px; font-size: 20px; font-weight: bold; word-break: break-all; }
        .card-value.small { font-size: 14px; }
        .card-subtitle { margin-top: 6px; color: var(--muted); font-size: 10px; word-break: break-all; }
        .card-value.negative { color: var(--error); }
        
        .btn-switch { margin-top: 10px; width: 100%; padding: 8px; background: #333; color: white; border: 1px solid #555; border-radius: 4px; cursor: pointer; font-weight: bold; }
        .btn-switch.on { background: #2a6140; border-color: #3b8057; }

        .table-wrap { overflow-x: auto; }
        table { width: 100%; border-collapse: collapse; font-size: 12px; table-layout: fixed; }
        th, td { padding: 10px; border-bottom: 1px solid var(--border); text-align: left; overflow: hidden; text-overflow: ellipsis; word-wrap: break-word; }
        th { color: var(--muted); font-size: 10px; text-transform: uppercase; }
        td:nth-child(1) { width: 45%; }
        td:nth-child(2) { width: 45%; word-break: break-all; }
        td:nth-child(3) { width: 10%; white-space: nowrap; }

        input, select, textarea, button.action { width: 100%; border-radius: 5px; font: inherit; }
        input, select, textarea { display: block; padding: 10px; margin: 6px 0 12px; color: var(--text); background: var(--card); border: 1px solid #353945; }
        button.action { width: auto; padding: 10px 18px; background: #2a6140; color: white; border: 1px solid #3b8057; cursor: pointer; }
        button.action:hover { background: #34774d; }
        button.danger { background: #8b2626; border-color: #a83232; }
        label { color: #b9bdc8; font-size: 12px; }
        
        .widget-row { display: flex; gap: 8px; align-items: center; background: var(--card); padding: 8px; border: 1px solid var(--border); border-radius: 5px; margin-bottom: 8px; }
        .widget-row span { flex-grow: 1; font-size: 13px; }
    </style>
</head>
<body>
<header>
    <div class="brand">MQTT Gateway</div>
    <a href="/update" target="_blank" style="color: var(--muted); text-decoration: none; font-size: 12px; transition: color 0.2s;" onmouseover="this.style.color='var(--text)'" onmouseout="this.style.color='var(--muted)'">Прошивка (OTA) &rarr;</a>
</header>
<main>
    <nav class="tabs">
        <button type="button" class="tab-button active" data-tab="dashboard">Дашборд</button>
        <button type="button" class="tab-button" data-tab="mqtt">MQTT</button>
        <button type="button" class="tab-button" data-tab="control">Управление</button>
        <button type="button" class="tab-button" data-tab="settings">Настройки</button>
        <button type="button" class="tab-button" data-tab="dashconfig">⚙️ Настройка дашборда</button>
        <button type="button" class="tab-button" data-tab="max">📢 MAX</button>
    </nav>

    <!-- Дашборд -->
    <section id="tab-dashboard" class="tab-panel active">
        <div class="panel">
            <h2 class="panel-title">Состояние шлюза</h2>
            <div class="grid">
                <div class="card"><div class="card-label">Локальный MQTT</div><div id="localMqtt" class="card-value small">—</div></div>
                <div class="card"><div class="card-label">Главный MQTT</div><div id="mainMqtt" class="card-value small">—</div></div>
                <div class="card"><div class="card-label">Wi-Fi</div><div id="wifiRssi" class="card-value">—</div><div class="card-subtitle">dBm</div></div>
                <div class="card"><div class="card-label">Память ОЗУ</div><div id="freeHeap" class="card-value">—</div></div>
            </div>
        </div>

        <div class="panel">
            <h2 class="panel-title">Избранные приборы и датчики</h2>
            <div id="customWidgetsGrid" class="grid"></div>
        </div>
    </section>

    <!-- MQTT -->
    <section id="tab-mqtt" class="tab-panel">
        <div class="panel">
            <h2 class="panel-title">Все сообщения в памяти</h2>
            <div class="table-wrap">
                <table>
                    <thead><tr><th>Топик</th><th>Значение</th><th>Возраст</th></tr></thead>
                    <tbody id="mqttTable"></tbody>
                </table>
            </div>
        </div>
    </section>

    <!-- Управление -->
    <section id="tab-control" class="tab-panel">
        <div class="panel">
            <h2 class="panel-title">Локальная команда MQTT</h2>
            <form id="commandForm">
                <label>Топик (cmnd/...)</label><input id="commandTopic" placeholder="cmnd/zb_bridge/POWER" required>
                <label>Сообщение</label><textarea id="commandPayload" placeholder="ON"></textarea>
                <button type="submit" class="action">Отправить</button>
            </form>
            <div id="commandResult"></div>
        </div>
    </section>

    <!-- Настройки системы -->
    <section id="tab-settings" class="tab-panel">
        <div class="panel">
            <h2 class="panel-title">Параметры сети и MQTT</h2>
            <form id="settingsForm">
                <label>Wi-Fi SSID</label><input id="setSsid" required>
                <label>Wi-Fi Пароль</label><input id="setPass" type="password">
                <label>MQTT Хост</label><input id="setMqttHost" required>
                <label>MQTT Порт</label><input id="setMqttPort" type="number" required>
                <label>MQTT Пользователь</label><input id="setMqttUser">
                <label>MQTT Пароль</label><input id="setMqttPass" type="password">
                <button type="submit" class="action">Сохранить и перезагрузить</button>
            </form>
            <div id="settingsResult"></div>
        </div>
    </section>

    <!-- Настройка виджетов дашборда -->
    <!-- Настройка виджетов дашборда -->
    <section id="tab-dashconfig" class="tab-panel">
        <div class="panel">
            <h2 class="panel-title" id="formTitle">Добавить элемент на дашборд</h2>
            <form id="addWidgetForm">
                <input type="hidden" id="editIndex" value="-1">
                
                <label>Название</label>
                <input id="wLabel" required placeholder="Например: Температура в доме">
                
                <label>Тип элемента</label>
                <select id="wType" onchange="toggleSwitchFields()">
                    <option value="sensor">Датчик (чтение значения)</option>
                    <option value="switch">Кнопка / Выключатель (управление)</option>
                </select>
                
                <label>Топик состояния (State Topic)</label>
                <input id="wTopic" placeholder="must_pv18/pv_voltage или tele/...#Temperature" required>
                
                <!-- Поле единицы измерения -->
                <div id="sensorFields">
                    <label>Единица измерения (суффикс)</label>
                    <input id="wUnit" placeholder="V, A, W, °C, %">
                </div>

                <div id="switchFields" style="display:none;">
                    <label>Топик команды (Command Topic)</label>
                    <input id="wCmdTopic" placeholder="cmnd/tasmota_zb/zbsend">
                    
                    <label>JSON для ВКЛ (ON)</label>
                    <input id="wPayloadOn" placeholder='{"Device":"Patio","send":{"power": "on"}}'>
                    
                    <label>JSON для ВЫКЛ (OFF)</label>
                    <input id="wPayloadOff" placeholder='{"Device":"Patio","send":{"power": "off"}}'>
                </div>

                <!-- Блок настройки оповещений MAX -->
                <!-- Блок оповещений в MAX -->
                <div style="margin-top: 15px; border-top: 1px solid var(--border); padding-top: 12px;">
                    <div style="display: flex; justify-content: space-between; align-items: center; margin-bottom: 8px;">
                        <label style="color: var(--accent); font-weight: bold; margin:0;">📢 Оповещения в MAX</label>
                        <button type="button" class="action" style="padding: 4px 10px; font-size: 11px;" onclick="addAlertRule()">+ Добавить правило</button>
                    </div>
                    <div id="alertRulesContainer"></div>
                </div>

                <div style="display: flex; gap: 8px; margin-top: 14px;">
                    <button type="submit" class="action" id="submitBtn">Добавить</button>
                    <button type="button" class="action danger" id="cancelBtn" style="display:none;" onclick="cancelEdit()">Отмена</button>
                </div>
            </form>
        </div>
        <div class="panel">
            <h2 class="panel-title">Активные элементы на дашборде</h2>
            <div id="widgetsList"></div>
        </div>
    </section>
    <!-- Настройки MAX -->
    <section id="tab-max" class="tab-panel">
        <div class="panel">
            <h2 class="panel-title">Параметры интеграции MAX</h2>
            <form id="maxForm">
                <label>Токен авторизации (Authorization)</label>
                <input id="maxToken" type="password" placeholder="Ваш токен бота MAX" required>
                
                <label>ID пользователя (User ID)</label>
                <input id="maxUserId" placeholder="Например: 123456789" required>

                <div style="display: flex; gap: 8px;">
                    <button type="submit" class="action">Сохранить</button>
                    <button type="button" class="action" style="background:#205081; border-color:#2a66a1;" onclick="sendMaxTest()">Тест (Отправить сообщение)</button>
                </div>
            </form>
            <div id="maxResult"></div>
        </div>
    </section>
</main>

<!-- Модальное окно графика -->
<div id="chartModal" style="display:none; position:fixed; top:0; left:0; width:100%; height:100%; background:rgba(0,0,0,0.8); z-index:1000; justify-content:center; align-items:center;">
    <div style="background:var(--panel); border:1px solid var(--border); border-radius:10px; padding:20px; width:90%; max-width:600px; position:relative;">
        <h3 id="chartTitle" style="margin-top:0; color:var(--text); font-size:16px;">График за 24 часа</h3>
        <div id="chartContainer" style="width:100%; height:250px; display:flex; align-items:center; justify-content:center;">
            <svg id="sensorChart" width="100%" height="100%" viewBox="0 0 500 200" style="overflow:visible;"></svg>
        </div>
        <button class="action" style="margin-top:15px; width:100%;" onclick="closeChart()">Закрыть</button>
    </div>
</div>

<script>
let globalStates = {};
let activeWidgets = [];

let currentAlertRules = [];

function renderAlertRules(rules) {
    const container = document.getElementById("alertRulesContainer");
    container.replaceChildren();
    
    if (!rules || rules.length === 0) {
        container.innerHTML = '<div class="empty-notice" style="color:var(--muted); font-size:12px; margin-bottom:10px;">Нет активных правил оповещений.</div>';
        return;
    }

    rules.forEach((r, idx) => {
        const isBetween = r.cond === 'between';
        const div = document.createElement("div");
        div.className = "alert-rule-card";
        div.style.cssText = "background:var(--card); border:1px solid var(--border); padding:10px; border-radius:6px; margin-bottom:8px; position:relative;";
        div.innerHTML = `
            <div style="display:flex; justify-content:space-between; margin-bottom:6px;">
                <span style="font-size:11px; color:var(--muted);">Правило #${idx + 1}</span>
                <button type="button" class="action danger" style="padding:2px 6px; font-size:10px;" onclick="removeAlertRule(${idx})">Удалить</button>
            </div>
            <label>Условие</label>
            <select class="rCond" onchange="onConditionChange(this)" style="width:100%; padding:6px; margin:4px 0 8px; background:var(--background); color:var(--text); border:1px solid #353945; border-radius:4px;">
                <option value="change" ${r.cond==='change'?'selected':''}>При любом изменении</option>
                <option value="below" ${r.cond==='below'?'selected':''}>Ниже порога (&lt;)</option>
                <option value="above" ${r.cond==='above'?'selected':''}>Выше порога (&gt;)</option>
                <option value="between" ${r.cond==='between'?'selected':''}>Между (От и До)</option>
            </select>

            <div class="singleValBlock" style="display: ${isBetween ? 'none' : 'block'};">
                <label>Порог</label>
                <input class="rVal" type="number" step="any" value="${!isBetween && r.val !== undefined ? r.val : ''}" placeholder="Значение порога" style="width:100%; padding:6px; margin:4px 0 8px; background:var(--background); color:var(--text); border:1px solid #353945; border-radius:4px;">
            </div>

            <div class="rangeValBlock" style="display: ${isBetween ? 'flex' : 'none'}; gap: 8px; margin-bottom: 8px;">
                <div style="flex:1;">
                    <label>От (мин)</label>
                    <input class="rValMin" type="number" step="any" value="${isBetween && r.val !== undefined ? r.val : ''}" placeholder="Мин" style="width:100%; padding:6px; margin:4px 0 0; background:var(--background); color:var(--text); border:1px solid #353945; border-radius:4px;">
                </div>
                <div style="flex:1;">
                    <label>До (макс)</label>
                    <input class="rValMax" type="number" step="any" value="${isBetween && r.valMax !== undefined ? r.valMax : ''}" placeholder="Макс" style="width:100%; padding:6px; margin:4px 0 0; background:var(--background); color:var(--text); border:1px solid #353945; border-radius:4px;">
                </div>
            </div>

            <label>Текст сообщения</label>
            <input class="rMsg" type="text" value="${r.msg || ''}" placeholder="Текст уведомления" style="width:100%; padding:6px; margin:4px 0 4px; background:var(--background); color:var(--text); border:1px solid #353945; border-radius:4px;">
        `;
        container.appendChild(div);
    });
}
    
window.addAlertRule = function(cond = 'below', val = '', valMax = '', msg = '') {
    currentAlertRules.push({ cond, val, valMax, msg });
    renderAlertRules(currentAlertRules);
}

window.removeAlertRule = function(index) {
    currentAlertRules.splice(index, 1);
    renderAlertRules(currentAlertRules);
}

function toggleSwitchFields() {
    const type = document.getElementById("wType").value;
    document.getElementById("switchFields").style.display = (type === "switch") ? "block" : "none";
    document.getElementById("sensorFields").style.display = (type === "sensor") ? "block" : "none";
}

function activateTab(name) {
    document.querySelectorAll(".tab-button").forEach(b => b.classList.toggle("active", b.dataset.tab === name));
    document.querySelectorAll(".tab-panel").forEach(p => p.classList.toggle("active", p.id === "tab-" + name));
}
document.querySelectorAll(".tab-button").forEach(b => b.addEventListener("click", () => activateTab(b.dataset.tab)));

function setConnection(id, connected) {
    const el = document.getElementById(id);
    el.textContent = connected ? "Подключён" : "Нет связи";
    el.className = "card-value small " + (connected ? "ok" : "error");
}

async function sendCommand(topic, payload) {
    try {
        await fetch("/api/command", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({ topic: topic, payload: payload })
        });
    } catch(e) { 
        alert("Ошибка отправки команды"); 
    }
}

function findJsonValue(obj, targetKey) {
    if (!obj || typeof obj !== 'object') return undefined;
    
    targetKey = targetKey.toLowerCase();
    
    for (const key of Object.keys(obj)) {
        if (key.toLowerCase() === targetKey) {
            return obj[key];
        }
        if (typeof obj[key] === 'object' && obj[key] !== null) {
            const res = findJsonValue(obj[key], targetKey);
            if (res !== undefined) return res;
        }
    }
    return undefined;
}

function formatSensorValue(v) {
    if (v === null || v === undefined || v === "—") return "—";
    const num = Number(v);
    // Проверяем, что это число, а не boolean или пустая строка
    if (!isNaN(num) && typeof v !== 'boolean' && String(v).trim() !== "") {
        return num.toFixed(1);
    }
    return v;
}

function renderDashboardWidgets() {
    const container = document.getElementById("customWidgetsGrid");
    container.replaceChildren();

    if (activeWidgets.length === 0) {
        container.innerHTML = '<div style="color:var(--muted); grid-column: span 4;">Дашборд пуст. Перейдите в "⚙️ Настройка дашборда", чтобы добавить датчики или выключатели.</div>';
        return;
    }

    activeWidgets.forEach((w, index) => {
        const card = document.createElement("div");
        card.className = "card";
        
        let val = "—";
        let hasValue = false;

        if (w.topic) {
            if (w.topic.includes('#')) {
                const parts = w.topic.split('#');
                const baseTopic = parts[0].trim();
                const jsonKey = parts[1].trim();
                
                const rawPayload = globalStates[baseTopic];
                if (rawPayload !== undefined) {
                    try {
                        const parsed = JSON.parse(rawPayload);
                        const found = findJsonValue(parsed, jsonKey);
                        if (found !== undefined && typeof found !== 'object') {
                            val = found;
                            hasValue = true;
                        }
                    } catch (e) {}
                }
            } else {
                const cleanTopic = w.topic.trim();
                if (globalStates[cleanTopic] !== undefined) {
                    val = globalStates[cleanTopic];
                    hasValue = true;
                }
            }
        }

        if (w.type === "sensor") {
            const formattedVal = formatSensorValue(val);
            const num = Number(val);
            const isNegative = !isNaN(num) && num < 0;
            const unitStr = w.unit ? ` <span style="font-size: 14px; color: var(--muted);">${w.unit}</span>` : "";
            card.innerHTML = `<div class="card-label">${w.label}</div><div class="card-value ${isNegative ? 'negative' : ''}">${formattedVal}${unitStr}</div>`;
            card.style.cursor = "pointer";
            card.title = "Кликните для просмотра графика";
            card.onclick = () => openChart(w.topic, w.label, w.unit);
        } else if (w.type === "switch") {
            const valStr = String(val).toLowerCase().trim();
            const isOn = hasValue && (valStr === "on" || valStr === "1" || valStr === "true" || valStr === "yes");
            const displayVal = hasValue ? (isOn ? "ВКЛ" : "ВЫКЛ") : "—";
            
            card.innerHTML = `<div class="card-label">${w.label}</div><div class="card-value small">${displayVal}</div><button class="btn-switch ${isOn ? 'on' : ''}">${isOn ? 'ВЫКЛ' : 'ВКЛ'}</button>`;
            
            const btn = card.querySelector("button");
            btn.onclick = () => {
                const nextPayload = !isOn 
                    ? (w.payloadOn || "ON") 
                    : (w.payloadOff || "OFF");
                
                sendCommand(w.cmdTopic || w.topic, nextPayload);
            };
        }
        container.appendChild(card);
    });
}

async function openChart(topic, label, unit) {
    document.getElementById("chartTitle").textContent = label + " (24 часа)";
    document.getElementById("chartModal").style.display = "flex";
    
    const svg = document.getElementById("sensorChart");
    svg.innerHTML = '<text x="250" y="100" fill="#848996" text-anchor="middle">Загрузка данных...</text>';

    try {
        const res = await fetch(`/api/history?topic=${encodeURIComponent(topic)}`);
        const data = await res.json();

        if (!Array.isArray(data) || data.length === 0) {
            svg.innerHTML = '<text x="250" y="100" fill="#848996" text-anchor="middle">Нет данных истории</text>';
            return;
        }

        const values = data.map(d => d.val);
        const min = Math.min(...values);
        const max = Math.max(...values);
        const range = (max - min) || 1;

        let points = "";
        const width = 500;
        const height = 180;

        data.forEach((d, i) => {
            const x = (i / (data.length - 1 || 1)) * width;
            const y = height - ((d.val - min) / range) * (height - 20) - 10;
            points += `${x},${y} `;
        });

        svg.innerHTML = `
            <rect width="500" height="200" fill="#111319" rx="5"/>
            <line x1="0" y1="40" x2="500" y2="40" stroke="#242731" stroke-dasharray="4"/>
            <line x1="0" y1="110" x2="500" y2="110" stroke="#242731" stroke-dasharray="4"/>
            <line x1="0" y1="180" x2="500" y2="180" stroke="#242731" stroke-dasharray="4"/>
            <text x="10" y="35" fill="#848996" font-size="10">${max.toFixed(1)} ${unit || ''}</text>
            <text x="10" y="175" fill="#848996" font-size="10">${min.toFixed(1)} ${unit || ''}</text>
            <polyline fill="none" stroke="#78d594" stroke-width="3" points="${points}"/>
        `;
    } catch(e) {
        svg.innerHTML = '<text x="250" y="100" fill="#f27d83" text-anchor="middle">Ошибка загрузки графика</text>';
    }
}

function closeChart() {
    document.getElementById("chartModal").style.display = "none";
}

function renderWidgetsConfigList() {
    const list = document.getElementById("widgetsList");
    list.replaceChildren();
    
    if (activeWidgets.length === 0) {
        list.innerHTML = '<div style="color:var(--muted);">Нет добавленных элементов.</div>';
        return;
    }
    
    activeWidgets.forEach((w, index) => {
        const row = document.createElement("div");
        row.className = "widget-row";
        row.innerHTML = `
            <span><b>${w.label}</b> [${w.type}] → <code>${w.topic}</code></span>
            <button class="action" style="background:#205081; border-color:#2a66a1; padding:6px 12px;" onclick="editWidget(${index})">Изменить</button>
            <button class="action danger" style="padding:6px 12px;" onclick="removeWidget(${index})">Удалить</button>
        `;
        list.appendChild(row);
    });
}

async function loadWidgets() {
    try {
        const res = await fetch("/api/widgets", { cache: "no-store" });
        if (res.ok) {
            const data = await res.json();
            if (Array.isArray(data)) {
                activeWidgets = data;
                renderWidgetsConfigList();
                renderDashboardWidgets();
            }
        }
    } catch(e) {
        console.error("Ошибка загрузки виджетов:", e);
    }
}

async function saveWidgets() {
    await fetch("/api/widgets", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(activeWidgets)
    });
    renderWidgetsConfigList();
    renderDashboardWidgets();
}

async function loadMaxSettings() {
    try {
        const res = await fetch("/api/max-settings");
        const data = await res.json();
        document.getElementById("maxToken").value = data.token || "";
        document.getElementById("maxUserId").value = data.userid || "";
    } catch(e) {}
}

document.getElementById("maxForm").addEventListener("submit", async function(e) {
    e.preventDefault();
    const res = document.getElementById("maxResult");
    const body = new URLSearchParams({
        token: document.getElementById("maxToken").value.trim(),
        userid: document.getElementById("maxUserId").value.trim()
    });
    try {
        const response = await fetch("/api/max-settings", { method: "POST", body });
        res.textContent = await response.text();
        res.className = "ok";
    } catch(err) { res.textContent = "Ошибка: " + err; res.className = "error"; }
});

async function sendMaxTest() {
    const res = document.getElementById("maxResult");
    res.textContent = "Отправка тестового сообщения...";
    res.className = "";
    try {
        const response = await fetch("/api/max-test", { method: "POST" });
        res.textContent = await response.text();
        res.className = response.ok ? "ok" : "error";
    } catch(err) {
        res.textContent = "Ошибка теста: " + err;
        res.className = "error";
    }
}

window.editWidget = function(index) {
    const w = activeWidgets[index];
    document.getElementById("editIndex").value = index;
    document.getElementById("wLabel").value = w.label;
    document.getElementById("wType").value = w.type;
    document.getElementById("wTopic").value = w.topic;
    document.getElementById("wCmdTopic").value = w.cmdTopic || "";
    document.getElementById("wPayloadOn").value = w.payloadOn || "";
    document.getElementById("wPayloadOff").value = w.payloadOff || "";
    document.getElementById("wUnit").value = w.unit || "";
    currentAlertRules = w.alerts || [];
    renderAlertRules(currentAlertRules);
    
    toggleSwitchFields();
    
    document.getElementById("formTitle").textContent = "Редактировать элемент";
    document.getElementById("submitBtn").textContent = "Сохранить";
    document.getElementById("cancelBtn").style.display = "inline-block";
    
    // Перекручиваем страницу наверх к форме
    window.scrollTo({ top: 0, behavior: 'smooth' });
}

window.onConditionChange = function(select) {
    const card = select.closest('.alert-rule-card');
    const isBetween = select.value === 'between';
    
    const singleBlock = card.querySelector('.singleValBlock');
    const rangeBlock = card.querySelector('.rangeValBlock');
    
    if (singleBlock) singleBlock.style.display = isBetween ? 'none' : 'block';
    if (rangeBlock) rangeBlock.style.display = isBetween ? 'flex' : 'none';
}
    
window.cancelEdit = function() {
    document.getElementById("addWidgetForm").reset();
    document.getElementById("editIndex").value = "-1";
    document.getElementById("formTitle").textContent = "Добавить элемент на дашборд";
    document.getElementById("submitBtn").textContent = "Добавить";
    document.getElementById("cancelBtn").style.display = "none";
    currentAlertRules = [];
    renderAlertRules(currentAlertRules);
    toggleSwitchFields();
}

window.removeWidget = async function(index) {
    if (confirm("Удалить этот элемент?")) {
        activeWidgets.splice(index, 1);
        await saveWidgets();
        cancelEdit();
    }
}

document.getElementById("addWidgetForm").addEventListener("submit", async function(e) {
    e.preventDefault();
    const editIndex = parseInt(document.getElementById("editIndex").value);
    const type = document.getElementById("wType").value;

    // Сбор правил оповещений (выбираем только карточки правил, игнорируя текст-заглушку)
    const alertRules = [];
    document.querySelectorAll("#alertRulesContainer .alert-rule-card").forEach(div => {
        const cond = div.querySelector(".rCond").value;
        let val = 0, valMax = 0;
        
        if (cond === 'between') {
            val = parseFloat(div.querySelector(".rValMin").value) || 0;
            valMax = parseFloat(div.querySelector(".rValMax").value) || 0;
        } else {
            val = parseFloat(div.querySelector(".rVal").value) || 0;
        }

        alertRules.push({
            cond: cond,
            val: val,
            valMax: valMax,
            msg: div.querySelector(".rMsg").value.trim()
        });
    });

    const w = {
        label: document.getElementById("wLabel").value,
        type: type,
        topic: document.getElementById("wTopic").value.trim(),
        unit: type === "sensor" ? document.getElementById("wUnit").value.trim() : "",
        cmdTopic: type === "switch" ? document.getElementById("wCmdTopic").value.trim() : "",
        payloadOn: type === "switch" ? document.getElementById("wPayloadOn").value.trim() : "",
        payloadOff: type === "switch" ? document.getElementById("wPayloadOff").value.trim() : "",
        alerts: alertRules
    };

    if (editIndex >= 0) {
        activeWidgets[editIndex] = w; // Обновляем существующий
    } else {
        activeWidgets.push(w); // Добавляем новый
    }

    await saveWidgets();
    cancelEdit();
    alert("Сохранено!");
});

async function updateDashboard() {
    try {
        const res = await fetch("/api/status", { cache: "no-store" });
        if (!res.ok) return;
        const data = await res.json();
        
        document.getElementById("wifiRssi").textContent = data.rssi + " dBm";
        document.getElementById("freeHeap").textContent = Math.round(data.free_heap / 1024) + " КБ";
        setConnection("localMqtt", data.local_mqtt);
        setConnection("mainMqtt", data.main_mqtt);

        globalStates = {};
        const tbody = document.getElementById("mqttTable");
        tbody.replaceChildren();
        
        for (const st of data.states) {
            globalStates[st.topic] = st.payload;
            const row = document.createElement("tr");
            row.innerHTML = `<td>${st.topic}</td><td>${st.payload}</td><td>${st.age_seconds}с</td>`;
            tbody.appendChild(row);
        }

        renderDashboardWidgets();
    } catch (e) {
        console.error("Ошибка обновления статуса:", e);
    }
}

async function loadSettings() {
    try {
        const res = await fetch("/api/settings");
        const data = await res.json();
        document.getElementById("setSsid").value = data.ssid || "";
        document.getElementById("setPass").value = data.pass || "";
        document.getElementById("setMqttHost").value = data.host || "";
        document.getElementById("setMqttPort").value = data.port || 1883;
        document.getElementById("setMqttUser").value = data.user || "";
        document.getElementById("setMqttPass").value = data.pass_mqtt || "";
    } catch(e) {}
}

document.getElementById("settingsForm").addEventListener("submit", async function(e) {
    e.preventDefault();
    const res = document.getElementById("settingsResult");
    const body = new URLSearchParams({
        ssid: document.getElementById("setSsid").value, pass: document.getElementById("setPass").value,
        host: document.getElementById("setMqttHost").value, port: document.getElementById("setMqttPort").value,
        user: document.getElementById("setMqttUser").value, pass_mqtt: document.getElementById("setMqttPass").value
    });
    try {
        const response = await fetch("/api/settings", { method: "POST", body });
        res.textContent = await response.text();
        res.className = "ok";
    } catch(err) { res.textContent = "Ошибка: " + err; res.className = "error"; }
});

document.getElementById("commandForm").addEventListener(
    "submit",
    async function(e) {
        e.preventDefault();

        const result = document.getElementById("commandResult");

        const command = {
            topic: document.getElementById("commandTopic").value.trim(),
            payload: document.getElementById("commandPayload").value
        };

        try {
            const response = await fetch("/api/command", {
                method: "POST",
                headers: {
                    "Content-Type": "application/json"
                },
                body: JSON.stringify(command)
            });

            result.textContent = await response.text();
            result.className = response.ok ? "ok" : "error";
        } catch (error) {
            result.textContent = "Ошибка: " + error;
            result.className = "error";
        }
    }
);

// Запускаем загрузку последовательно при открытии страницы
async function initApp() {
    await loadSettings();
    await loadWidgets();
    await updateDashboard();
    await loadMaxSettings();
    setInterval(updateDashboard, 3000);
}

initApp();
</script>
</body>
</html>
)HTML";

void handleDashboard() {
    if (isConfigMode) {
        server.send_P(200, "text/html; charset=utf-8", SETUP_HTML);
    } else {
        server.send_P(200, "text/html; charset=utf-8", DASHBOARD_HTML);
    }
}

void handleStatus() {
    JsonDocument doc;
    doc["ip"] = WiFi.localIP().toString();
    doc["rssi"] = WiFi.RSSI();
    doc["free_heap"] = ESP.getFreeHeap();
    doc["local_mqtt"] = localMqttClient && localMqttClient->connected();
    doc["main_mqtt"] = mainMqttClient && mainMqttClient->connected();

    JsonArray states = doc["states"].to<JsonArray>();
    unsigned long now = millis();
    
    if (cachedStates != nullptr) {
        for (const auto& st : *cachedStates) {
            JsonObject item = states.add<JsonObject>();
            item["topic"] = st.topic;
            item["payload"] = st.payload;
            item["age_seconds"] = (now - st.updatedAt) / 1000UL;
        }
    }
    
    String response;
    serializeJson(doc, response);
    server.send(200, "application/json; charset=utf-8", response);
}

void handleGetWidgets() {
    server.send(200, "application/json; charset=utf-8", Settings::get().widgetsJson);
}

void handlePostWidgets() {
    if (server.hasArg("plain")) {
        Settings::saveWidgets(server.arg("plain"));
        server.send(200, "text/plain", "OK");
    } else {
        server.send(400, "text/plain", "Bad Request");
    }
}

void handleGetSettings() {
    SystemSettings s = Settings::get();
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
    if (!server.hasArg("ssid") || !server.hasArg("host")) {
        server.send(400, "text/plain", "Missing required fields");
        return;
    }
    SystemSettings s = Settings::get();
    s.wifiSsid = server.arg("ssid");
    s.wifiPassword = server.arg("pass");
    s.mqttHost = server.arg("host");
    s.mqttPort = server.arg("port").toInt();
    s.mqttUser = server.arg("user");
    s.mqttPassword = server.arg("pass_mqtt");

    Settings::save(s);
    server.send(200, "text/plain", "Настройки сохранены! Перезагрузка...");
    delay(1000);
    ESP.restart();
}

void handleCommand() {
    if (!server.hasArg("plain")) {
        server.send(400, "text/plain", "Missing body");
        return;
    }
    
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, server.arg("plain"));
    if (error) {
        server.send(400, "text/plain", "Invalid JSON body");
        return;
    }

    String topic = doc["topic"].as<String>();
    String payload = doc["payload"].as<String>();

    if (topic.length() == 0 || payload.length() == 0) {
        server.send(400, "text/plain", "Missing topic or payload");
        return;
    }

    if (!localMqttClient || !localMqttClient->connected()) {
        server.send(503, "text/plain", "Local broker not connected");
        return;
    }

    if (localMqttClient->publish(topic.c_str(), payload.c_str(), false)) {
        server.send(200, "text/plain", "Command published successfully");
    } else {
        server.send(500, "text/plain", "Failed to publish command");
    }
}

void handleGetMaxSettings() {
    SystemSettings s = Settings::get();
    JsonDocument doc;
    doc["token"] = s.maxToken;
    doc["userid"] = s.maxUserId;
    String response;
    serializeJson(doc, response);
    server.send(200, "application/json; charset=utf-8", response);
}

void handlePostMaxSettings() {
    if (!server.hasArg("token") || !server.hasArg("userid")) {
        server.send(400, "text/plain", "Missing fields");
        return;
    }
    Settings::saveMax(server.arg("token"), server.arg("userid"));
    server.send(200, "text/plain", "Настройки MAX успешно сохранены!");
}

void handleMaxTest() {
    bool ok = sendMaxNotification("Тестовое уведомление от шлюза ESP32!");
    if (ok) {
        server.send(200, "text/plain", "Тестовое сообщение успешно отправлено в MAX!");
    } else {
        server.send(500, "text/plain", "Ошибка отправки. Проверьте токен, User ID и интернет на шлюзе.");
    }
}

void handleHistory() {
    if (!server.hasArg("topic")) {
        server.send(400, "text/plain", "Missing topic");
        return;
    }
    String targetTopic = server.arg("topic");
    
    // Отсекаем путь JSON (например, #Temperature), чтобы найти базовый топик истории
    int hashIdx = targetTopic.indexOf('#');
    if (hashIdx >= 0) {
        targetTopic = targetTopic.substring(0, hashIdx);
        targetTopic.trim();
    }
    
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();

    if (cachedStates != nullptr) {
        for (const auto& st : *cachedStates) {
            if (strcmp(st.topic, targetTopic.c_str()) == 0) {
                for (const auto& pt : st.history) {
                    JsonObject obj = arr.add<JsonObject>();
                    obj["val"] = pt.value;
                    obj["time"] = pt.timestamp;
                }
                break;
            }
        }
    }

    String response;
    serializeJson(doc, response);
    server.send(200, "application/json; charset=utf-8", response);
}

} // namespace

namespace WebDashboard {
    void begin(PubSubClient& localMqtt, PubSubClient& mainMqtt, std::vector<CachedState>& states, bool apMode) {
        localMqttClient = &localMqtt;
        mainMqttClient = &mainMqtt;
        cachedStates = &states;
        isConfigMode = apMode;

        ElegantOTA.begin(&server);

        server.on("/", HTTP_GET, handleDashboard);
        server.on("/api/status", HTTP_GET, handleStatus);
        server.on("/api/widgets", HTTP_GET, handleGetWidgets);
        server.on("/api/widgets", HTTP_POST, handlePostWidgets);
        server.on("/api/settings", HTTP_GET, handleGetSettings);
        server.on("/api/settings", HTTP_POST, handlePostSettings);
        server.on("/api/command", HTTP_POST, handleCommand);
        server.on("/api/max-settings", HTTP_GET, handleGetMaxSettings);
        server.on("/api/max-settings", HTTP_POST, handlePostMaxSettings);
        server.on("/api/max-test", HTTP_POST, handleMaxTest);
        server.on("/api/history", HTTP_GET, handleHistory);
        server.begin();
    }

    void handleClient() {
        server.handleClient();
    }
}
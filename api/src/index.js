import "dotenv/config";
import express from "express";
import cors from "cors";
import mqtt from "mqtt";
import {
  insertTelemetry, createAlert, listDevices,
  deviceTelemetry, listAlerts, ackAlert, upsertDevice,
} from "./db.js";

const app = express();
app.use(cors());
app.use(express.json());

// ---------- Regras simples de alerta (v1; ML entra depois) ----------
const windows = new Map(); // device -> últimas magnitudes
async function evaluate(device, mag) {
  if (mag == null) return;
  const arr = windows.get(device) || [];
  arr.push(mag);
  if (arr.length > 60) arr.shift();
  windows.set(device, arr);
  if (arr.length < 30) return;
  const mean = arr.reduce((a, b) => a + b, 0) / arr.length;
  // Energia muito acima da média por janela cheia = possível cio/estresse
  if (mean > 1.8) {
    await createAlert(device, "ACTIVITY_BURST", `Energia média ${mean.toFixed(2)}g acima do limiar (possível cio/estresse)`);
    windows.set(device, []);
  }
}

// ---------- MQTT: databov/<device>/telemetry ----------
const client = mqtt.connect(process.env.MQTT_URL || "mqtt://localhost:1883");
client.on("connect", () => {
  console.log("[mqtt] conectado");
  client.subscribe("databov/+/telemetry", (err) => {
    if (err) console.error("[mqtt] subscribe:", err.message);
  });
  // Registros agregados de 1 min vindos da coleira v2 (firmware com MQTT)
  client.subscribe("databov/+/status", (err) => {
    if (err) console.error("[mqtt] subscribe status:", err.message);
  });
});

// Alerta de bem-estar baixo com intervalo mínimo de 1h por animal
const lastWelfareAlert = new Map();
async function welfareCheck(device, bemEstar, valido) {
  if (!valido || bemEstar > 2) return;
  const last = lastWelfareAlert.get(device) || 0;
  if (Date.now() - last < 3600000) return;
  lastWelfareAlert.set(device, Date.now());
  await createAlert(device, "WELFARE_LOW", `Índice de bem-estar ${bemEstar}/5 (triagem, verificar animal)`);
}
client.on("message", async (topic, payload) => {
  try {
    const parts = topic.split("/");
    const device = parts[1];
    const kind = parts[2];
    const data = JSON.parse(payload.toString());
    if (kind === "status") {
      // Agregado de 1 min da coleira v2: odba vira proxy de atividade
      await upsertDevice(device, data.colar || device);
      const t = { device, mag: data.odba ?? null, batt: data.batt ?? null };
      await insertTelemetry(t);
      await welfareCheck(device, data.bem_estar, data.bem_estar_valido);
      return;
    }
    const t = { device, ...data };
    if (t.ax != null && t.mag == null) {
      t.mag = Math.sqrt(t.ax ** 2 + (t.ay || 0) ** 2 + (t.az || 0) ** 2);
    }
    await insertTelemetry(t);
    await evaluate(device, t.mag);
  } catch (e) {
    console.error("[mqtt] msg:", e.message);
  }
});

// ---------- REST ----------
app.get("/api/health", (_req, res) => res.json({ ok: true, service: "databov" }));
app.get("/api/devices", async (_req, res) => res.json(await listDevices()));
app.get("/api/devices/:id/telemetry", async (req, res) => {
  const { from, to, limit } = req.query;
  res.json(await deviceTelemetry(req.params.id, from, to, Number(limit) || 500));
});
app.get("/api/alerts", async (req, res) => res.json(await listAlerts(req.query.open !== "0")));
app.post("/api/alerts/:id/ack", async (req, res) => {
  await ackAlert(req.params.id);
  res.json({ ok: true });
});

const PORT = process.env.PORT || 3001;
app.listen(PORT, () => console.log(`[api] http://localhost:${PORT}`));

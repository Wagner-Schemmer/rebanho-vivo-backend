import "dotenv/config";
import express from "express";
import cors from "cors";
import mqtt from "mqtt";
import {
  insertTelemetry, createAlert, listDevices,
  deviceTelemetry, listAlerts, ackAlert,
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
});
client.on("message", async (topic, payload) => {
  try {
    const device = topic.split("/")[1];
    const data = JSON.parse(payload.toString());
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

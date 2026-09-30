# DataBov — Backend da coleira

MQTT + TimescaleDB + API REST + firmware ESP8266. Roda no gateway da propriedade (Raspberry Pi, mini-PC ou VPS).

```
databov-backend/
├── docker-compose.yml   # mosquitto + timescaledb + api
├── mosquitto/           # config do broker
├── db/init.sql          # devices, telemetry (hypertable), alerts
├── api/                 # ingestão MQTT + REST + regras de alerta v1
└── firmware/coleira/    # ESP8266 + MPU6050 -> MQTT
```

## Subir (1 comando no gateway)
```bash
cp .env.example .env   # ajuste DB_PASSWORD
docker compose up -d --build
docker compose logs -f api
```

## Tópicos MQTT
- `databov/<device>/telemetry` → `{"mag":1.2,"ax":..,"ay":..,"az":..,"gx":..,"gy":..,"gz":..,"batt":3.9,"ts":"2026-.."}`
- `mag` é calculada na API se ausente.

## REST
- `GET /api/health`
- `GET /api/devices` (com último status)
- `GET /api/devices/:id/telemetry?from=&to=&limit=`
- `GET /api/alerts?open=1` · `POST /api/alerts/:id/ack`

## Firmware
Abra `firmware/coleira/coleira.ino` no Arduino IDE, instale PubSubClient + ArduinoJson,
ajuste WIFI/MQTT/DEVICE_ID e grave no Wemos D1 Mini. Fiação no topo do arquivo.

## Próximos passos (roadmap)
- Trocar regra de limiar por Random Forest treinado com dados rotulados
- RTLS via beacons BLE (confinamento) em vez de GPS
- Rádio BLE final no lugar do Wi-Fi

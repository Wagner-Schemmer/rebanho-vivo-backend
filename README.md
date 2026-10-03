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
- `databov/<device>/status` → agregado de 1 min da coleira v2 (odba, bem_estar, hr_max, spo2…); cria o device sozinho e gera alerta `WELFARE_LOW` se índice ≤ 2 (máx 1/h por animal).

## REST
- `GET /api/health`
- `GET /api/devices` (com último status)
- `GET /api/devices/:id/telemetry?from=&to=&limit=`
- `GET /api/alerts?open=1` · `POST /api/alerts/:id/ack`

## Firmware
Abra `firmware/coleira/coleira.ino` no Arduino IDE, instale e grave no Wemos D1 Mini. Fiação no topo do arquivo.
Bibliotecas: **SparkFun MAX3010x** + **PubSubClient** (Library Manager).
Compile verificado com arduino-cli (ESP8266 core 3.1.2, RAM 55%, flash 25%).
Fiação: MPU `3V3/VCC G/GND D1/SCL D2/SDA AD0/GND` · MAX30102 `3V3/VIN G/GND D1/SCL D2/SDA` · PulseSensor opcional `3V3/VCC G/GND A0/S` (`USE_PULSE=1`).
Configure `WIFI_SSID`, `WIFI_PASS`, `MQTT_HOST` (broker) e `COLLAR_ID` no topo. Para MQTT local: `docker compose up -d --build` e aponte o MQTT_HOST ao IP do gateway.

## Próximos passos (roadmap)
- Trocar regra de limiar por Random Forest treinado com dados rotulados
- RTLS via beacons BLE (confinamento) em vez de GPS
- Rádio BLE final no lugar do Wi-Fi

```console
$ ssh gateway@fazenda
gateway:~$ git clone https://github.com/Wagner-Schemmer/rebanho-vivo-backend.git databov && cd databov
gateway:~/databov$ cp .env.example .env   # ajustar DB_PASSWORD
gateway:~/databov$ docker compose up -d --build
 ✔ mosquitto   Pulled + Up
 ✔ timescaledb Up (hypertable: telemetry)
 ✔ api         Listening on :3000
gateway:~/databov$ mosquitto_sub -t 'databov/#' -v
databov/C-0001/telemetry {"mag":1.2,"ax":0.1,"batt":3.9,"ts":"2026-10-10T…"}
databov/C-0001/status    {"odba":0.4,"bem_estar":4.6,"alertas":[]}
gateway:~/databov$ curl localhost:3000/api/health
{"ok":true}
```

# backend-da-coleira

MQTT + TimescaleDB + API REST + firmware ESP8266. Do pasto ao banco em 1 comando.

## Mapa da rede

```
[C-0001 · Wemos+MPU6050+MAX30102] ──MQTT──▶ [mosquitto] ──▶ [api] ──▶ [timescaledb]
                                                            │──▶ WELFARE_LOW (índice ≤ 2, máx 1/h)
                                                            └──▶ REST: /api/devices /api/telemetry /api/alerts
```

## Gravar o firmware

`firmware/coleira/coleira.ino` → Arduino IDE → Wemos D1 Mini. Libs: **SparkFun MAX3010x** + **PubSubClient**. Fiação e `WIFI_SSID`/`MQTT_HOST`/`COLLAR_ID` no topo do arquivo. (Compilação verificada: ESP8266 core 3.1.2.)

## Arquivos

`docker-compose.yml` · `mosquitto/` · `db/init.sql` · `api/` · `firmware/coleira/`

## Próximos pacotes (roadmap)

Random Forest no lugar do limiar · RTLS BLE em vez de GPS · rádio BLE final em vez de Wi-Fi.

---
`logout` — [Wagner Schemmer](https://wagner-port.vercel.app) · site: [databov-v2.vercel.app](https://databov-v2.vercel.app)

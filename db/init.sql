-- RebanhoVivo — schema inicial (TimescaleDB)
CREATE TABLE IF NOT EXISTS devices (
  id TEXT PRIMARY KEY,
  name TEXT NOT NULL,
  birth_date DATE,
  created_at TIMESTAMPTZ DEFAULT now()
);

CREATE TABLE IF NOT EXISTS telemetry (
  time TIMESTAMPTZ NOT NULL,
  device_id TEXT NOT NULL REFERENCES devices(id),
  ax REAL, ay REAL, az REAL,
  gx REAL, gy REAL, gz REAL,
  mag REAL,
  batt REAL
);
SELECT create_hypertable('telemetry', 'time', if_not_exists => TRUE);

CREATE TABLE IF NOT EXISTS alerts (
  id BIGSERIAL PRIMARY KEY,
  time TIMESTAMPTZ DEFAULT now(),
  device_id TEXT NOT NULL REFERENCES devices(id),
  type TEXT NOT NULL,
  message TEXT NOT NULL,
  acknowledged BOOLEAN DEFAULT FALSE
);

CREATE INDEX IF NOT EXISTS idx_telemetry_device_time ON telemetry (device_id, time DESC);

-- Último status por animal
CREATE OR REPLACE VIEW latest_status AS
SELECT DISTINCT ON (device_id)
  device_id, time, mag, batt
FROM telemetry
ORDER BY device_id, time DESC;

-- Dispositivo de exemplo (trocar pelos reais)
INSERT INTO devices (id, name) VALUES ('COL-001', 'Mimosa')
ON CONFLICT (id) DO NOTHING;

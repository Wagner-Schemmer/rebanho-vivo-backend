import pg from "pg";

const pool = new pg.Pool({
  connectionString: process.env.DATABASE_URL,
  max: 10,
});

pool.on("error", (err) => console.error("[db] pool error:", err.message));

export async function upsertDevice(id, name) {
  await pool.query(
    `INSERT INTO devices (id, name) VALUES ($1, $2)
     ON CONFLICT (id) DO NOTHING`,
    [id, name || id]
  );
}

export async function insertTelemetry(t) {
  await pool.query(
    `INSERT INTO telemetry (time, device_id, ax, ay, az, gx, gy, gz, mag, batt)
     VALUES (COALESCE($1, now()), $2, $3, $4, $5, $6, $7, $8, $9, $10)`,
    [t.ts || null, t.device, t.ax, t.ay, t.az, t.gx, t.gy, t.gz, t.mag, t.batt]
  );
}

export async function createAlert(device, type, message) {
  await pool.query(
    `INSERT INTO alerts (device_id, type, message) VALUES ($1, $2, $3)`,
    [device, type, message]
  );
}

export async function listDevices() {
  const { rows } = await pool.query(
    `SELECT d.*, s.time AS last_seen, s.mag AS last_mag, s.batt AS last_batt
     FROM devices d LEFT JOIN latest_status s ON s.device_id = d.id
     ORDER BY d.id`
  );
  return rows;
}

export async function deviceTelemetry(id, from, to, limit = 500) {
  const { rows } = await pool.query(
    `SELECT time, ax, ay, az, gx, gy, gz, mag, batt FROM telemetry
     WHERE device_id = $1
       AND ($2::timestamptz IS NULL OR time >= $2)
       AND ($3::timestamptz IS NULL OR time <= $3)
     ORDER BY time DESC LIMIT $4`,
    [id, from || null, to || null, Math.min(limit, 5000)]
  );
  return rows;
}

export async function listAlerts(onlyOpen = true) {
  const { rows } = await pool.query(
    `SELECT * FROM alerts ${onlyOpen ? "WHERE acknowledged = FALSE" : ""}
     ORDER BY time DESC LIMIT 100`
  );
  return rows;
}

export async function ackAlert(id) {
  await pool.query(`UPDATE alerts SET acknowledged = TRUE WHERE id = $1`, [id]);
}

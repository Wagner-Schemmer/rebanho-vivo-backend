// DataBov — coleira.ino v2
// Wemos D1 mini (ESP8266) + MPU6050 (GY-521) + MAX30102 [+ PulseSensor opcional].
//
// O QUE FAZ (herdado da v1 + novo)
//   - Amostra MPU6050 a 100 Hz com Wi-Fi DESLIGADO entre envios.
//   - A cada 3 s classifica ocio/ruminando/alimentando/caminhando/agitada,
//     detecta decúbito e estima HR/RR por acelerômetro (só quando quieto).
//   - NOVO: MAX30102 (I2C 0x57, mesmo barramento do MPU) mede HR real por
//     fotopletismografia + estimativa de SpO2. Tem prioridade sobre o HR
//     do acelerômetro quando o sinal é válido.
//   - NOVO (opcional): PulseSensor analógico em A0 como 3ª fonte de HR.
//     Desligado por padrão: A0 é o único pino analógico (conflita com
//     leitura de bateria — escolha um dos dois).
//   - Índice de bem-estar 1-5 (média móvel ~2 h), igual à v1.
//   - A cada 60 s fecha registro agregado e publica em
//     databov/<COLLAR_ID>/status via MQTT (+ HTTP legado p/ CSV, opcional).
//
// LIGAÇÃO (D1 mini)
//   MPU6050:     3V3->VCC  G->GND  D1->SCL  D2->SDA  AD0->GND
//   MAX30102:    3V3->VIN* G->GND  D1->SCL  D2->SDA   (*na bancada USB pode usar 5V)
//   PulseSensor: 3V3->VCC(+) G->GND(-) A0->Sinal  (só se USE_PULSE=1)
//
// BIBLIOTECAS (Arduino IDE > Library Manager ou arduino-cli lib install)
//   - SparkFun MAX3010x Pulse and Proximity Sensor Library
//   - PubSubClient
//
// SOBRE HR/RR: acelerômetro no pescoço é fraco — vale só quieto. MAX30102
// precisa de contato com pele (orelha/úbere/tosa); sem sinal, reporta 0
// ("sem medida"). SpO2 é estimativa (curva empírica), não dado clínico.
// Índice de bem-estar: triagem, não diagnóstico (ver doc da v1).
// Limiares vêm de modelo — recalibrar com observação de campo.

#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <Wire.h>
#include <MAX30105.h>
#include <PubSubClient.h>

// ===================== CONFIGURAÇÃO =====================
const char* WIFI_SSID  = "SUA_REDE";
const char* WIFI_PASS  = "SUA_SENHA";
// NOTA: ESP8266 NÃO suporta rede corporativa (PEAP, ex: IFFar-estudantes).
// Use Wi-Fi doméstico ou hotspot do celular (WPA2 comum).
const char* SERVER_URL = "http://192.168.0.10:5000/telemetria"; // legado CSV (USE_HTTP=1)
const char* COLLAR_ID  = "C-0001";

const char* MQTT_HOST = "IP_DO_GATEWAY"; // broker do backend DataBov
const int   MQTT_PORT = 1883;

// Supabase (dashboard ao vivo no site): host sem https://, chave anon (pública)
const char* SUPA_HOST = "bjzyrzpilrvolycbdeyk.supabase.co";
const char* SUPA_KEY  = "sb_publishable_9Vr4s6HGBILeHTe8qHRh-A_RMtgE1hd";
#define USE_SUPABASE 1   // 1 = espelha cada registro em /leituras (HTTPS)

#define USE_MAX30102 1   // 1 = HR real + SpO2 via MAX30102
#define USE_PULSE    0   // 1 = PulseSensor em A0 (desliga leitura de bateria)
#define USE_HTTP     1   // 1 = mantém POST legado p/ telemetria.csv
#define USE_MQTT     1   // 1 = publica agregado em databov/<id>/status

const int LOTE_ALVO = 1; // MODO DEMO: envia a cada registro (campo: 5)
const unsigned long WIFI_TIMEOUT_MS = 8000;

const float ODBA_AGITADA     = 0.45f;
const float ODBA_CAMINHANDO  = 0.16f;
const float ODBA_ALIMENTANDO = 0.07f;
const float ODBA_QUIETO      = 0.07f;
const float FREQ_RUM_MIN     = 0.7f;
const float FREQ_RUM_MAX     = 1.7f;
const float PITCH_DEITADA    = -25.0f;

const float GYRO_AGITADA     = 120.0f;
const float GYRO_ALIMENTANDO = 25.0f;
const float GYRO_QUIETO      = 15.0f;

const float DEITADA_MIN_OK = 0.40f, DEITADA_MAX_OK = 0.60f;
const float RUMINA_MIN_OK  = 0.30f, RUMINA_MAX_OK  = 0.40f;
const float AGITADA_MAX_OK = 0.05f;
const float EMA_ALPHA_BEMESTAR = 1.0f / 120.0f;

#define DEBUG_SERIAL 1
// ========================================================

const uint8_t MPU_ADDR = 0x68;
const float ACCEL_SENS = 16384.0f;
const float GYRO_SENS  = 65.5f;
const float FS = 100.0f;
const unsigned long PERIODO_US = 1000000UL / 100UL;

const int WIN_S = 3;
const int WIN = 300;
const int JANELAS_POR_REGISTRO = 5; // MODO DEMO: 5 x 3 s = 15 s por registro (campo: 20)
const int FILA_MAX = 30;

enum Estado : uint8_t { OCIO = 0, RUMINANDO, ALIMENTANDO, CAMINHANDO, AGITADA, N_ESTADOS };
const char* NOME_ESTADO[N_ESTADOS] = {"ocio", "ruminando", "alimentando", "caminhando", "agitada"};

struct BandPass {
  float c0, c1, c2, d1, d2;
  float z1 = 0, z2 = 0;
  void configurar(float f0, float larguraHz, float fs) {
    float w0 = 2.0f * PI * f0 / fs;
    float Q = f0 / larguraHz;
    float alpha = sinf(w0) / (2.0f * Q);
    float a0 = 1.0f + alpha;
    c0 = alpha / a0; c1 = 0.0f; c2 = -alpha / a0;
    d1 = (-2.0f * cosf(w0)) / a0; d2 = (1.0f - alpha) / a0;
  }
  float processar(float x) {
    float y = c0 * x + z1;
    z1 = c1 * x - d1 * y + z2;
    z2 = c2 * x - d2 * y;
    return y;
  }
};

struct DetectorPico {
  float topo = 0, fundo = 0, alphaEnv = 0.05f, fracao = 0.5f;
  unsigned long refratarioMs = 300, ultimoPicoMs = 0;
  bool acima = false;
  static const int N = 5;
  float buf[N] = {0};
  int idx = 0;
  bool cheio = false;
  void iniciar(float alpha, float frac, unsigned long refratario) {
    alphaEnv = alpha; fracao = frac; refratarioMs = refratario;
  }
  void atualizar(float x, unsigned long agoraMs) {
    if (x > topo) topo = x; else topo += alphaEnv * (x - topo);
    if (x < fundo) fundo = x; else fundo += alphaEnv * (x - fundo);
    float limiar = fundo + fracao * (topo - fundo);
    if (x > limiar && !acima && (agoraMs - ultimoPicoMs) > refratarioMs) {
      acima = true;
      if (ultimoPicoMs > 0) {
        buf[idx] = 60000.0f / (float)(agoraMs - ultimoPicoMs);
        idx = (idx + 1) % N;
        if (idx == 0) cheio = true;
      }
      ultimoPicoMs = agoraMs;
    } else if (x < limiar) {
      acima = false;
    }
  }
  float taxa() const {
    int n = cheio ? N : idx;
    if (n == 0) return 0;
    float s = 0;
    for (int i = 0; i < n; i++) s += buf[i];
    return s / n;
  }
  bool fresco(unsigned long agoraMs, unsigned long maxIdadeMs) const {
    return cheio && ultimoPicoMs > 0 && (agoraMs - ultimoPicoMs) < maxIdadeMs;
  }
};

struct Registro {
  uint32_t seq;
  uint32_t t_s;
  uint16_t seg[N_ESTADOS];
  uint16_t deitada_s;
  float odba;
  float gyro;
  float hr;  uint8_t hrN;
  float rr;  uint8_t rrN;
  uint8_t hrMax;    // HR real (MAX30102), 0 = sem medida
  uint8_t spo2;     // estimativa %, 0 = sem medida
  uint8_t hrPulso;  // PulseSensor, 0 = sem medida/desligado
  uint8_t bemEstar;
  bool bemEstarValido;
};

Registro fila[FILA_MAX];
int filaN = 0;
int alvoLote = LOTE_ALVO;
uint32_t seqGlobal = 0;
Registro ultimoFechado;
bool temUltimo = false;

uint16_t acSeg[N_ESTADOS];
uint16_t acDeitada = 0;
float acOdba = 0, acGyro = 0;
int acJanelas = 0;
float acHr = 0, acRr = 0;
uint8_t acHrN = 0, acRrN = 0;
uint8_t acHrMax = 0;
uint32_t acSpo2 = 0; uint8_t acSpo2N = 0;
uint8_t acHrPulso = 0;

float emaDeitada = -1, emaRumina = -1, emaAgitada = -1;
uint16_t registrosAcumulados = 0;
const uint16_t REGISTROS_PARA_VALIDAR = 20;

float wax[WIN], way[WIN], waz[WIN], wmag[WIN], wgyro[WIN];
int wi = 0;

BandPass filtroCard, filtroResp;
DetectorPico detCard, detResp, detPulso;
float dcMag = 0, emaMag = 0;

// quietude instantânea (p/ sensores ópticos)
float emaDynQ = 0, emaGyroQ = 0;

unsigned long ultimaAmostraUs = 0;

// ---------- MAX30102 ----------
#if USE_MAX30102
MAX30105 maxSensor;
bool temMax = false;
const int MAXN = 800;               // ~8 s a 100 Hz
uint32_t maxIR[MAXN], maxRed[MAXN];
int maxIdx = 0;
bool maxCheio = false;
DetectorPico detMax;
#endif

WiFiClient espClient;
PubSubClient mqttCli(espClient);

void escreverReg(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission(true);
}

bool mpuIniciar() {
  Wire.begin(D2, D1);
  Wire.setClock(400000);
  Wire.beginTransmission(MPU_ADDR);
  if (Wire.endTransmission() != 0) return false;
  escreverReg(0x6B, 0x00);
  escreverReg(0x1A, 0x03);
  escreverReg(0x1C, 0x00);
  escreverReg(0x1B, 0x08);
  return true;
}

bool mpuLer(float &ax, float &ay, float &az, float &gx, float &gy, float &gz) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x3B);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(MPU_ADDR, (size_t)14, true) != 14) return false;
  int16_t x = (Wire.read() << 8) | Wire.read();
  int16_t y = (Wire.read() << 8) | Wire.read();
  int16_t z = (Wire.read() << 8) | Wire.read();
  Wire.read(); Wire.read();
  int16_t rx = (Wire.read() << 8) | Wire.read();
  int16_t ry = (Wire.read() << 8) | Wire.read();
  int16_t rz = (Wire.read() << 8) | Wire.read();
  ax = x / ACCEL_SENS; ay = y / ACCEL_SENS; az = z / ACCEL_SENS;
  gx = rx / GYRO_SENS; gy = ry / GYRO_SENS; gz = rz / GYRO_SENS;
  return true;
}

Estado classificar(float odba, float freq, float gyro) {
  if (odba > ODBA_AGITADA || gyro > GYRO_AGITADA) return AGITADA;
  if (odba > ODBA_CAMINHANDO) return CAMINHANDO;
  if (odba > ODBA_ALIMENTANDO || gyro > GYRO_ALIMENTANDO) return ALIMENTANDO;
  if (freq > FREQ_RUM_MIN && freq < FREQ_RUM_MAX) return RUMINANDO;
  return OCIO;
}

void zerarAcumuladores() {
  for (int i = 0; i < N_ESTADOS; i++) acSeg[i] = 0;
  acDeitada = 0; acOdba = 0; acGyro = 0; acJanelas = 0;
  acHr = 0; acRr = 0; acHrN = 0; acRrN = 0;
  acHrMax = 0; acSpo2 = 0; acSpo2N = 0; acHrPulso = 0;
}

uint8_t calcularBemEstar(float fDeitada, float fRumina, float fAgitada) {
  int desvios = 0;
  float penalidade = 0;
  if (fDeitada < DEITADA_MIN_OK || fDeitada > DEITADA_MAX_OK) {
    desvios++;
    float d = (fDeitada < DEITADA_MIN_OK) ? (DEITADA_MIN_OK - fDeitada) : (fDeitada - DEITADA_MAX_OK);
    penalidade += 1.0f + fminf(2.0f, d * 6.0f);
  }
  if (fRumina < RUMINA_MIN_OK || fRumina > RUMINA_MAX_OK) {
    desvios++;
    float d = (fRumina < RUMINA_MIN_OK) ? (RUMINA_MIN_OK - fRumina) : (fRumina - RUMINA_MAX_OK);
    penalidade += 1.0f + fminf(2.0f, d * 6.0f);
  }
  if (fAgitada > AGITADA_MAX_OK) {
    desvios++;
    penalidade += 1.0f + fminf(2.0f, (fAgitada - AGITADA_MAX_OK) * 10.0f);
  }
  if (desvios >= 2) penalidade += 1.0f;
  int nota = (int)roundf(5.0f - penalidade);
  if (nota < 1) nota = 1;
  if (nota > 5) nota = 5;
  return (uint8_t)nota;
}

void fecharRegistro() {
  Registro r;
  r.seq = seqGlobal++;
  r.t_s = millis() / 1000;
  for (int i = 0; i < N_ESTADOS; i++) r.seg[i] = acSeg[i];
  r.deitada_s = acDeitada;
  r.odba = acJanelas ? acOdba / acJanelas : 0;
  r.gyro = acJanelas ? acGyro / acJanelas : 0;
  r.hrN = acHrN; r.rrN = acRrN;
  r.hr = (acHrN >= 3) ? acHr / acHrN : 0;
  r.rr = (acRrN >= 3) ? acRr / acRrN : 0;
  r.hrMax = acHrMax; r.hrPulso = acHrPulso;
  r.spo2 = acSpo2N ? (uint8_t)(acSpo2 / acSpo2N) : 0;

  float fDeitada = acDeitada / (float)(WIN_S * JANELAS_POR_REGISTRO);
  float fRumina  = acSeg[RUMINANDO] / (float)(WIN_S * JANELAS_POR_REGISTRO);
  float fAgitada = acSeg[AGITADA]   / (float)(WIN_S * JANELAS_POR_REGISTRO);

  if (emaDeitada < 0) { emaDeitada = fDeitada; emaRumina = fRumina; emaAgitada = fAgitada; }
  else {
    emaDeitada += EMA_ALPHA_BEMESTAR * (fDeitada - emaDeitada);
    emaRumina  += EMA_ALPHA_BEMESTAR * (fRumina  - emaRumina);
    emaAgitada += EMA_ALPHA_BEMESTAR * (fAgitada - emaAgitada);
  }
  if (registrosAcumulados < REGISTROS_PARA_VALIDAR) registrosAcumulados++;

  r.bemEstarValido = (registrosAcumulados >= REGISTROS_PARA_VALIDAR);
  r.bemEstar = calcularBemEstar(emaDeitada, emaRumina, emaAgitada);

  ultimoFechado = r; temUltimo = true;
  if (filaN == FILA_MAX) {
    memmove(&fila[0], &fila[1], sizeof(Registro) * (FILA_MAX - 1));
    filaN--;
  }
  fila[filaN++] = r;
  zerarAcumuladores();
}

void processarJanela() {
  float mx = 0, my = 0, mz = 0, mm = 0, mg = 0;
  for (int i = 0; i < WIN; i++) { mx += wax[i]; my += way[i]; mz += waz[i]; mm += wmag[i]; mg += wgyro[i]; }
  mx /= WIN; my /= WIN; mz /= WIN; mm /= WIN; mg /= WIN;

  float odba = 0, var = 0;
  for (int i = 0; i < WIN; i++) {
    odba += fabsf(wax[i] - mx) + fabsf(way[i] - my) + fabsf(waz[i] - mz);
    float d = wmag[i] - mm;
    var += d * d;
  }
  odba /= WIN;
  float sd = sqrtf(var / WIN);

  float h = fmaxf(0.004f, 0.3f * sd);
  bool baixo = false;
  int cruz = 0;
  for (int i = 0; i < WIN; i++) {
    float d = wmag[i] - mm;
    if (d > h) { if (baixo) { cruz++; baixo = false; } }
    else if (d < -h) baixo = true;
  }
  float freq = (float)cruz / WIN_S;

  float pitch = atan2f(mx, sqrtf(my * my + mz * mz)) * 180.0f / PI;
  Estado e = classificar(odba, freq, mg);

  acSeg[e] += WIN_S;
  if (pitch < PITCH_DEITADA) acDeitada += WIN_S;
  acOdba += odba;
  acGyro += mg;
  acJanelas++;

  unsigned long agora = millis();
  if (odba < ODBA_QUIETO && mg < GYRO_QUIETO) {
    if (detCard.fresco(agora, 4000)) {
      float hr = detCard.taxa();
      if (hr >= 35 && hr <= 130) { acHr += hr; acHrN++; }
    }
    if (detResp.fresco(agora, 10000)) {
      float rr = detResp.taxa();
      if (rr >= 8 && rr <= 70) { acRr += rr; acRrN++; }
    }
  }

#if DEBUG_SERIAL
  Serial.printf("[jan] odba=%.3f f=%.2fHz giro=%.0f pitch=%.0f estado=%s%s hrMax=%u spo2=%u\n",
                odba, freq, mg, pitch, NOME_ESTADO[e], pitch < PITCH_DEITADA ? " (deitada)" : "",
                acHrMax, acSpo2N ? acSpo2 / acSpo2N : 0);
#endif

  if (acJanelas >= JANELAS_POR_REGISTRO) fecharRegistro();
}

#if USE_MAX30102
// HR por PPG: picos do infravermelho; SpO2 por razão das razões (curva empírica).
void processarMax() {
  unsigned long agora = millis();
  float dcIr = 0, dcRed = 0;
  for (int i = 0; i < MAXN; i++) { dcIr += maxIR[i]; dcRed += maxRed[i]; }
  dcIr /= MAXN; dcRed /= MAXN;
  if (dcIr < 5000) return; // sem dedo/orelha no sensor
  bool quieto = (emaDynQ < 0.05f && emaGyroQ < GYRO_QUIETO);
  if (!quieto) return;
  float acIr = 0, acRed = 0;
  for (int i = 0; i < MAXN; i++) {
    acIr += fabsf((float)maxIR[i] - dcIr);
    acRed += fabsf((float)maxRed[i] - dcRed);
  }
  acIr /= MAXN; acRed /= MAXN;
  if (acIr < 20) return; // sinal fraco demais
  float R = (acRed / dcRed) / (acIr / dcIr);
  float spo2 = -45.06f * R * R + 30.354f * R + 94.845f;
  if (spo2 < 70) spo2 = 70; if (spo2 > 100) spo2 = 100;
  // HR: taxa de picos recentes do buffer (detMax alimentado a cada amostra)
  if (detMax.fresco(agora, 6000)) {
    float hr = detMax.taxa();
    if (hr >= 25 && hr <= 220) {
      if (hr > acHrMax) acHrMax = (uint8_t)hr;
      acSpo2 += (uint32_t)spo2; acSpo2N++;
    }
  }
}
#endif

bool wifiOn() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < WIFI_TIMEOUT_MS) delay(50);
  return WiFi.status() == WL_CONNECTED;
}

void wifiOff() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

#if USE_SUPABASE
// Espelha os registros da fila em /leituras (melhor esforço: não trava a fila).
void enviarSupabase() {
  if (filaN == 0) return;
  WiFiClientSecure tls;
  tls.setInsecure();
  tls.setTimeout(4000);
  bool algumOk = false;
  for (int i = 0; i < filaN; i++) {
    const Registro &r = fila[i];
    char corpo[260];
    snprintf(corpo, sizeof(corpo),
      "{\"device\":\"%s\",\"odba\":%.3f,\"hr_max\":%u,\"spo2\":%u,\"bem_estar\":%u,"
      "\"seg_ocioso\":%u,\"seg_ruminando\":%u,\"seg_alimentando\":%u,"
      "\"seg_caminhando\":%u,\"seg_agitada\":%u}",
      COLLAR_ID, r.odba, r.hrMax, r.spo2, r.bemEstar,
      r.seg[0], r.seg[1], r.seg[2], r.seg[3], r.seg[4]);
    if (!tls.connect(SUPA_HOST, 443)) break;
    String req = String("POST /rest/v1/leituras HTTP/1.1\r\nHost: ") + SUPA_HOST +
      "\r\napikey: " + SUPA_KEY + "\r\nAuthorization: Bearer " + SUPA_KEY +
      "\r\nContent-Type: application/json\r\nPrefer: return=minimal\r\nContent-Length: " +
      String(strlen(corpo)) + "\r\nConnection: close\r\n\r\n" + corpo;
    tls.print(req);
    unsigned long t0 = millis();
    String resp;
    while (millis() - t0 < 4000 && tls.connected()) {
      while (tls.available()) resp += (char)tls.read();
      if (resp.indexOf("201 Created") >= 0) break;
      yield();
    }
    tls.stop();
    if (resp.indexOf("201") >= 0) algumOk = true;
    else {
#if DEBUG_SERIAL
      Serial.println("[supa] falha em um registro");
#endif
    }
  }
  if (algumOk) {
#if DEBUG_SERIAL
    Serial.println("[supa] espelhado");
#endif
  }
}
#endif

bool enviarLote() {
  bool ok = false;
#if USE_HTTP
  if (wifiOn()) {
    String js;
    js.reserve(160 + filaN * 260);
    js += "{\"colar\":\""; js += COLLAR_ID;
    js += "\",\"uptime_envio_s\":"; js += String(millis() / 1000);
    js += ",\"rssi\":"; js += String(WiFi.RSSI());
    js += ",\"registros\":[";
    char tmp[300];
    for (int i = 0; i < filaN; i++) {
      const Registro &r = fila[i];
      snprintf(tmp, sizeof(tmp),
        "%s{\"seq\":%lu,\"t_s\":%lu,\"seg\":[%u,%u,%u,%u,%u],\"deitada_s\":%u,"
        "\"odba\":%.3f,\"gyro\":%.1f,\"hr\":%.1f,\"hr_n\":%u,\"rr\":%.1f,\"rr_n\":%u,"
        "\"hr_max\":%u,\"spo2\":%u,\"hr_pulso\":%u,"
        "\"bem_estar\":%u,\"bem_estar_valido\":%s}",
        i ? "," : "", (unsigned long)r.seq, (unsigned long)r.t_s,
        r.seg[0], r.seg[1], r.seg[2], r.seg[3], r.seg[4], r.deitada_s,
        r.odba, r.gyro, r.hr, r.hrN, r.rr, r.rrN,
        r.hrMax, r.spo2, r.hrPulso,
        r.bemEstar, r.bemEstarValido ? "true" : "false");
      js += tmp;
    }
    js += "]}";
    WiFiClient cli;
    HTTPClient http;
    http.setTimeout(4000);
    if (http.begin(cli, SERVER_URL)) {
      http.addHeader("Content-Type", "application/json");
      int code = http.POST(js);
      ok = (code >= 200 && code < 300);
#if DEBUG_SERIAL
      Serial.printf("[envio] %d registros -> HTTP %d\n", filaN, code);
#endif
      http.end();
    }
  } else {
#if DEBUG_SERIAL
    Serial.println("[envio] sem Wi-Fi");
#endif
  }
#endif

#if USE_MQTT
  // Publica o último registro fechado no formato do backend DataBov.
  // Independe do HTTP: usa a mesma janela de Wi-Fi ligado.
  if (temUltimo && WiFi.status() != WL_CONNECTED) wifiOn();
  if (temUltimo && WiFi.status() == WL_CONNECTED) {
    mqttCli.setServer(MQTT_HOST, MQTT_PORT);
    mqttCli.setBufferSize(512);
    char clientId[32];
    snprintf(clientId, sizeof(clientId), "databov-%s", COLLAR_ID);
    if (mqttCli.connect(clientId)) {
      const Registro &r = ultimoFechado;
      char topic[64], pay[400];
      snprintf(topic, sizeof(topic), "databov/%s/status", COLLAR_ID);
      snprintf(pay, sizeof(pay),
        "{\"ts\":%lu,\"seg\":[%u,%u,%u,%u,%u],\"deitada_s\":%u,"
        "\"odba\":%.3f,\"gyro\":%.1f,\"hr\":%.1f,\"hr_n\":%u,\"rr\":%.1f,\"rr_n\":%u,"
        "\"hr_max\":%u,\"spo2\":%u,\"hr_pulso\":%u,\"mag\":%.3f,"
        "\"bem_estar\":%u,\"bem_estar_valido\":%s}",
        (unsigned long)r.t_s,
        r.seg[0], r.seg[1], r.seg[2], r.seg[3], r.seg[4], r.deitada_s,
        r.odba, r.gyro, r.hr, r.hrN, r.rr, r.rrN,
        r.hrMax, r.spo2, r.hrPulso, r.odba,
        r.bemEstar, r.bemEstarValido ? "true" : "false");
      if (mqttCli.publish(topic, pay)) {
        ok = true;
#if DEBUG_SERIAL
        Serial.printf("[mqtt] %s publicado\n", topic);
#endif
      }
      mqttCli.disconnect();
    }
#if DEBUG_SERIAL
    else Serial.println("[mqtt] broker inalcançável");
#endif
  }
#endif

#if USE_SUPABASE
  // Mesma janela de Wi-Fi: espelha a fila no Supabase (não conta p/ limpar a fila)
  if (WiFi.status() != WL_CONNECTED) wifiOn();
  if (WiFi.status() == WL_CONNECTED) enviarSupabase();
#endif

  wifiOff();
  return ok;
}

void tentarEnviar() {
  if (filaN < alvoLote) return;
  if (enviarLote()) {
    filaN = 0;
    alvoLote = LOTE_ALVO;
  } else {
    alvoLote = filaN + LOTE_ALVO;
  }
  ultimaAmostraUs = micros();
  wi = 0;
}

void setup() {
  Serial.begin(115200);
  delay(200);
  WiFi.persistent(false);
  WiFi.mode(WIFI_OFF);

  if (!mpuIniciar()) {
    Serial.println("MPU6050 nao encontrado (0x68). Confira SDA=D2, SCL=D1, AD0=GND.");
    while (true) delay(1000);
  }

  filtroCard.configurar(1.1f, 0.6f, FS);
  filtroResp.configurar(0.4f, 0.4f, FS);
  detCard.iniciar(0.08f, 0.5f, 350);
  detResp.iniciar(0.03f, 0.5f, 1200);
#if USE_PULSE
  detPulso.iniciar(0.05f, 0.5f, 400);
#endif

#if USE_MAX30102
  if (!maxSensor.begin(Wire, I2C_SPEED_FAST)) {
    Serial.println("MAX30102 nao encontrado (0x57). Segue sem HR real.");
  } else {
    maxSensor.setup(60, 4, 2, 100, 411, 4096); // brilho, média, LEDs, taxa, largura, ADC
    maxSensor.setPulseAmplitudeRed(0x3C);
    maxSensor.setPulseAmplitudeGreen(0);
    detMax.iniciar(0.02f, 0.5f, 350);
    temMax = true;
    Serial.println("MAX30102 ok.");
  }
#endif

  zerarAcumuladores();
  ultimaAmostraUs = micros();
  Serial.printf("DataBov v2: coleira %s pronta.\n", COLLAR_ID);
}

void loop() {
  unsigned long agoraUs = micros();
  if (agoraUs - ultimaAmostraUs < PERIODO_US) { yield(); return; }
  ultimaAmostraUs += PERIODO_US;
  if (agoraUs - ultimaAmostraUs > 5 * PERIODO_US) ultimaAmostraUs = agoraUs;

  float ax, ay, az, gx, gy, gz;
  if (!mpuLer(ax, ay, az, gx, gy, gz)) return;

  float mag = sqrtf(ax * ax + ay * ay + az * az);
  dcMag += 0.01f * (mag - dcMag);
  float dyn = mag - dcMag;
  float gyroMag = sqrtf(gx * gx + gy * gy + gz * gz);
  emaDynQ += 0.05f * (fabsf(dyn) - emaDynQ);
  emaGyroQ += 0.05f * (gyroMag - emaGyroQ);

  unsigned long agoraMs = millis();
  detCard.atualizar(filtroCard.processar(dyn), agoraMs);
  detResp.atualizar(filtroResp.processar(dyn), agoraMs);

#if USE_MAX30102
  if (temMax) {
    maxSensor.check();
    while (maxSensor.available()) {
      uint32_t red = maxSensor.getRed();
      uint32_t ir = maxSensor.getIR();
      maxSensor.nextSample();
      maxIR[maxIdx] = ir; maxRed[maxIdx] = red;
      maxIdx = (maxIdx + 1) % MAXN;
      if (maxIdx == 0) maxCheio = true;
      detMax.atualizar((float)ir, agoraMs);
    }
    // a cada ~2 s com buffer cheio: HR + SpO2 (se quieto)
    static unsigned long ultMaxMs = 0;
    if (maxCheio && agoraMs - ultMaxMs > 2000) {
      ultMaxMs = agoraMs;
      processarMax();
    }
  }
#endif

#if USE_PULSE
  {
    int raw = analogRead(A0);
    detPulso.atualizar((float)raw, agoraMs);
    if (emaDynQ < 0.05f && emaGyroQ < GYRO_QUIETO && detPulso.fresco(agoraMs, 5000)) {
      float bpm = detPulso.taxa();
      if (bpm >= 25 && bpm <= 220 && bpm > acHrPulso) acHrPulso = (uint8_t)bpm;
    }
  }
#endif

  emaMag += 0.25f * (dyn - emaMag);
  wax[wi] = ax; way[wi] = ay; waz[wi] = az; wmag[wi] = emaMag; wgyro[wi] = gyroMag;
  if (++wi >= WIN) {
    wi = 0;
    processarJanela();
    tentarEnviar();
  }
}

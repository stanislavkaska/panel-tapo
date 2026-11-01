#include "tapo_klap.h"
#include <WiFi.h>
#include <mbedtls/sha1.h>
#include <mbedtls/sha256.h>
#include <mbedtls/aes.h>
#include <esp_random.h>
#include <time.h>
#include <string.h>
#include <ctype.h>

static const uint32_t KLAP_MAGIC = 0x4B4C5031;   // "KLP1"
static const uint32_t HTTP_TIMEOUT_MS = 4000;
static const uint32_t CONNECT_TIMEOUT_MS = 3000;
static const int64_t  SESSION_MAX_S = 20LL * 3600LL;   // cookie TIMEOUT bývá 1440 min, bereme s rezervou

// Sdílené buffery: požadavky jdou vždy po jednom
static uint8_t _txBuf[1024];
static uint8_t _rxBuf[4096];

static TapoYieldFn _yieldFn = nullptr;
static uint32_t    _lastYieldMs = 0;

void tapoSetYield(TapoYieldFn fn) { _yieldFn = fn; }

// Krátké čekání v síťové smyčce; mezitím pustí ke slovu dotyk
static void netWait() {
  uint32_t now = millis();
  if (_yieldFn && (now - _lastYieldMs) >= 15) {
    _lastYieldMs = now;
    _yieldFn();
  }
  delay(1);
}

static void be32(uint8_t* out, int32_t v) {
  uint32_t u = (uint32_t)v;
  out[0] = (uint8_t)(u >> 24);
  out[1] = (uint8_t)(u >> 16);
  out[2] = (uint8_t)(u >> 8);
  out[3] = (uint8_t)u;
}

// sha256 ze spojení až čtyř částí
static void sha256Cat(const void* a, size_t al, const void* b, size_t bl,
                      const void* c, size_t cl, const void* d, size_t dl, uint8_t out[32]) {
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);
  if (a && al) mbedtls_sha256_update(&ctx, (const uint8_t*)a, al);
  if (b && bl) mbedtls_sha256_update(&ctx, (const uint8_t*)b, bl);
  if (c && cl) mbedtls_sha256_update(&ctx, (const uint8_t*)c, cl);
  if (d && dl) mbedtls_sha256_update(&ctx, (const uint8_t*)d, dl);
  mbedtls_sha256_finish(&ctx, out);
  mbedtls_sha256_free(&ctx);
}

static bool startsWithNoCase(const char* s, const char* prefix) {
  while (*prefix) {
    if (!*s || tolower((unsigned char)*s) != tolower((unsigned char)*prefix)) return false;
    s++;
    prefix++;
  }
  return true;
}

// ─── Nastavení ─────────────────────────────────────────────────────────────
void TapoKlap::setHost(const char* host) {
  strncpy(_host, host ? host : "", sizeof(_host) - 1);
  _host[sizeof(_host) - 1] = 0;
  _hostIsIp = _ip.fromString(_host);
  if (!_hostIsIp) _ip = IPAddress((uint32_t)0);   // jméno se rozliší až při prvním spojení
}

void TapoKlap::setAuth(const char* user, const char* pass) {
  uint8_t h1[20], h2[20], cat[40];
  mbedtls_sha1((const uint8_t*)user, strlen(user), h1);
  mbedtls_sha1((const uint8_t*)pass, strlen(pass), h2);
  memcpy(cat, h1, 20);
  memcpy(cat + 20, h2, 20);
  mbedtls_sha256(cat, 40, _auth, 0);
  _authSet = true;
}

bool TapoKlap::setAuthHashHex(const char* hex) {
  if (!hex || strlen(hex) != 64) return false;
  for (int i = 0; i < 32; i++) {
    char b[3] = { hex[2 * i], hex[2 * i + 1], 0 };
    if (!isxdigit((unsigned char)b[0]) || !isxdigit((unsigned char)b[1])) return false;
    _auth[i] = (uint8_t)strtoul(b, nullptr, 16);
  }
  _authSet = true;
  return true;
}

// ─── Relace v RTC paměti ───────────────────────────────────────────────────
bool TapoKlap::restore(const KlapSession& s) {
  if (s.magic != KLAP_MAGIC) return false;
  if (s.ip == 0) return false;
  if ((uint32_t)_ip == 0) {
    _ip = IPAddress(s.ip);          // host zadaný jménem: převezme se minule rozlišená adresa
  } else if (s.ip != (uint32_t)_ip) {
    return false;
  }
  if (s.expiresAt != 0 && (int64_t)time(nullptr) > s.expiresAt) return false;
  memcpy(_key, s.key, sizeof(_key));
  memcpy(_iv, s.iv, sizeof(_iv));
  memcpy(_sig, s.sig, sizeof(_sig));
  _seq = s.seq;
  strncpy(_cookie, s.cookie, sizeof(_cookie) - 1);
  _cookie[sizeof(_cookie) - 1] = 0;
  _expiresAt = s.expiresAt;
  _hasSession = true;
  return true;
}

void TapoKlap::save(KlapSession& s) const {
  if (!_hasSession) {
    s.magic = 0;
    return;
  }
  s.magic = KLAP_MAGIC;
  s.ip = (uint32_t)_ip;
  memcpy(s.key, _key, sizeof(_key));
  memcpy(s.iv, _iv, sizeof(_iv));
  memcpy(s.sig, _sig, sizeof(_sig));
  s.seq = _seq;
  strncpy(s.cookie, _cookie, sizeof(s.cookie) - 1);
  s.cookie[sizeof(s.cookie) - 1] = 0;
  s.expiresAt = _expiresAt;
}

// ─── HTTP po syrovém TCP (binární tělo, cookie, keep-alive) ────────────────
bool TapoKlap::ensureConnected() {
  if (_client.connected()) return true;
  _client.stop();
  if ((uint32_t)_ip == 0) {
    IPAddress r;
    if (!WiFi.hostByName(_host, r)) return false;
    _ip = r;
  }
  if (!_client.connect(_ip, 80, CONNECT_TIMEOUT_MS)) {
    if (!_hostIsIp) _ip = IPAddress((uint32_t)0);   // jméno se příště rozliší znovu
    return false;
  }
  _client.setNoDelay(true);
  return true;
}

bool TapoKlap::readLine(char* buf, size_t cap, uint32_t deadline) {
  size_t n = 0;
  while ((int32_t)(millis() - deadline) < 0) {
    int a = _client.available();
    if (a <= 0) {
      if (!_client.connected()) return false;
      netWait();
      continue;
    }
    int c = _client.read();
    if (c < 0) continue;
    if (c == '\n') {
      buf[n] = 0;
      return true;
    }
    if (c != '\r' && n < cap - 1) buf[n++] = (char)c;
  }
  return false;
}

// Vrací HTTP stav, nebo záporně: -1 spojení, -3 vypršel čas (nebo se spojení
// zavřelo), -4 odpověď se nevešla. httpPostOnce navíc -2 = spojení zavřené
// ještě před odpovědí.
int TapoKlap::httpPost(const char* path, const uint8_t* body, size_t bodyLen,
                       uint8_t* resp, size_t respCap, size_t& respLen, bool parseCookie) {
  // Spojení keep-alive mohlo zařízení mezitím zavřít (po probuzení jde vždy
  // o nové spojení, ale mezi načtením stavu a přepnutím uběhnou sekundy).
  // Když se znovu použité spojení zavře dřív, než přijde odpověď, zkusí se
  // požadavek jednou na novém spojení.
  bool reused = _client.connected();
  int st = httpPostOnce(path, body, bodyLen, resp, respCap, respLen, parseCookie);
  if (st == -2 && reused) {
    _client.stop();
    st = httpPostOnce(path, body, bodyLen, resp, respCap, respLen, parseCookie);
  }
  return (st == -2) ? -3 : st;
}

int TapoKlap::httpPostOnce(const char* path, const uint8_t* body, size_t bodyLen,
                           uint8_t* resp, size_t respCap, size_t& respLen, bool parseCookie) {
  respLen = 0;
  _httpStatus = 0;
  if (!ensureConnected()) return -1;

  char hdr[320];
  int n = snprintf(hdr, sizeof(hdr),
    "POST %s HTTP/1.1\r\n"
    "Host: %s\r\n"
    "Content-Type: application/octet-stream\r\n"
    "Accept: */*\r\n"
    "Connection: keep-alive\r\n"
    "Content-Length: %u\r\n",
    path, _ip.toString().c_str(), (unsigned)bodyLen);
  if (_cookie[0]) {
    n += snprintf(hdr + n, sizeof(hdr) - n, "Cookie: TP_SESSIONID=%s\r\n", _cookie);
  }
  n += snprintf(hdr + n, sizeof(hdr) - n, "\r\n");
  if (_client.write((const uint8_t*)hdr, n) != (size_t)n) { _client.stop(); return -2; }
  if (bodyLen && _client.write(body, bodyLen) != bodyLen) { _client.stop(); return -2; }

  uint32_t deadline = millis() + HTTP_TIMEOUT_MS;
  char line[256];
  int status = -3;
  long contentLen = -1;
  bool closeAfter = false;
  bool first = true;
  for (;;) {
    if (!readLine(line, sizeof(line), deadline)) {
      // Zavřené spojení ještě před stavovým řádkem: zařízení požadavek nepřijalo
      bool closedEarly = first && !_client.connected() && (int32_t)(millis() - deadline) < 0;
      _client.stop();
      return closedEarly ? -2 : -3;
    }
    if (first) {
      first = false;
      if (strncmp(line, "HTTP/1.", 7) != 0 || strlen(line) < 12) { _client.stop(); return -3; }
      status = atoi(line + 9);
      continue;
    }
    if (line[0] == 0) break;
    if (startsWithNoCase(line, "content-length:")) {
      contentLen = atol(line + 15);
    } else if (startsWithNoCase(line, "connection:")) {
      if (strstr(line, "close") || strstr(line, "Close")) closeAfter = true;
    } else if (parseCookie && startsWithNoCase(line, "set-cookie:")) {
      const char* p = strstr(line, "TP_SESSIONID=");
      if (p) {
        p += 13;
        size_t i = 0;
        while (*p && *p != ';' && *p != ' ' && i < sizeof(_cookie) - 1) _cookie[i++] = *p++;
        _cookie[i] = 0;
      }
      const char* t = strstr(line, "TIMEOUT=");
      if (t) {
        long secs = atol(t + 8);                // v sekundách (typicky 86400)
        if (secs > 60) {
          int64_t life = (int64_t)secs - 20 * 60;   // rezerva 20 min jako python-kasa
          if (life < 60) life = 60;
          if (life > SESSION_MAX_S) life = SESSION_MAX_S;
          _expiresAt = (int64_t)time(nullptr) + life;
        }
      }
    }
  }
  _httpStatus = status;

  if (contentLen > (long)respCap) {
    // Odpověď se nevejde: spojení zavřít, zbytek by zůstal v rouře
    _client.stop();
    return -4;
  }

  // Bez Content-Length (zařízení ho běžně posílá) se čte, dokud data chodí;
  // 300 ms ticha nebo zavření spojení = konec těla
  size_t want = (contentLen >= 0) ? (size_t)contentLen : respCap;
  uint32_t lastDataMs = millis();
  while (respLen < want) {
    if ((int32_t)(millis() - deadline) >= 0) {
      if (contentLen >= 0) { _client.stop(); return -3; }
      break;
    }
    int a = _client.available();
    if (a > 0) {
      size_t chunk = (size_t)a;
      if (chunk > want - respLen) chunk = want - respLen;
      int r = _client.read(resp + respLen, chunk);
      if (r > 0) respLen += (size_t)r;
      lastDataMs = millis();
    } else if (!_client.connected()) {
      if (contentLen >= 0 && respLen < want) return -3;
      break;
    } else {
      if (contentLen < 0 && (millis() - lastDataMs) >= 300) break;
      netWait();
    }
  }
  if (closeAfter) _client.stop();
  return status;
}

// ─── Handshake ─────────────────────────────────────────────────────────────
TapoKlap::Err TapoKlap::handshake() {
  if (!_authSet) return ERR_NOAUTH;
  _hasSession = false;
  _cookie[0] = 0;
  _expiresAt = 0;

  uint8_t local[16];
  esp_fill_random(local, sizeof(local));

  uint8_t resp[64];
  size_t rl = 0;
  int st = httpPost("/app/handshake1", local, 16, resp, sizeof(resp), rl, true);
  if (st == -1) return ERR_CONNECT;
  if (st < 0) return ERR_TIMEOUT;
  if (st != 200) return ERR_HTTP;
  if (rl < 48) return ERR_PROTOCOL;

  uint8_t remote[16];
  memcpy(remote, resp, 16);
  uint8_t expect[32];
  sha256Cat(local, 16, remote, 16, _auth, 32, nullptr, 0, expect);
  if (memcmp(expect, resp + 16, 32) != 0) return ERR_AUTH;

  uint8_t h2[32];
  sha256Cat(remote, 16, local, 16, _auth, 32, nullptr, 0, h2);
  st = httpPost("/app/handshake2", h2, 32, resp, sizeof(resp), rl, false);
  if (st == -1) return ERR_CONNECT;
  if (st < 0) return ERR_TIMEOUT;
  if (st != 200) return ERR_HTTP;

  uint8_t t[32];
  sha256Cat("lsk", 3, local, 16, remote, 16, _auth, 32, t);
  memcpy(_key, t, 16);
  sha256Cat("iv", 2, local, 16, remote, 16, _auth, 32, t);
  memcpy(_iv, t, 12);
  _seq = (int32_t)(((uint32_t)t[28] << 24) | ((uint32_t)t[29] << 16) | ((uint32_t)t[30] << 8) | (uint32_t)t[31]);
  sha256Cat("ldk", 3, local, 16, remote, 16, _auth, 32, t);
  memcpy(_sig, t, 28);

  if (_expiresAt == 0) _expiresAt = (int64_t)time(nullptr) + SESSION_MAX_S;
  _hasSession = true;
  return OK;
}

// ─── Šifrovaný požadavek ───────────────────────────────────────────────────
TapoKlap::Err TapoKlap::doRequest(const char* json, String& out) {
  size_t jl = strlen(json);
  size_t padded = (jl / 16 + 1) * 16;          // PKCS7: vždy aspoň 1 bajt výplně
  if (padded + 32 > sizeof(_txBuf)) return ERR_PROTOCOL;

  uint8_t* ct = _txBuf + 32;
  memcpy(ct, json, jl);
  memset(ct + jl, (int)(padded - jl), padded - jl);

  _seq++;
  uint8_t ivseq[16];
  memcpy(ivseq, _iv, 12);
  be32(ivseq + 12, _seq);

  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  mbedtls_aes_setkey_enc(&aes, _key, 128);
  int rc = mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_ENCRYPT, padded, ivseq, ct, ct);
  mbedtls_aes_free(&aes);
  if (rc != 0) return ERR_PROTOCOL;

  uint8_t seqbe[4];
  be32(seqbe, _seq);
  sha256Cat(_sig, 28, seqbe, 4, ct, padded, nullptr, 0, _txBuf);   // podpis do prvních 32 bajtů

  char path[48];
  snprintf(path, sizeof(path), "/app/request?seq=%ld", (long)_seq);
  size_t rl = 0;
  int st = httpPost(path, _txBuf, 32 + padded, _rxBuf, sizeof(_rxBuf), rl, false);
  if (st == -1) return ERR_CONNECT;
  if (st == -4) return ERR_PROTOCOL;
  if (st < 0) return ERR_TIMEOUT;
  if (st == 403) return ERR_SESSION;
  if (st != 200) return ERR_HTTP;
  if (rl < 32 + 16 || ((rl - 32) % 16) != 0) return ERR_PROTOCOL;

  size_t cl = rl - 32;
  memcpy(ivseq, _iv, 12);
  be32(ivseq + 12, _seq);
  mbedtls_aes_init(&aes);
  mbedtls_aes_setkey_dec(&aes, _key, 128);
  rc = mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_DECRYPT, cl, ivseq, _rxBuf + 32, _rxBuf + 32);
  mbedtls_aes_free(&aes);
  if (rc != 0) return ERR_PROTOCOL;

  uint8_t pad = _rxBuf[32 + cl - 1];
  if (pad < 1 || pad > 16 || pad > cl) return ERR_PROTOCOL;
  size_t len = cl - pad;
  out = String();
  if (!out.reserve(len + 1)) return ERR_PROTOCOL;
  out.concat((const char*)(_rxBuf + 32), len);
  if (len > 0 && out[0] != '{') return ERR_PROTOCOL;
  return OK;
}

TapoKlap::Err TapoKlap::request(const char* json, String& out) {
  if (!_authSet) return ERR_NOAUTH;
  _devCode = 0;
  Err e = OK;
  for (int attempt = 0; attempt < 2; attempt++) {
    if (!_hasSession) {
      e = handshake();
      if (e != OK) return e;
    }
    e = doRequest(json, out);
    if (e == OK) {
      // Zařízení hlásí vypršelou relaci i přes HTTP 200
      if (strstr(out.c_str(), "\"error_code\":9999") || strstr(out.c_str(), "\"error_code\": 9999")) {
        e = ERR_SESSION;
      } else if (strstr(out.c_str(), "\"error_code\":-1501") || strstr(out.c_str(), "\"error_code\": -1501")) {
        return ERR_AUTH;
      } else {
        return OK;
      }
    }
    if (e == ERR_CONNECT || e == ERR_TIMEOUT || e == ERR_AUTH) return e;
    // Neplatná relace nebo nečekaná odpověď: nový handshake a ještě jeden pokus
    _hasSession = false;
    _client.stop();
  }
  return e;
}

const char* TapoKlap::errText(Err e) const {
  switch (e) {
    case OK:           return "";
    case ERR_CONNECT:  return "není v síti";
    case ERR_TIMEOUT:  return "neodpovídá";
    case ERR_HTTP:     return "chyba HTTP";
    case ERR_SESSION:  return "relace vypršela";
    case ERR_AUTH:     return "špatné přihlášení";
    case ERR_PROTOCOL: return "chyba protokolu";
    case ERR_NOAUTH:   return "chybí účet Tapo";
    case ERR_DEVICE:
      switch (_devCode) {
        case -1501: return "špatné přihlášení";
        case 9999:  return "relace vypršela";
        case 1002: case -1002: case 1003: return "neplatný požadavek";
        case -1008: return "neplatný parametr";
        case -1301: return "zařízení mimo síť";
        case -1803: return "stejný čas zap i vyp";
        case -1901: case -2001: return "zařízení zaneprázdněno";
        default: {
          static char b[24];
          snprintf(b, sizeof(b), "chyba %d", _devCode);
          return b;
        }
      }
  }
  return "chyba";
}

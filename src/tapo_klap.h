#pragma once
#include <Arduino.h>
#include <IPAddress.h>
#include <WiFiClient.h>

// Klient protokolu KLAP, kterým se zařízení Tapo (firmware od roku 2023)
// ovládají přímo v místní síti bez cloudu. Postup podle python-kasa
// (KlapTransportV2):
//   auth_hash  = sha256(sha1(e-mail) + sha1(heslo))
//   handshake1 = POST /app/handshake1 s 16 náhodnými bajty (local_seed);
//                odpověď = remote_seed(16) + sha256(local + remote + auth)
//   handshake2 = POST /app/handshake2 se sha256(remote + local + auth)
//   klíč       = sha256("lsk" + local + remote + auth)[0..16]
//   iv + seq   = sha256("iv"  + local + remote + auth): [0..12] + poslední 4 bajty
//   podpis     = sha256("ldk" + local + remote + auth)[0..28]
//   požadavek  = POST /app/request?seq=N, tělo = sha256(sig + seq + šifra) + AES-CBC(json)
// Relace (cookie TP_SESSIONID + klíče + seq) jde uložit do RTC paměti a po
// hlubokém spánku pokračovat bez nového handshaku.

struct KlapSession {
  uint32_t magic;
  uint32_t ip;
  uint8_t  key[16];
  uint8_t  iv[12];
  uint8_t  sig[28];
  int32_t  seq;
  char     cookie[40];
  int64_t  expiresAt;     // time() v sekundách, 0 = neznámo
};

// Volá se během čekání na odpověď zařízení (každých ~15 ms), aby firmware mohl
// mezitím vzorkovat dotyk. nullptr = bez volání.
typedef void (*TapoYieldFn)();
void tapoSetYield(TapoYieldFn fn);

class TapoKlap {
public:
  enum Err : uint8_t {
    OK = 0,
    ERR_CONNECT,     // TCP spojení se nepovedlo (zařízení není v síti)
    ERR_TIMEOUT,     // zařízení neodpovědělo včas
    ERR_HTTP,        // jiný HTTP stav než 200
    ERR_SESSION,     // HTTP 403 nebo error_code 9999: relace neplatí
    ERR_AUTH,        // hash přihlášení nesedí (špatný e-mail/heslo, nebo vypnutá kompatibilita)
    ERR_PROTOCOL,    // nečekaná odpověď (délka, dešifrování, JSON)
    ERR_DEVICE,      // zařízení vrátilo error_code (viz lastDeviceCode())
    ERR_NOAUTH       // nejsou nastavené přihlašovací údaje
  };

  void setHost(const char* host);                 // IP adresa nebo jméno
  void setAuth(const char* user, const char* pass);
  bool setAuthHashHex(const char* hex64);         // alternativa: hotový hash (64 hex znaků)

  bool restore(const KlapSession& s);             // obnoví relaci z RTC paměti
  void save(KlapSession& s) const;
  bool hasSession() const { return _hasSession; }

  Err  handshake();
  // Pošle JSON a vrátí dešifrovanou odpověď. Při neplatné relaci udělá jednou
  // nový handshake a požadavek zopakuje.
  Err  request(const char* json, String& out);

  int  lastDeviceCode() const { return _devCode; }
  void setDeviceError(int code) { _devCode = code; }
  int  lastHttpStatus() const { return _httpStatus; }
  const char* errText(Err e) const;

private:
  bool ensureConnected();
  int  httpPost(const char* path, const uint8_t* body, size_t bodyLen,
                uint8_t* resp, size_t respCap, size_t& respLen, bool parseCookie);
  int  httpPostOnce(const char* path, const uint8_t* body, size_t bodyLen,
                    uint8_t* resp, size_t respCap, size_t& respLen, bool parseCookie);
  bool readLine(char* buf, size_t cap, uint32_t deadline);
  Err  doRequest(const char* json, String& out);

  WiFiClient _client;
  char       _host[48] = "";
  IPAddress  _ip;
  bool       _hostIsIp = false;
  bool       _authSet = false;
  uint8_t    _auth[32];
  bool       _hasSession = false;
  uint8_t    _key[16];
  uint8_t    _iv[12];
  uint8_t    _sig[28];
  int32_t    _seq = 0;
  char       _cookie[40] = "";
  int64_t    _expiresAt = 0;
  int        _devCode = 0;
  int        _httpStatus = 0;
};

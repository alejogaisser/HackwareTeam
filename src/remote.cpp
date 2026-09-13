#include "remote.h"
#include "config.h"
#include "webui.h"

#include <WiFi.h>
#include <ESPmDNS.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <esp_system.h>
#include "mbedtls/md.h"

RemoteState remote = {
    // Arranca APAGADO y quieto. Que un robot con motores empiece a andar solo
    // apenas se le enchufa la bateria es una sorpresa desagradable: ahora hay
    // que elegir el modo a mano desde el celular.
    /* mode           */ MODE_OFF,
    /* joyX           */ 0,
    /* joyY           */ 0,
    /* joyAt          */ 0,
    /* armL           */ 0,           // 0 logico = brazo abajo, pegado al cuerpo
    /* armR           */ 0,
    /* micMuted       */ false,
    /* micSource      */ MIC_ROBOT,
    /* sayRequested   */ false,
    /* phoneAudioReady*/ false,
    /* boxeoRequested */ false,
    /* talkRequested  */ false,
    /* testMic        */ false,
    /* testArms       */ false,
    /* testSpeaker    */ false,
    /* testOled       */ false,
    /* testTone       */ false,
};

static AsyncWebServer server(WEB_PORT);
static AsyncWebSocket ws("/ws");

// Texto escrito desde la web. Buffer fijo y no String: lo escribe la tarea de
// AsyncTCP y lo lee loop(), y un String se realoca al asignarlo -si justo lo
// estan leyendo, se lee memoria ya liberada-.
static char sayText[192] = {0};

// Audio subido desde el celular. Se arma en la tarea de AsyncTCP mientras entra
// el POST y se le cede a loop() de una pieza.
static uint8_t *phoneBuf = nullptr;
static size_t   phoneLen = 0;
static size_t   phoneCap = 0;
static char     phoneName[24] = PHONE_AUDIO_FILENAME;
static char     phoneMime[24] = PHONE_AUDIO_MIME;

// ======================= AUTENTICACION =======================
//
// La pagina se sirve por HTTP plano, sin cifrar: cualquiera en la misma red WiFi
// puede ver el trafico. Por eso la contraseña NUNCA viaja:
//
//   1. Al conectarse, el robot le manda al celular un numero al azar ("nonce"),
//      distinto en cada conexion.
//   2. El celular responde HMAC-SHA256(contraseña, "walle-auth:" + nonce).
//   3. El robot hace la misma cuenta y compara.
//
// Quien espie la red ve el nonce y la respuesta, pero no la contraseña, y la
// respuesta no le sirve para la proxima conexion porque el nonce cambia.
//
// Lo que esto NO protege, y conviene saberlo:
//   - Con esos dos datos se puede intentar adivinar la contraseña probando sin
//     conexion. Contra una contraseña corta eso funciona; contra una larga y al
//     azar, no. Por eso se exige un minimo y se recomienda larga.
//   - El resto del trafico (comandos, textos de la charla) viaja sin cifrar.
//
// Hasta autenticarse, un cliente no puede mandar ningun comando ni recibe nada:
// ni telemetria, ni diagnostico, ni el dialogo.

#define AUTH_SLOTS   8     // igual al maximo de clientes de AsyncWebSocket
#define NONCE_HEX    32    // 16 bytes al azar
#define FAIL_IPS     8

struct AuthSlot {
  uint32_t id;             // id del cliente WebSocket; 0 = slot libre
  bool authed;
  unsigned long since;     // cuando se conecto
  char nonce[NONCE_HEX + 1];
};

// La tabla la tocan dos tareas: AsyncTCP (conexiones y comandos) y loop()
// (telemetria). El spinlock evita leer un slot a medio escribir. Adentro del
// lock solo se copian bytes: nada de Serial, ni red, ni reservas de memoria.
static AuthSlot slots[AUTH_SLOTS];
static portMUX_TYPE authMux = portMUX_INITIALIZER_UNLOCKED;

// Token de la sesion actual, para la subida de audio por HTTP (que no pasa por
// el WebSocket y por lo tanto no sabe quien se autentico). Se regenera en cada
// login. Solo lo toca la tarea de AsyncTCP.
static char micToken[NONCE_HEX + 1] = {0};

// Intentos fallidos POR IP. Por IP y no global a proposito: con un bloqueo
// global, cualquiera en la red podria fallar a proposito cinco veces y dejar
// afuera al dueño del robot. Solo lo toca la tarea de AsyncTCP.
struct FailEntry {
  uint32_t ip;
  uint8_t fails;
  unsigned long lockUntil;   // 0 = no bloqueada
  unsigned long lastAt;
};
static FailEntry failTable[FAIL_IPS];

bool remoteAuthConfigured() {
  const char *pw = WEB_PASSWORD;
  if (strlen(pw) < WEB_PASSWORD_MIN_LEN) return false;
  // El valor de ejemplo de secrets.h.example no cuenta como contraseña.
  if (!strncmp(pw, "CAMBIAME", 8)) return false;
  return true;
}

static void randomHex(char *out, size_t hexLen) {
  uint8_t b[32];
  const size_t n = hexLen / 2;
  // Con el WiFi encendido, esp_fill_random es un generador por hardware de
  // verdad. random() de Arduino es predecible y no sirve para esto.
  esp_fill_random(b, n);
  for (size_t i = 0; i < n; i++) sprintf(out + 2 * i, "%02x", b[i]);
  out[hexLen] = 0;
}

// Comparacion en tiempo constante. strcmp() corta en el primer caracter
// distinto, y midiendo cuanto tarda en contestar se puede ir adivinando la
// respuesta correcta de a un caracter.
static bool sameConstTime(const char *a, const char *b) {
  const size_t la = strlen(a), lb = strlen(b);
  if (la != lb) return false;
  uint8_t diff = 0;
  for (size_t i = 0; i < la; i++) diff |= (uint8_t)(a[i] ^ b[i]);
  return diff == 0;
}

static bool hmacHex(const char *key, const char *msg, char *out65) {
  uint8_t mac[32];
  const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (!info) return false;
  if (mbedtls_md_hmac(info, (const unsigned char *)key, strlen(key),
                      (const unsigned char *)msg, strlen(msg), mac) != 0) {
    return false;
  }
  for (int i = 0; i < 32; i++) sprintf(out65 + 2 * i, "%02x", mac[i]);
  out65[64] = 0;
  return true;
}

// Solo con authMux tomado.
static int slotIndexLocked(uint32_t id) {
  for (int i = 0; i < AUTH_SLOTS; i++) {
    if (slots[i].id == id) return i;
  }
  return -1;
}

static bool isAuthed(uint32_t id) {
  bool ok = false;
  portENTER_CRITICAL(&authMux);
  const int i = slotIndexLocked(id);
  if (i >= 0) ok = slots[i].authed;
  portEXIT_CRITICAL(&authMux);
  return ok;
}

// Copia los ids autenticados. Se manda afuera del lock.
static uint8_t authedIds(uint32_t *out) {
  uint8_t n = 0;
  portENTER_CRITICAL(&authMux);
  for (int i = 0; i < AUTH_SLOTS; i++) {
    if (slots[i].id && slots[i].authed) out[n++] = slots[i].id;
  }
  portEXIT_CRITICAL(&authMux);
  return n;
}

// Reemplazo de ws.textAll(): manda SOLO a los clientes autenticados.
static void textAuthed(const char *msg) {
  uint32_t ids[AUTH_SLOTS];
  const uint8_t n = authedIds(ids);
  for (uint8_t k = 0; k < n; k++) ws.text(ids[k], msg);
}

static FailEntry *failEntryFor(uint32_t ip, unsigned long now) {
  FailEntry *libre = nullptr, *masVieja = &failTable[0];
  for (int i = 0; i < FAIL_IPS; i++) {
    if (failTable[i].ip == ip) return &failTable[i];
    if (!failTable[i].ip && !libre) libre = &failTable[i];
    if (failTable[i].lastAt < masVieja->lastAt) masVieja = &failTable[i];
  }
  // Tabla llena: se recicla la IP que hace mas tiempo que no falla.
  FailEntry *e = libre ? libre : masVieja;
  e->ip = ip;
  e->fails = 0;
  e->lockUntil = 0;
  e->lastAt = now;
  return e;
}

// Manda un nonce nuevo. Se usa al conectarse y despues de cada intento
// fallido: un nonce sirve para UN intento, asi un espia no puede reusarlo.
static void sendHello(AsyncWebSocketClient *client) {
  char nonce[NONCE_HEX + 1];
  randomHex(nonce, NONCE_HEX);

  bool stored = false;
  portENTER_CRITICAL(&authMux);
  const int i = slotIndexLocked(client->id());
  if (i >= 0) {
    memcpy(slots[i].nonce, nonce, sizeof(nonce));
    slots[i].authed = false;
    stored = true;
  }
  portEXIT_CRITICAL(&authMux);
  if (!stored) return;

  char buf[96];
  snprintf(buf, sizeof(buf), "{\"t\":\"hello\",\"nonce\":\"%s\",\"cfg\":%s}", nonce,
           remoteAuthConfigured() ? "true" : "false");
  client->text(buf);
}

static void handleAuth(AsyncWebSocketClient *client, const char *mac) {
  const unsigned long now = millis();
  char buf[96];

  if (!remoteAuthConfigured()) {
    // Sin contraseña configurada el control queda cerrado. Abrirlo "porque no
    // hay contraseña" dejaria el robot manejable por cualquiera en la red.
    client->text("{\"t\":\"authfail\",\"wait\":0,\"cfg\":false}");
    return;
  }

  const uint32_t ip = (uint32_t)client->remoteIP();
  FailEntry *fe = failEntryFor(ip, now);

  if (fe->lockUntil && now < fe->lockUntil) {
    snprintf(buf, sizeof(buf), "{\"t\":\"authfail\",\"wait\":%lu}",
             (fe->lockUntil - now + 999) / 1000);
    client->text(buf);
    sendHello(client);
    return;
  }
  if (fe->lockUntil) {   // el bloqueo ya vencio
    fe->lockUntil = 0;
    fe->fails = 0;
  }

  char nonce[NONCE_HEX + 1];
  nonce[0] = 0;
  portENTER_CRITICAL(&authMux);
  const int i = slotIndexLocked(client->id());
  if (i >= 0) memcpy(nonce, slots[i].nonce, sizeof(nonce));
  portEXIT_CRITICAL(&authMux);
  if (!nonce[0]) return;   // intento sin nonce vigente: se ignora

  char msg[16 + NONCE_HEX];
  snprintf(msg, sizeof(msg), "walle-auth:%s", nonce);
  char expected[65];
  const bool ok = hmacHex(WEB_PASSWORD, msg, expected) && sameConstTime(expected, mac);

  if (!ok) {
    fe->fails++;
    fe->lastAt = now;
    unsigned long wait = 0;
    if (fe->fails >= AUTH_MAX_FAILS) {
      fe->fails = 0;
      fe->lockUntil = now + AUTH_LOCK_MS;
      if (!fe->lockUntil) fe->lockUntil = 1;
      wait = AUTH_LOCK_MS / 1000;
    }
    Serial.printf("Web: contrasena incorrecta desde %s%s\n",
                  client->remoteIP().toString().c_str(),
                  wait ? " (IP bloqueada un rato)" : "");
    snprintf(buf, sizeof(buf), "{\"t\":\"authfail\",\"wait\":%lu}", wait);
    client->text(buf);
    sendHello(client);
    return;
  }

  fe->fails = 0;

  portENTER_CRITICAL(&authMux);
  const int j = slotIndexLocked(client->id());
  if (j >= 0) {
    slots[j].authed = true;
    slots[j].nonce[0] = 0;   // nonce consumido
  }
  portEXIT_CRITICAL(&authMux);

  randomHex(micToken, NONCE_HEX);

  // Recien ahora un celular puede manejar: se arranca con el joystick vencido,
  // igual que siempre.
  remote.joyX = 0;
  remote.joyY = 0;
  remote.joyAt = 0;

  Serial.printf("Web: cliente #%u autenticado\n", client->id());
  snprintf(buf, sizeof(buf), "{\"t\":\"authok\",\"tok\":\"%s\"}", micToken);
  client->text(buf);

  // El reporte de arranque se manda a cada cliente que entra: el celular casi
  // siempre se conecta despues de que el robot booteo.
  remoteSendDiag(client);
}

static bool tokenOk(AsyncWebServerRequest *request) {
  if (!micToken[0]) return false;
  if (!request->hasHeader("X-Robot-Token")) return false;
  return sameConstTime(request->getHeader("X-Robot-Token")->value().c_str(), micToken);
}

// ======================= COMANDOS QUE LLEGAN DEL CELULAR =======================
//
// OJO: esto corre en la tarea de AsyncTCP. Ver la nota de arriba en remote.h.
// Aca solo se anota lo que se pidio; quien mueve algo es loop().

static int16_t clampArm(int v) {
  if (v < ARM_DEG_MIN) return ARM_DEG_MIN;
  if (v > ARM_DEG_MAX) return ARM_DEG_MAX;
  return (int16_t)v;
}

static void handleCommand(AsyncWebSocketClient *client, const uint8_t *data, size_t len) {
  // Los mensajes son de 30 a 60 bytes. StaticJsonDocument va en la pila de la
  // tarea de AsyncTCP (por eso platformio.ini le sube la pila a 16KB) y no
  // toca el heap, que es lo que uno quiere en un callback que se llama 20
  // veces por segundo: con DynamicJsonDocument el heap se fragmentaria y a los
  // pocos minutos fallaria la conexion TLS del proximo turno de voz.
  StaticJsonDocument<256> doc;
  if (deserializeJson(doc, data, len)) return;

  const char *t = doc["t"];
  if (!t) return;

  if (!strcmp(t, "auth")) {
    handleAuth(client, doc["mac"] | "");
    return;
  }

  // Todo lo que sigue exige estar autenticado. Sin esto, cualquiera en la red
  // podria mover el robot y gastar los creditos de la API.
  if (!isAuthed(client->id())) return;

  if (!strcmp(t, "joy")) {
    int x = doc["x"] | 0;
    int y = doc["y"] | 0;
    remote.joyX = (int16_t)constrain(x, -100, 100);
    remote.joyY = (int16_t)constrain(y, -100, 100);
    // La marca de tiempo es lo mas importante del mensaje: si deja de
    // actualizarse, loop() frena los motores. Ver JOYSTICK_TIMEOUT_MS.
    remote.joyAt = millis();

  } else if (!strcmp(t, "mode")) {
    const char *v = doc["v"] | "off";
    if      (!strcmp(v, "control")) remote.mode = MODE_CONTROL;
    else if (!strcmp(v, "auto"))    remote.mode = MODE_AUTO;
    else                            remote.mode = MODE_OFF;
    // Al entrar al modo control el joystick arranca en cero y "vencido": el
    // robot no se mueve hasta que llegue una posicion de verdad. Sin esto,
    // cambiar de pestaña con el joystick a medio camino lo largaba andando.
    remote.joyX = 0;
    remote.joyY = 0;
    remote.joyAt = 0;

  } else if (!strcmp(t, "arm")) {
    const char *a = doc["a"] | "";
    int16_t v = clampArm(doc["v"] | 0);
    if (a[0] == 'l') remote.armL = v;
    else if (a[0] == 'r') remote.armR = v;

  } else if (!strcmp(t, "mute")) {
    remote.micMuted = doc["v"] | false;

  } else if (!strcmp(t, "micsrc")) {
    const char *v = doc["v"] | "robot";
    remote.micSource = (!strcmp(v, "phone")) ? MIC_PHONE : MIC_ROBOT;

  } else if (!strcmp(t, "say")) {
    // Texto escrito a mano: el ultimo camino que queda cuando no anda ningun
    // microfono. Salta el paso de transcripcion y va derecho al modelo.
    const char *txt = doc["txt"] | "";
    if (txt[0] && !remote.sayRequested) {
      strlcpy(sayText, txt, sizeof(sayText));
      remote.sayRequested = true;
    }

  } else if (!strcmp(t, "box")) {
    remote.boxeoRequested = true;

  } else if (!strcmp(t, "talk")) {
    remote.talkRequested = true;

  } else if (!strcmp(t, "test")) {
    const char *w = doc["w"] | "";
    if      (!strcmp(w, "mic"))     remote.testMicRequested = true;
    else if (!strcmp(w, "arms"))    remote.testArmsRequested = true;
    else if (!strcmp(w, "speaker")) remote.testSpeakerRequested = true;
    else if (!strcmp(w, "oled"))    remote.testOledRequested = true;
    else if (!strcmp(w, "tone"))    remote.testToneRequested = true;

  } else if (!strcmp(t, "stop")) {
    remote.joyX = 0;
    remote.joyY = 0;
    remote.joyAt = millis();
  }
}

static void onWsEvent(AsyncWebSocket *srv, AsyncWebSocketClient *client,
                      AwsEventType type, void *arg, uint8_t *data, size_t len) {
  switch (type) {
    case WS_EVT_CONNECT: {
      const unsigned long now = millis();
      int libre = -1;
      portENTER_CRITICAL(&authMux);
      for (int i = 0; i < AUTH_SLOTS; i++) {
        if (slots[i].id == 0) {
          libre = i;
          slots[i].id = client->id();
          slots[i].authed = false;
          slots[i].since = now;
          slots[i].nonce[0] = 0;
          break;
        }
      }
      portEXIT_CRITICAL(&authMux);

      if (libre < 0) {
        client->close();
        break;
      }
      Serial.printf("Web: se conecto un cliente (#%u) desde %s\n",
                    client->id(), client->remoteIP().toString().c_str());
      // Conectarse no autoriza nada. A diferencia de antes, tampoco frena el
      // joystick: si no, cualquiera en la red podria cortarle el manejo al
      // dueño con solo abrir la pagina.
      sendHello(client);
      break;
    }

    case WS_EVT_DISCONNECT: {
      bool eraAutenticado = false;
      bool quedaAlguno = false;
      portENTER_CRITICAL(&authMux);
      const int i = slotIndexLocked(client->id());
      if (i >= 0) {
        eraAutenticado = slots[i].authed;
        slots[i].id = 0;
        slots[i].authed = false;
        slots[i].nonce[0] = 0;
      }
      for (int k = 0; k < AUTH_SLOTS; k++) {
        if (slots[k].id && slots[k].authed) quedaAlguno = true;
      }
      portEXIT_CRITICAL(&authMux);

      Serial.printf("Web: se fue el cliente #%u\n", client->id());
      if (eraAutenticado) {
        remote.joyX = 0;
        remote.joyY = 0;
        remote.joyAt = 0;
      }
      if (!quedaAlguno) micToken[0] = 0;   // sin sesiones, sin subidas de audio
      break;
    }

    case WS_EVT_DATA: {
      AwsFrameInfo *info = (AwsFrameInfo *)arg;
      // Los comandos son cortisimos, siempre entran en un solo frame de texto.
      // Un frame partido en pedazos solo puede venir de otra cosa: se ignora.
      if (info->final && info->index == 0 && info->len == len &&
          info->opcode == WS_TEXT) {
        handleCommand(client, data, len);
      }
      break;
    }

    default:
      break;
  }
}

// ======================= ARRANQUE =======================

void remoteSetup() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Web: sin WiFi, no se levanta el servidor.");
    return;
  }

  // mDNS: ademas de por IP, se puede entrar por http://walle.local/
  // Es comodidad, no es critico: si falla, la IP sigue funcionando.
  if (MDNS.begin(ROBOT_HOSTNAME)) {
    MDNS.addService("http", "tcp", WEB_PORT);
  }

  ws.onEvent(onWsEvent);
  server.addHandler(&ws);

  // La pagina en si no tiene nada secreto -la puerta es el WebSocket- asi que
  // se sirve sin contraseña. Las cabeceras impiden que otro sitio la meta
  // adentro de un iframe invisible para hacerte tocar botones sin darte cuenta.
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    AsyncWebServerResponse *res =
        request->beginResponse_P(200, "text/html; charset=utf-8", INDEX_HTML);
    res->addHeader("X-Frame-Options", "DENY");
    res->addHeader("Content-Security-Policy", "frame-ancestors 'none'");
    res->addHeader("X-Content-Type-Options", "nosniff");
    res->addHeader("Referrer-Policy", "no-referrer");
    res->addHeader("Cache-Control", "no-store");
    request->send(res);
  });

  // Los celulares piden un favicon apenas abren la pagina. Sin esta ruta cada
  // carga deja un 404 en el log serie que no significa nada.
  server.on("/favicon.ico", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(204);
  });

  // ---- audio grabado con el microfono del celular ----
  //
  // Va por HTTP y no por el WebSocket a proposito: son 15 a 20KB de una, y el
  // WebSocket esta para mensajes de 30 bytes que tienen que llegar ya.
  //
  // Exige el token de la sesion en la cabecera X-Robot-Token. El chequeo va
  // ANTES de tocar el buffer: si no, un pedido sin permiso podria tirar el
  // audio que estaba subiendo el dueño.
  //
  // Esto corre en la tarea de AsyncTCP: aca NO se puede llamar al servicio de
  // transcripcion, una conexion TLS bloqueante la colgaria.
  server.on(
      "/mic", HTTP_POST,
      [](AsyncWebServerRequest *request) {
        if (!tokenOk(request)) {
          request->send(401, "text/plain", "sin sesion: volve a entrar con la contrasena");
          return;
        }
        if (phoneBuf && phoneLen > 0) {
          remote.phoneAudioReady = true;
          request->send(200, "text/plain", "ok");
        } else {
          request->send(400, "text/plain", "audio vacio o demasiado grande");
        }
      },
      NULL,
      [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index,
         size_t total) {
        if (index == 0) {
          if (!tokenOk(request)) {
            Serial.println("Web: subida de audio rechazada (sin sesion valida)");
            return;
          }
          // Empieza una subida nueva: se tira lo que hubiera quedado colgado.
          if (phoneBuf) { free(phoneBuf); phoneBuf = nullptr; }
          phoneLen = 0;
          phoneCap = 0;

          // Sin Content-Length no se puede reservar de una, y crecer el buffer
          // a mano mientras entra el audio fragmenta el heap justo antes de
          // necesitar una conexion TLS. Se rechaza y listo.
          if (total == 0 || total > PHONE_AUDIO_MAX_BYTES) {
            Serial.printf("Web: audio del celular rechazado (%u bytes)\n",
                          (unsigned)total);
            return;
          }
          // El turno anterior todavia no se proceso: no se pisa.
          if (remote.phoneAudioReady) return;

          // El navegador dice en que formato grabo. Android Chrome da webm/opus;
          // Safari da mp4/aac. El servicio de transcripcion los acepta a los
          // dos, pero deduce el formato de la EXTENSION del archivo, asi que
          // mandarle un mp4 llamado .webm falla con un 400 confuso.
          strlcpy(phoneName, PHONE_AUDIO_FILENAME, sizeof(phoneName));
          strlcpy(phoneMime, PHONE_AUDIO_MIME, sizeof(phoneMime));
          if (request->hasParam("fmt")) {
            const String fmt = request->getParam("fmt")->value();
            if (fmt == "mp4") {
              strlcpy(phoneName, "audio.mp4", sizeof(phoneName));
              strlcpy(phoneMime, "audio/mp4", sizeof(phoneMime));
            } else if (fmt == "ogg") {
              strlcpy(phoneName, "audio.ogg", sizeof(phoneName));
              strlcpy(phoneMime, "audio/ogg", sizeof(phoneMime));
            } else if (fmt == "wav") {
              strlcpy(phoneName, "audio.wav", sizeof(phoneName));
              strlcpy(phoneMime, "audio/wav", sizeof(phoneMime));
            }
          }

          phoneBuf = (uint8_t *)malloc(total);
          if (!phoneBuf) {
            Serial.println("Web: sin memoria para el audio del celular");
            return;
          }
          phoneCap = total;
        }
        if (!phoneBuf) return;
        if (phoneLen + len > phoneCap) return;
        memcpy(phoneBuf + phoneLen, data, len);
        phoneLen += len;
      });

  server.onNotFound([](AsyncWebServerRequest *request) {
    request->send(404, "text/plain", "no existe");
  });

  server.begin();

  Serial.println();
  Serial.println("=============================================");
  Serial.printf("  Control web listo:  http://%s/\n", WiFi.localIP().toString().c_str());
  Serial.printf("  o tambien:          http://%s.local/\n", ROBOT_HOSTNAME);
  Serial.println("  (el celular tiene que estar en la MISMA red WiFi)");
  if (!remoteAuthConfigured()) {
    Serial.println("  !! SIN CONTRASENA: el control web queda BLOQUEADO.");
    Serial.println("     Definir WEB_PASSWORD en src/secrets.h (8+ caracteres).");
  }
  Serial.println("=============================================");
  Serial.println();
}

void remoteLoop(unsigned long now) {
  // Los clientes que se van sin avisar (el celular se bloquea, se corta el
  // WiFi) quedan como sockets abiertos hasta que alguien los limpia. Sin esto
  // se acumulan y a la cuarta o quinta reconexion el servidor no acepta mas.
  ws.cleanupClients();

  // Clientes que se conectaron y nunca se autenticaron. Se cortan: si no,
  // alguien en la red podria abrir ocho conexiones y dejar sin lugar al dueño.
  uint32_t cortar[AUTH_SLOTS];
  uint8_t n = 0;
  portENTER_CRITICAL(&authMux);
  for (int i = 0; i < AUTH_SLOTS; i++) {
    if (slots[i].id && !slots[i].authed && now - slots[i].since > AUTH_TIMEOUT_MS) {
      cortar[n++] = slots[i].id;
      slots[i].since = now;   // no volver a cortarlo en cada vuelta del loop
    }
  }
  portEXIT_CRITICAL(&authMux);
  for (uint8_t k = 0; k < n; k++) {
    AsyncWebSocketClient *c = ws.client(cortar[k]);
    if (c) c->close();
  }
}

bool remoteHasClients() {
  uint32_t ids[AUTH_SLOTS];
  return authedIds(ids) > 0;
}

String remoteTakeSayText() {
  if (!remote.sayRequested) return "";
  String out = sayText;
  sayText[0] = 0;
  remote.sayRequested = false;
  return out;
}

bool remoteTakePhoneAudio(uint8_t **bufOut, size_t *lenOut,
                          char *nameOut, size_t nameCap,
                          char *mimeOut, size_t mimeCap) {
  if (!remote.phoneAudioReady || !phoneBuf || phoneLen == 0) return false;
  *bufOut = phoneBuf;
  *lenOut = phoneLen;
  if (nameOut) strlcpy(nameOut, phoneName, nameCap);
  if (mimeOut) strlcpy(mimeOut, phoneMime, mimeCap);
  // Se sueltan las referencias ANTES de bajar la bandera: a partir de aca el
  // buffer es de quien llamo, y una subida nueva empieza de cero sin tocarlo.
  phoneBuf = nullptr;
  phoneLen = 0;
  phoneCap = 0;
  remote.phoneAudioReady = false;
  return true;
}

void remoteBroadcast(const char *face, long distanceCm, bool busy) {
  if (!remoteHasClients()) return;

  char buf[192];
  snprintf(buf, sizeof(buf),
           "{\"t\":\"st\",\"mode\":\"%s\",\"dist\":%ld,\"face\":\"%s\","
           "\"mute\":%s,\"busy\":%s,\"armL\":%d,\"armR\":%d,\"src\":\"%s\"}",
           remote.mode == MODE_CONTROL ? "control"
                                       : (remote.mode == MODE_AUTO ? "auto" : "off"),
           distanceCm, face,
           remote.micMuted ? "true" : "false",
           busy ? "true" : "false",
           (int)remote.armL, (int)remote.armR,
           remote.micSource == MIC_PHONE ? "phone" : "robot");

  textAuthed(buf);
}

// ======================= ESTADO EN VIVO =======================

void remoteStatus(const char *phase, const String &detail) {
  if (!remoteHasClients()) return;

  DynamicJsonDocument doc(detail.length() + 128);
  doc["t"] = "ph";
  doc["p"] = phase;
  doc["d"] = detail;

  String out;
  serializeJson(doc, out);
  textAuthed(out.c_str());
}

void remoteMicLevel(int rms, int threshold) {
  if (!remoteHasClients()) return;

  // Limitador. Durante la grabacion esto entra ~30 veces por segundo, y cada
  // envio encola un paquete TCP: sin limitar, la cola de AsyncTCP se llena,
  // empieza a descartar y de paso le roba tiempo al I2S del microfono.
  static unsigned long lastAt = 0;
  const unsigned long now = millis();
  if (now - lastAt < 80) return;
  lastAt = now;

  char buf[80];
  snprintf(buf, sizeof(buf), "{\"t\":\"mic\",\"v\":%d,\"th\":%d}", rms, threshold);
  textAuthed(buf);
}

// ======================= PANEL DE DIAGNOSTICO =======================

#define DIAG_MAX_ROWS  12

struct DiagRow {
  const char *key;
  String value;
  int ok;
};

static DiagRow diagRows[DIAG_MAX_ROWS];
static uint8_t diagCount = 0;

void remoteDiag(const char *key, const String &value, int ok) {
  // Si la fila ya existe se actualiza en el lugar. Asi "MICROFONO" no aparece
  // cinco veces despues de cinco autotests.
  for (uint8_t i = 0; i < diagCount; i++) {
    if (!strcmp(diagRows[i].key, key)) {
      diagRows[i].value = value;
      diagRows[i].ok = ok;
      remoteSendDiag(nullptr);
      return;
    }
  }
  if (diagCount >= DIAG_MAX_ROWS) return;
  diagRows[diagCount].key = key;
  diagRows[diagCount].value = value;
  diagRows[diagCount].ok = ok;
  diagCount++;
  remoteSendDiag(nullptr);
}

void remoteSendDiag(void *clientOrNull) {
  if (!clientOrNull && !remoteHasClients()) return;

  DynamicJsonDocument doc(1792);
  doc["t"] = "diag";
  JsonArray rows = doc.createNestedArray("rows");
  for (uint8_t i = 0; i < diagCount; i++) {
    JsonArray r = rows.createNestedArray();
    r.add(diagRows[i].key);
    r.add(diagRows[i].value);
    r.add(diagRows[i].ok);
  }

  String out;
  serializeJson(doc, out);

  // Con cliente explicito solo lo llama handleAuth(), despues de verificarlo.
  if (clientOrNull) ((AsyncWebSocketClient *)clientOrNull)->text(out);
  else              textAuthed(out.c_str());
}

void remoteLog(const char *who, const String &msg) {
  if (!remoteHasClients()) return;

  // El texto que sale del LLM o del transcriptor puede traer comillas o barras
  // y romperia el JSON del lado del navegador. ArduinoJson se encarga del
  // escapado; armarlo a mano con snprintf() era pedir problemas.
  DynamicJsonDocument doc(msg.length() + 128);
  doc["t"] = "log";
  doc["who"] = who;
  doc["msg"] = msg;

  String out;
  serializeJson(doc, out);
  textAuthed(out.c_str());
}

#include "remote.h"
#include "config.h"
#include "webui.h"

#include <WiFi.h>
#include <ESPmDNS.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>

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

// ======================= COMANDOS QUE LLEGAN DEL CELULAR =======================
//
// OJO: esto corre en la tarea de AsyncTCP. Ver la nota de arriba en remote.h.
// Aca solo se anota lo que se pidio; quien mueve algo es loop().

static int16_t clampArm(int v) {
  if (v < ARM_DEG_MIN) return ARM_DEG_MIN;
  if (v > ARM_DEG_MAX) return ARM_DEG_MAX;
  return (int16_t)v;
}

static void handleCommand(const uint8_t *data, size_t len) {
  // Los mensajes son de 30 a 60 bytes. StaticJsonDocument va en la pila de la
  // tarea de AsyncTCP (por eso platformio.ini le sube la pila a 16KB) y no
  // toca el heap, que es lo que uno quiere en un callback que se llama 20
  // veces por segundo: con DynamicJsonDocument el heap se fragmentaria y a los
  // pocos minutos fallaria la conexion TLS del proximo turno de voz.
  StaticJsonDocument<256> doc;
  if (deserializeJson(doc, data, len)) return;

  const char *t = doc["t"];
  if (!t) return;

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
    case WS_EVT_CONNECT:
      Serial.printf("Web: se conecto un cliente (#%u) desde %s\n",
                    client->id(), client->remoteIP().toString().c_str());
      // Que se conecte un celular no autoriza a mover nada: el joystick sigue
      // vencido hasta que mande su primera posicion.
      remote.joyX = 0;
      remote.joyY = 0;
      remote.joyAt = 0;
      // El reporte de arranque se manda de nuevo a cada cliente que entra: el
      // celular casi siempre se conecta despues de que el robot booteo, y si no
      // se reenviara, el panel de diagnostico quedaria vacio justo cuando mas
      // se lo necesita.
      remoteSendDiag(client);
      break;

    case WS_EVT_DISCONNECT:
      Serial.printf("Web: se fue el cliente #%u\n", client->id());
      remote.joyX = 0;
      remote.joyY = 0;
      remote.joyAt = 0;
      break;

    case WS_EVT_DATA: {
      AwsFrameInfo *info = (AwsFrameInfo *)arg;
      // Los comandos son cortisimos, siempre entran en un solo frame de texto.
      // Un frame partido en pedazos solo puede venir de otra cosa: se ignora.
      if (info->final && info->index == 0 && info->len == len &&
          info->opcode == WS_TEXT) {
        handleCommand(data, len);
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

  // send_P lee la pagina directo de la flash, de a pedazos. Con send() habria
  // que copiar los 14KB del HTML a un String en RAM primero.
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send_P(200, "text/html; charset=utf-8", INDEX_HTML);
  });

  // Los celulares piden un favicon apenas abren la pagina. Sin esta ruta cada
  // carga deja un 404 en el log serie que no significa nada.
  server.on("/favicon.ico", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(204);
  });

  // ---- audio grabado con el microfono del celular ----
  //
  // Va por HTTP y no por el WebSocket a proposito: son 15 a 20KB de una, y el
  // WebSocket esta para mensajes de 30 bytes que tienen que llegar ya. Meterle
  // el audio por ahi le agrega retardo justo al joystick.
  //
  // El cuerpo llega en pedazos; se junta en un buffer y recien cuando termina se
  // le avisa a loop(). Aca NO se puede llamar al servicio de transcripcion: esto
  // corre en la tarea de AsyncTCP y una conexion TLS bloqueante la colgaria.
  server.on(
      "/mic", HTTP_POST,
      [](AsyncWebServerRequest *request) {
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
  Serial.println("=============================================");
  Serial.println();
}

void remoteLoop(unsigned long now) {
  (void)now;
  // Los clientes que se van sin avisar (el celular se bloquea, se corta el
  // WiFi) quedan como sockets abiertos hasta que alguien los limpia. Sin esto
  // se acumulan y a la cuarta o quinta reconexion el servidor no acepta mas.
  ws.cleanupClients();
}

bool remoteHasClients() {
  return ws.count() > 0;
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
  if (ws.count() == 0) return;

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

  ws.textAll(buf);
}

// ======================= ESTADO EN VIVO =======================

void remoteStatus(const char *phase, const String &detail) {
  if (ws.count() == 0) return;

  DynamicJsonDocument doc(detail.length() + 128);
  doc["t"] = "ph";
  doc["p"] = phase;
  doc["d"] = detail;

  String out;
  serializeJson(doc, out);
  ws.textAll(out);
}

void remoteMicLevel(int rms, int threshold) {
  if (ws.count() == 0) return;

  // Limitador. Durante la grabacion esto entra ~30 veces por segundo, y cada
  // textAll() encola un paquete TCP: sin limitar, la cola de AsyncTCP se llena,
  // empieza a descartar y de paso le roba tiempo al I2S del microfono.
  static unsigned long lastAt = 0;
  const unsigned long now = millis();
  if (now - lastAt < 80) return;
  lastAt = now;

  char buf[80];
  snprintf(buf, sizeof(buf), "{\"t\":\"mic\",\"v\":%d,\"th\":%d}", rms, threshold);
  ws.textAll(buf);
}

// ======================= PANEL DE DIAGNOSTICO =======================

#define DIAG_MAX_ROWS  10

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
  if (ws.count() == 0) return;

  DynamicJsonDocument doc(1536);
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

  if (clientOrNull) ((AsyncWebSocketClient *)clientOrNull)->text(out);
  else              ws.textAll(out);
}

void remoteLog(const char *who, const String &msg) {
  if (ws.count() == 0) return;

  // El texto que sale del LLM o del transcriptor puede traer comillas o barras
  // y romperia el JSON del lado del navegador. ArduinoJson se encarga del
  // escapado; armarlo a mano con snprintf() era pedir problemas.
  DynamicJsonDocument doc(msg.length() + 128);
  doc["t"] = "log";
  doc["who"] = who;
  doc["msg"] = msg;

  String out;
  serializeJson(doc, out);
  ws.textAll(out);
}

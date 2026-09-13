#include "voice.h"
#include "audio.h"
#include "config.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

#include "api_certs.h"

// ======================= TLS =======================
//
// Todas las conexiones a la API pasan por aca. Antes no se validaba el
// certificado: la conexion iba cifrada, pero sin comprobar con quien se
// hablaba. En una red compartida (la WiFi de un evento, por ejemplo) alguien
// podia hacerse pasar por api.openai.com y quedarse con la API key, que viaja
// en la cabecera Authorization.
//
// Ahora se valida contra los certificados de api_certs.h. Si OpenAI cambia de
// autoridad certificante, las llamadas fallan con un error claro -mejor que
// seguir mandando la key a ciegas- y se actualiza con tools/actualizar_certs.py.
static void configurarTls(WiFiClientSecure &client) {
#if API_TLS_INSECURE
  client.setInsecure();
#else
  client.setCACert(API_ROOT_CA);
#endif
}

// Traduce un fallo de conexion a algo accionable. Sin esto, un certificado
// vencido y "no hay internet" se ven iguales.
static String motivoFalloConexion(WiFiClientSecure &client) {
  char err[96];
  err[0] = 0;
  const int code = client.lastError(err, sizeof(err));
  Serial.printf("TLS: fallo la conexion (%d) %s\n", code, err);
  if (code == 0) {
    return String("No se pudo conectar a " API_HOST " (sin internet?)");
  }
  return String("No se pudo verificar la conexion segura con " API_HOST
                " (hora sin sincronizar o cambio el certificado: ver tools/actualizar_certs.py)");
}

// ======================= HELPERS =======================

static void putLE16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)(v & 0xFF);
  p[1] = (uint8_t)(v >> 8);
}

static void putLE32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v & 0xFF);
  p[1] = (uint8_t)((v >> 8) & 0xFF);
  p[2] = (uint8_t)((v >> 16) & 0xFF);
  p[3] = (uint8_t)((v >> 24) & 0xFF);
}

// Cabecera WAV de 44 bytes, PCM 16 bits mono. El servicio de transcripcion
// necesita un archivo con formato, no PCM suelto.
static void writeWavHeader(Client &out, uint32_t dataBytes, uint32_t sampleRate) {
  uint8_t h[44];
  memcpy(h, "RIFF", 4);
  putLE32(h + 4, 36 + dataBytes);
  memcpy(h + 8, "WAVEfmt ", 8);
  putLE32(h + 16, 16);                  // tamano del bloque fmt
  putLE16(h + 20, 1);                   // 1 = PCM sin comprimir
  putLE16(h + 22, 1);                   // mono
  putLE32(h + 24, sampleRate);
  putLE32(h + 28, sampleRate * 2);      // byte rate
  putLE16(h + 32, 2);                   // block align
  putLE16(h + 34, 16);                  // bits por muestra
  memcpy(h + 36, "data", 4);
  putLE32(h + 40, dataBytes);
  out.write(h, sizeof(h));
}

// Traduce los codigos de error mas comunes a algo accionable, para no tener
// que ir a buscar que significa cada numero.
static void explicarCodigo(int code) {
  switch (code) {
    case 401:
      Serial.println("   -> 401: la API key no es valida, esta vacia, o quedo el");
      Serial.println("           texto de relleno. Revisar LLM_API_KEY en secrets.h");
      break;
    case 403:
      Serial.println("   -> 403: la key es valida pero no tiene permiso para este");
      Serial.println("           endpoint (puede ser una key restringida)");
      break;
    case 429:
      Serial.println("   -> 429: sin credito en la cuenta, o demasiados pedidos seguidos");
      break;
    case 400:
      Serial.println("   -> 400: el pedido salio mal armado desde la placa");
      break;
    case -1:
      Serial.println("   -> -1: no se pudo conectar. Sin internet, la hora no se");
      Serial.println("          sincronizo, o cambio el certificado de la API");
      Serial.println("          (actualizarlo con tools/actualizar_certs.py)");
      break;
    default:
      break;
  }
}

// Salta la cabecera HTTP y devuelve el cuerpo.
//
// Se lee crudo y despues se recorta entre la primera llave y la ultima, porque
// el servidor puede responder con Transfer-Encoding: chunked y en ese caso el
// cuerpo trae intercalados los tamanos de cada bloque en hexadecimal. Para las
// respuestas cortas de este proyecto (un JSON de pocos cientos de bytes, que
// viaja en un solo bloque) el recorte alcanza y evita escribir un parser de
// chunked a mano.
static String readJsonBody(WiFiClientSecure &client, int *statusOut) {
  // Se lee TODO lo que llegue -cabeceras incluidas- y recien despues se
  // interpreta.
  //
  // Leer la primera linea por separado NO funciona: cuando se la pide, el
  // servidor todavia esta transcribiendo y no mando un solo byte, asi que la
  // lectura vuelve vacia y el codigo de estado queda en -1 aunque la respuesta
  // despues llegue perfecta.
  String raw;
  raw.reserve(1024);
  unsigned long lastData = millis();
  while (millis() - lastData < 15000) {
    while (client.available()) {
      raw += (char)client.read();
      lastData = millis();
    }
    if (!client.connected() && !client.available()) break;
    delay(10);
  }

  // Codigo de estado, de la primera linea: "HTTP/1.1 200 OK"
  *statusOut = -1;
  if (raw.startsWith("HTTP/")) {
    int sp = raw.indexOf(' ');
    if (sp > 0) *statusOut = raw.substring(sp + 1, sp + 4).toInt();
  }

  int start = raw.indexOf('{');
  int end = raw.lastIndexOf('}');
  if (start < 0 || end <= start) return "";
  return raw.substring(start, end + 1);
}

// ======================= REPORTE DE AVANCE =======================

VoicePhaseFn voiceOnPhase = nullptr;
VoiceLevelFn voiceOnLevel = nullptr;

static void phase(const char *p, const String &detail = String()) {
  if (voiceOnPhase) voiceOnPhase(p, detail);
}

// ======================= AUTOTEST DEL MICROFONO =======================
//
// Mide el microfono sin tocar la red. Es la misma medicion que hace
// MIC_TEST_MODE, pero se dispara desde la web sin recompilar ni reflashear,
// que es la diferencia entre diagnosticar en 5 segundos y en 5 minutos.
// Devuelve valores CRUDOS del ADC de 12 bits, sin filtrar: asi se ve el nivel
// de reposo, que es lo que delata la alimentacion mal conectada.

bool micSelfTest(uint16_t ms, uint16_t *restOut, uint16_t *ppOut) {
  if (restOut) *restOut = 0;
  if (ppOut) *ppOut = 0;

  if (!micStart()) return false;

  static uint16_t buf[512];
  uint16_t minV = 4095, maxV = 0;
  uint32_t suma = 0, total = 0;

  const unsigned long t0 = millis();
  while (millis() - t0 < ms) {
    size_t n = micReadRaw(buf, 512);
    if (n == 0) break;
    for (size_t i = 0; i < n; i++) {
      const uint16_t v = buf[i];
      if (v < minV) minV = v;
      if (v > maxV) maxV = v;
      suma += v;
    }
    total += n;
  }
  micStop();

  if (total == 0) return false;
  if (restOut) *restOut = (uint16_t)(suma / total);
  if (ppOut) *ppOut = (uint16_t)(maxV - minV);
  return true;
}

// ======================= 1. VOZ -> TEXTO =======================

// Tamano del cuadro de analisis del VAD. 512 muestras a 16kHz son 32ms, que es
// el largo tipico con el que se mide energia de voz: lo bastante corto para
// reaccionar rapido y lo bastante largo para que el RMS no salte con cada
// ciclo de la onda.
#define VAD_FRAME_SAMPLES   512

// Cuadros de "antes del disparo" que se guardan y se mandan igual.
//
// El VAD necesita VAD_TRIGGER_FRAMES cuadros seguidos con voz para decidir que
// alguien esta hablando, asi que para cuando decide ya pasaron ~100ms de la
// primera silaba. Si esos cuadros se tiraran, cada frase llegaria al
// transcriptor sin su arranque ("ola" en vez de "hola"). Se guardan en un
// buffer circular y se anteponen a la grabacion.
#define VAD_PREROLL_FRAMES  4

// RMS de un bloque de muestras. Se acumula en uint64 porque el cuadrado de una
// muestra de 16 bits llega a 1073741824 y 512 de esos desbordan un uint32.
static uint32_t blockRms(const int16_t *buf, size_t n) {
  if (n == 0) return 0;
  uint64_t acc = 0;
  for (size_t i = 0; i < n; i++) {
    int32_t v = buf[i];
    acc += (uint64_t)(v * v);
  }
  return (uint32_t)sqrt((double)(acc / n));
}

String sttRecordAndTranscribe(uint16_t seconds, void (*onTick)(),
                              SttResult *resultOut) {
  if (resultOut) *resultOut = STT_OK;

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("STT: sin WiFi");
    phase("err", "Sin WiFi");
    if (resultOut) *resultOut = STT_NO_WIFI;
    return "";
  }

  const uint32_t totalSamples = (uint32_t)MIC_SAMPLE_RATE * seconds;
  const uint32_t dataBytes = totalSamples * 2;

  WiFiClientSecure client;
  configurarTls(client);
  client.setTimeout(20);  // segundos

  phase("conn", "Conectando con el servicio de voz...");

  if (!client.connect(API_HOST, 443)) {
    Serial.println("STT: no se pudo conectar");
    phase("err", motivoFalloConexion(client));
    if (resultOut) *resultOut = STT_NO_CONNECT;
    return "";
  }

  const String boundary = "----walleboundary7391";

  String head;
  head += "--" + boundary + "\r\n";
  head += "Content-Disposition: form-data; name=\"model\"\r\n\r\n";
  head += STT_MODEL;
  head += "\r\n--" + boundary + "\r\n";
  head += "Content-Disposition: form-data; name=\"language\"\r\n\r\n";
  head += STT_LANGUAGE;
  head += "\r\n--" + boundary + "\r\n";
  head += "Content-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\n";
  head += "Content-Type: audio/wav\r\n\r\n";

  const String tail = "\r\n--" + boundary + "--\r\n";

  // Este es el truco que hace que todo entre en la RAM: como la duracion de la
  // grabacion es fija, el tamano total del cuerpo se conoce ANTES de grabar.
  const uint32_t contentLength = head.length() + 44 + dataBytes + tail.length();

  client.print("POST " STT_API_PATH " HTTP/1.1\r\n");
  client.print("Host: " API_HOST "\r\n");
  client.print("Authorization: Bearer " LLM_API_KEY "\r\n");
  client.print("Content-Type: multipart/form-data; boundary=" + boundary + "\r\n");
  client.print("Content-Length: " + String(contentLength) + "\r\n");
  client.print("Connection: close\r\n\r\n");

  client.print(head);
  writeWavHeader(client, dataBytes, MIC_SAMPLE_RATE);

  static int16_t chunk[VAD_FRAME_SAMPLES];
  uint32_t sent = 0;
  int32_t peak = 0;

  if (!micStart()) {
    // El driver no arranco. Esto NO es un cable suelto: es configuracion, y hay
    // que decirlo distinto o se pierde media tarde revisando soldaduras.
    client.stop();
    const String why = String("El microfono no arranco: ") + micLastError();
    Serial.println(why);
    phase("err", why);
    if (resultOut) *resultOut = STT_API_ERROR;
    return "";
  }

  // --------------------- ESPERA DE VOZ (VAD) ---------------------
  //
  // La conexion TLS ya esta abierta y las cabeceras ya salieron: recien ahora
  // se escucha. El orden importa. Al reves -escuchar primero y conectar
  // despues- el saludo TLS se come uno o dos segundos justo cuando la persona
  // ya empezo a hablar, y la frase llega cortada por delante.
  //
  // Si el VAD se cansa de esperar, se corta la conexion a medio cuerpo. Es
  // valido: el Content-Length ya se anuncio pero nunca se completa, y el
  // servidor descarta el pedido incompleto al ver el socket cerrado.
#if VAD_ENABLED
  {
    static int16_t preroll[VAD_PREROLL_FRAMES * VAD_FRAME_SAMPLES];
    uint8_t prerollSlot = 0;      // proximo cuadro a escribir
    uint8_t prerollCount = 0;     // cuantos hay guardados (tope: PREROLL_FRAMES)

    float noiseFloor = 0.0f;
    uint16_t warmupSeen = 0;
    uint8_t voiceFrames = 0;
    bool triggered = false;

    phase("wait", "Te escucho: hablale ahora.");

    const unsigned long vadStart = millis();

    while (millis() - vadStart < VAD_WAIT_MS) {
      size_t got = micRead(chunk, VAD_FRAME_SAMPLES);
      if (got == 0) break;
      if (onTick) onTick();

      const uint32_t rms = blockRms(chunk, got);

      // Los primeros cuadros hacen dos cosas a la vez: le dan tiempo al
      // estimador de continua de audio.cpp a engancharse, y de paso miden como
      // suena el ambiente. Ese promedio es el piso de ruido inicial.
      if (warmupSeen < VAD_WARMUP_FRAMES) {
        noiseFloor += ((float)rms - noiseFloor) / (float)(warmupSeen + 1);
        warmupSeen++;
        // Durante el warmup igual se reporta el nivel: si el microfono esta
        // muerto, se ve en cero ya aca y no hace falta esperar a nada mas.
        if (voiceOnLevel) voiceOnLevel((int)rms, 0, false);
        continue;
      }

      // El piso baja rapido y sube despacio. Asi, si el ambiente se calma, el
      // umbral lo sigue enseguida; y si alguien empieza a hablar, la voz no se
      // "absorbe" dentro del propio piso antes de llegar a disparar.
      if ((float)rms < noiseFloor) noiseFloor += ((float)rms - noiseFloor) * 0.5f;
      else                         noiseFloor += ((float)rms - noiseFloor) * 0.03f;

      // Tope al piso de ruido: sin esto, si el warmup agarra justo un portazo,
      // el umbral queda por las nubes y el robot se vuelve sordo por el resto
      // del turno.
      if (noiseFloor > 1200.0f) noiseFloor = 1200.0f;

      float threshold = noiseFloor * VAD_SNR_FACTOR;
      if (threshold < VAD_RMS_MIN) threshold = VAD_RMS_MIN;

      // Guardar el cuadro en el buffer circular ANTES de decidir: si este es el
      // que dispara, ya quedo adentro y se manda igual.
      memcpy(preroll + (size_t)prerollSlot * VAD_FRAME_SAMPLES, chunk,
             got * sizeof(int16_t));
      prerollSlot = (prerollSlot + 1) % VAD_PREROLL_FRAMES;
      if (prerollCount < VAD_PREROLL_FRAMES) prerollCount++;

      if (voiceOnLevel) voiceOnLevel((int)rms, (int)threshold, (float)rms > threshold);

      if ((float)rms > threshold) {
        voiceFrames++;
        if (voiceFrames >= VAD_TRIGGER_FRAMES) {
          triggered = true;
          break;
        }
      } else {
        voiceFrames = 0;   // tienen que ser seguidos, no sueltos
      }
    }

    if (!triggered) {
      micStop();
      client.stop();
      Serial.println("STT: el VAD no escucho voz, turno cancelado (no se gasto API).");
      phase("err", "No te escuche hablar (ruido de fondo ~" +
                       String((int)noiseFloor) + ", hacia falta " +
                       String((int)(noiseFloor * VAD_SNR_FACTOR)) + ")");
      if (resultOut) *resultOut = STT_NO_VOICE;
      return "";
    }

    Serial.printf("STT: voz detectada (piso de ruido ~%d)\n", (int)noiseFloor);

    // Volcar el pre-roll en orden cronologico. El cuadro mas viejo es el que
    // esta en prerollSlot cuando el buffer ya dio la vuelta.
    const uint8_t oldest = (prerollCount == VAD_PREROLL_FRAMES) ? prerollSlot : 0;
    for (uint8_t i = 0; i < prerollCount && sent < totalSamples; i++) {
      const int16_t *frame =
          preroll + (size_t)((oldest + i) % VAD_PREROLL_FRAMES) * VAD_FRAME_SAMPLES;

      uint32_t n = VAD_FRAME_SAMPLES;
      if (n > totalSamples - sent) n = totalSamples - sent;

      for (uint32_t j = 0; j < n; j++) {
        int32_t a = frame[j];
        if (a < 0) a = -a;
        if (a > peak) peak = a;
      }
      client.write((const uint8_t *)frame, n * 2);
      sent += n;
    }
  }
#endif  // VAD_ENABLED

  // --------------------- GRABACION ---------------------
  // Grabar y enviar al mismo tiempo, midiendo el pico de paso.

  phase("rec", "Grabando...");

  const unsigned long recStart = millis();

  while (sent < totalSamples) {
    uint32_t want = totalSamples - sent;
    if (want > VAD_FRAME_SAMPLES) want = VAD_FRAME_SAMPLES;

    size_t got = micRead(chunk, want);
    if (got == 0) break;

    int32_t framePeak = 0;
    for (size_t j = 0; j < got; j++) {
      int32_t a = chunk[j];
      if (a < 0) a = -a;
      if (a > peak) peak = a;
      if (a > framePeak) framePeak = a;
    }
    if (voiceOnLevel) voiceOnLevel((int)framePeak, MIC_SILENCE_PEAK, true);

    client.write((const uint8_t *)chunk, got * 2);
    sent += got;
    if (onTick) onTick();
  }
  micStop();

  // Verificacion de la frecuencia real de muestreo. Si juntar 'seconds'
  // segundos de muestras tarda la mitad de ese tiempo, el ADC esta corriendo al
  // doble de lo configurado y el WAV declara una frecuencia que no es: la voz
  // le llega al transcriptor a media velocidad y no la entiende.
  //
  // Se mide solo el tramo grabado despues del VAD; el pre-roll ya estaba en
  // memoria y se envio de golpe, asi que no cuenta para el tiempo.
  const unsigned long recMs = millis() - recStart;
  Serial.printf("STT: pico %d | %u muestras en %lu ms (esperado ~%u ms)\n",
                (int)peak, (unsigned)sent, recMs, (unsigned)seconds * 1000);
  if (recMs > 0 && (recMs < (unsigned long)seconds * 700 ||
                    recMs > (unsigned long)seconds * 1400)) {
    Serial.println("     OJO: la duracion no cuadra. La frecuencia real de");
    Serial.println("     muestreo no es la que declara el WAV.");
  }

  // Si la grabacion se corto antes de tiempo hay que completar igual, porque el
  // Content-Length ya se anuncio y el servidor lo espera entero.
  if (sent < totalSamples) {
    memset(chunk, 0, sizeof(chunk));
    while (sent < totalSamples) {
      uint32_t want = totalSamples - sent;
      if (want > VAD_FRAME_SAMPLES) want = VAD_FRAME_SAMPLES;
      client.write((const uint8_t *)chunk, want * 2);
      sent += want;
    }
  }

  // Segunda red: el VAD dijo que habia voz, pero si el pico de toda la
  // grabacion igual quedo bajo, no vale la pena mirar la respuesta. Ante audio
  // casi mudo el transcriptor devuelve frases inventadas y confunde mas de lo
  // que ayuda.
  if (peak < MIC_SILENCE_PEAK) {
    client.stop();
    phase("err", "Se escucho muy bajo (pico " + String((int)peak) +
                     ", hacia falta " + String(MIC_SILENCE_PEAK) + ")");
    Serial.println("STT: eso fue practicamente silencio, el microfono no capto voz.");
    Serial.println("     Revisar el cableado con MIC_TEST_MODE en 1 (config.h).");
    if (resultOut) *resultOut = STT_TOO_QUIET;
    return "";
  }

  client.print(tail);

  phase("stt", "Transcribiendo lo que dijiste...");

  int status = -1;
  String body = readJsonBody(client, &status);
  client.stop();

  if (status != 200) {
    Serial.printf("STT: la API respondio %d\n", status);
    if (body.length() > 0) {
      // El cuerpo del error suele decir exactamente que paso.
      Serial.print("   ");
      Serial.println(body.substring(0, 200));
    }
    explicarCodigo(status);
    phase("err", "La transcripcion devolvio HTTP " + String(status));
    if (resultOut) *resultOut = STT_API_ERROR;
    return "";
  }

  if (body.length() == 0) {
    Serial.println("STT: respuesta vacia");
    phase("err", "La transcripcion vino vacia");
    if (resultOut) *resultOut = STT_API_ERROR;
    return "";
  }

  StaticJsonDocument<64> filter;
  filter["text"] = true;

  DynamicJsonDocument doc(1024);
  DeserializationError err =
      deserializeJson(doc, body, DeserializationOption::Filter(filter));

  if (err) {
    Serial.print("STT: no se pudo parsear: ");
    Serial.println(err.c_str());
    if (resultOut) *resultOut = STT_API_ERROR;
    return "";
  }

  return doc["text"].as<String>();
}

// ======================= 1b. AUDIO YA GRABADO -> TEXTO =======================
//
// Misma subida multipart que arriba, pero con el archivo ya completo en RAM.
// Sale mas simple justamente porque no hay que grabar mientras se sube: el
// tamano se conoce de entrada y no hay VAD que esperar (el navegador ya decidio
// cuando empezar y cuando cortar).

String sttTranscribeBuffer(const uint8_t *data, size_t len, const char *filename,
                           const char *contentType, SttResult *resultOut) {
  if (resultOut) *resultOut = STT_OK;

  if (WiFi.status() != WL_CONNECTED) {
    phase("err", "Sin WiFi");
    if (resultOut) *resultOut = STT_NO_WIFI;
    return "";
  }
  if (!data || len == 0) {
    phase("err", "El audio del celular llego vacio");
    if (resultOut) *resultOut = STT_TOO_QUIET;
    return "";
  }

  WiFiClientSecure client;
  configurarTls(client);
  client.setTimeout(20);

  phase("conn", "Subiendo el audio del celular...");

  if (!client.connect(API_HOST, 443)) {
    phase("err", motivoFalloConexion(client));
    if (resultOut) *resultOut = STT_NO_CONNECT;
    return "";
  }

  const String boundary = "----walleboundary7391";

  String head;
  head += "--" + boundary + "\r\n";
  head += "Content-Disposition: form-data; name=\"model\"\r\n\r\n";
  head += STT_MODEL;
  head += "\r\n--" + boundary + "\r\n";
  head += "Content-Disposition: form-data; name=\"language\"\r\n\r\n";
  head += STT_LANGUAGE;
  head += "\r\n--" + boundary + "\r\n";
  head += "Content-Disposition: form-data; name=\"file\"; filename=\"";
  head += filename;
  head += "\"\r\n";
  head += "Content-Type: ";
  head += contentType;
  head += "\r\n\r\n";

  const String tail = "\r\n--" + boundary + "--\r\n";
  const uint32_t contentLength = head.length() + len + tail.length();

  client.print("POST " STT_API_PATH " HTTP/1.1\r\n");
  client.print("Host: " API_HOST "\r\n");
  client.print("Authorization: Bearer " LLM_API_KEY "\r\n");
  client.print("Content-Type: multipart/form-data; boundary=" + boundary + "\r\n");
  client.print("Content-Length: " + String(contentLength) + "\r\n");
  client.print("Connection: close\r\n\r\n");

  client.print(head);

  // De a pedazos y no de una: un write() de 90KB sobre TLS obliga a la libreria
  // a quedarse con un buffer enorme, y justo estamos cortos de heap por tener
  // el audio entero en memoria.
  size_t off = 0;
  while (off < len) {
    size_t n = len - off;
    if (n > 1024) n = 1024;
    if (client.write(data + off, n) == 0) break;
    off += n;
  }

  client.print(tail);

  phase("stt", "Transcribiendo lo que dijiste...");

  int status = -1;
  String body = readJsonBody(client, &status);
  client.stop();

  if (status != 200) {
    Serial.printf("STT (celular): la API respondio %d\n", status);
    if (body.length() > 0) {
      Serial.print("   ");
      Serial.println(body.substring(0, 200));
    }
    explicarCodigo(status);
    phase("err", "La transcripcion devolvio HTTP " + String(status));
    if (resultOut) *resultOut = STT_API_ERROR;
    return "";
  }

  StaticJsonDocument<64> filter;
  filter["text"] = true;

  DynamicJsonDocument doc(1024);
  if (deserializeJson(doc, body, DeserializationOption::Filter(filter))) {
    phase("err", "No se pudo leer la respuesta de la transcripcion");
    if (resultOut) *resultOut = STT_API_ERROR;
    return "";
  }

  return doc["text"].as<String>();
}

// ======================= 2. TEXTO -> LLM =======================

String askLLM(const String &userText) {
  if (WiFi.status() != WL_CONNECTED) {
    phase("err", "Sin WiFi");
    return "(sin conexion wifi)";
  }

  // Cliente TLS explicito. Llamar a http.begin(url) con una URL https:// usa la
  // API vieja de HTTPClient, que arma la conexion segura por atras sin que uno
  // controle nada.
  //
  // El certificado del servidor se valida: ver configurarTls() arriba.
  WiFiClientSecure client;
  configurarTls(client);

  HTTPClient http;
  http.setConnectTimeout(10000);
  http.setTimeout(20000);  // un LLM tarda bastante mas que los 5s por defecto

  phase("llm", "Pensando la respuesta...");

  if (!http.begin(client, LLM_API_URL)) {
    phase("err", "No se pudo abrir la conexion con el modelo");
    return "(no se pudo abrir la conexion)";
  }
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", String("Bearer ") + LLM_API_KEY);

  // El documento se dimensiona segun el texto real: con un tamano fijo, una
  // entrada larga se serializa truncada y en silencio, y la API devuelve 400
  // sin ninguna pista de por que.
  DynamicJsonDocument reqDoc(userText.length() + sizeof(ROBOT_SYSTEM_PROMPT) + 512);
  reqDoc["model"] = LLM_MODEL;

  JsonArray messages = reqDoc.createNestedArray("messages");

  JsonObject sys = messages.createNestedObject();
  sys["role"] = "system";
  sys["content"] = ROBOT_SYSTEM_PROMPT;

  JsonObject msg = messages.createNestedObject();
  msg["role"] = "user";
  msg["content"] = userText;

  String reqBody;
  serializeJson(reqDoc, reqBody);

  int httpCode = http.POST(reqBody);
  String reply;

  if (httpCode == 200) {
    // Filtro: de toda la respuesta nos quedamos SOLO con el texto. Una
    // respuesta de chat/completions trae ademas id, object, created, model,
    // usage, system_fingerprint... Sin filtrar no entra en el buffer y el
    // parseo falla siempre con NoMemory, que parece un error de red y no lo es.
    StaticJsonDocument<128> filter;
    filter["choices"][0]["message"]["content"] = true;

    // Se lee con getString() y no con getStream(): getStream() devuelve el
    // socket crudo, y si el servidor responde con Transfer-Encoding chunked el
    // cuerpo viene con los tamanos de bloque intercalados y el parseo falla.
    // getString() los desarma. La respuesta es corta -el prompt pide dos
    // oraciones como maximo- asi que tenerla en RAM no molesta.
    String body = http.getString();

    DynamicJsonDocument resDoc(1024);
    DeserializationError err = deserializeJson(
        resDoc, body, DeserializationOption::Filter(filter));

    if (err) {
      Serial.printf("Error parseando la respuesta: %s\n", err.c_str());
      Serial.println(body.substring(0, 200));
      reply = "(respuesta ilegible)";
    } else {
      JsonVariant content = resDoc["choices"][0]["message"]["content"];
      // isNull() y no length(): sobre un campo ausente, as<String>() puede
      // devolver el texto "null", que es como se colaba antes.
      if (content.isNull()) {
        Serial.println("LLM: la respuesta no traia texto. Cuerpo recibido:");
        Serial.println(body.substring(0, 200));
        reply = "(respuesta sin texto)";
      } else {
        reply = content.as<String>();
        if (reply.length() == 0) reply = "(respuesta vacia)";
      }
    }
  } else {
    // httpCode negativo = fallo de red/TLS; positivo = la API respondio error.
    phase("err", "El modelo devolvio HTTP " + String(httpCode));
    Serial.printf("Fallo la llamada al LLM, codigo %d\n", httpCode);
    explicarCodigo(httpCode);
    reply = "(error http " + String(httpCode) + ")";
  }

  http.end();
  return reply;
}

// ======================= 3. TEXTO -> VOZ =======================

// Sumidero que recibe el cuerpo de la respuesta y lo empuja al I2S.
//
// Se usa asi para que HTTPClient::writeToStream() se ocupe de desarmar el
// Transfer-Encoding chunked, en vez de leer el socket a mano. El audio nunca
// pasa por un buffer grande: baja y suena.
class I2SSink : public Stream {
 public:
  void (*onProgress)() = nullptr;

  using Print::write;

  size_t write(uint8_t b) override { return write(&b, 1); }

  size_t write(const uint8_t *buffer, size_t size) override {
    speakerWrite(buffer, size);
    if (onProgress) onProgress();
    return size;
  }

  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}
};

bool ttsSpeak(const String &text, void (*onProgress)()) {
  if (WiFi.status() != WL_CONNECTED || text.length() == 0) return false;

  WiFiClientSecure client;
  configurarTls(client);

  HTTPClient http;
  http.setConnectTimeout(10000);
  http.setTimeout(20000);

  phase("tts", "Generando la voz...");

  if (!http.begin(client, TTS_API_URL)) {
    Serial.println("TTS: no se pudo abrir la conexion");
    phase("err", "No se pudo abrir la conexion del TTS");
    return false;
  }
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", String("Bearer ") + LLM_API_KEY);

  DynamicJsonDocument reqDoc(text.length() + 256);
  reqDoc["model"] = TTS_MODEL;
  reqDoc["voice"] = TTS_VOICE;
  reqDoc["input"] = text;
  // "pcm" evita tener que decodificar MP3 en la placa: llega PCM crudo de
  // 24kHz, 16 bits, mono, que es exactamente lo que come el I2S.
  reqDoc["response_format"] = "pcm";

  String reqBody;
  serializeJson(reqDoc, reqBody);

  int httpCode = http.POST(reqBody);
  if (httpCode != 200) {
    phase("err", "El TTS devolvio HTTP " + String(httpCode));
    Serial.printf("TTS: fallo, codigo %d\n", httpCode);
    explicarCodigo(httpCode);
    http.end();
    return false;
  }

  phase("talk", "Hablando.");
  speakerStart(TTS_SAMPLE_RATE);

  I2SSink sink;
  sink.onProgress = onProgress;
  http.writeToStream(&sink);

  speakerStop();
  http.end();
  return true;
}

/*
 * CONTROL REMOTO - servidor web asincronico + WebSocket
 * -----------------------------------------------------------------------------
 * El celular abre la pagina que sirve el propio ESP32 y a partir de ahi todo
 * viaja por un unico WebSocket. No hay polling HTTP: con el joystick mandando
 * ~20 posiciones por segundo, un fetch() por posicion serian 20 conexiones TCP
 * nuevas por segundo, cada una con su handshake. El robot se arrastraba y el
 * joystick respondia con medio segundo de atraso. Con el socket abierto, cada
 * movimiento son unos 30 bytes y llega en el acto.
 *
 * QUIEN CORRE DONDE (importa, y mucho):
 *   Los callbacks del WebSocket NO corren en loop(). Corren en la tarea de
 *   AsyncTCP, en paralelo. Por eso aca adentro NO se toca ni un motor, ni un
 *   servo, ni la OLED, ni se llama a la API: lo unico que se hace es anotar lo
 *   que pidio el celular en la estructura de abajo. loop(), en la tarea
 *   principal, la lee y actua. Un servo movido desde dos tareas a la vez, o un
 *   i2s_write() desde la tarea equivocada, cuelga la placa de formas dificiles
 *   de rastrear.
 * -----------------------------------------------------------------------------
 */

#pragma once

#include <Arduino.h>

enum RobotMode {
  MODE_OFF,       // en reposo: no se mueve nada hasta que se elija un modo
  MODE_CONTROL,   // manejado desde el celular
  MODE_AUTO       // autonomo: esquiva solo y conversa
};

// De donde sale el audio de la voz.
enum MicSource {
  MIC_ROBOT,      // el GY-MAX9814 del robot
  MIC_PHONE       // el microfono del celular, subido por HTTP (respaldo)
};

// Lo que pidio el celular. La escribe la tarea de AsyncTCP y la lee loop().
//
// Todo volatile y todo de tipos simples (un entero de 32 bits o menos se
// escribe de una sola instruccion en el ESP32, asi que no hace falta un mutex
// para estos campos: no existe el caso de leer un valor a medio escribir).
struct RemoteState {
  volatile uint8_t  mode;        // RobotMode
  volatile int16_t  joyX;        // -100..100, giro (positivo = derecha)
  volatile int16_t  joyY;        // -100..100, avance (positivo = adelante)
  volatile uint32_t joyAt;       // millis() del ultimo mensaje de joystick
  volatile int16_t  armL;        // angulo LOGICO pedido, -10..90
  volatile int16_t  armR;
  volatile bool     micMuted;
  volatile uint8_t  micSource;   // MicSource
  volatile bool     sayRequested;      // hay texto escrito esperando
  volatile bool     phoneAudioReady;   // hay audio del celular esperando
  volatile bool     boxeoRequested;   // pulso: loop() lo consume y lo baja
  volatile bool     talkRequested;    // idem

  // Autotests pedidos desde la web. Tambien son pulsos.
  volatile bool     testMicRequested;
  volatile bool     testArmsRequested;
  volatile bool     testSpeakerRequested;
  volatile bool     testOledRequested;
  volatile bool     testToneRequested;   // sonido generado en la placa
};

extern RemoteState remote;

// Levanta el servidor y el WebSocket. Llamar despues de tener WiFi.
void remoteSetup();

// true si WEB_PASSWORD esta definida y es valida. Si no, el control web queda
// bloqueado: nadie se puede autenticar.
bool remoteAuthConfigured();

// Mantenimiento (cerrar clientes zombie). Va en loop().
void remoteLoop(unsigned long now);

// Manda el estado al celular. 'face' es el nombre de la expresion actual;
// 'busy' apaga los botones mientras el robot esta hablando o boxeando.
void remoteBroadcast(const char *face, long distanceCm, bool busy);

// Escribe una linea en el dialogo de la pagina.
//   who = "yo" (lo que se transcribio), "ro" (lo que contesto), "er" (un error)
void remoteLog(const char *who, const String &msg);

// ======================= VOZ POR CAMINOS ALTERNATIVOS =======================

// Toma el texto que se escribio en la web (respaldo cuando ningun microfono
// anda). Devuelve "" si no habia nada. Consume el pedido.
String remoteTakeSayText();

// Toma el audio subido desde el celular y CEDE LA PROPIEDAD del buffer: quien
// llama tiene que hacerle free() cuando termina. Devuelve false si no habia
// nada esperando.
// nameOut/mimeOut reciben copia del nombre de archivo y del MIME que declaro el
// navegador. Van copiados y no por puntero a proposito: apenas se suelta el
// audio puede entrar una subida nueva y pisarlos.
bool remoteTakePhoneAudio(uint8_t **bufOut, size_t *lenOut,
                          char *nameOut, size_t nameCap,
                          char *mimeOut, size_t mimeCap);

// true si hay al menos un celular conectado. Sirve para no perder tiempo
// armando JSON que no va a leer nadie.
bool remoteHasClients();

// ======================= ESTADO EN VIVO Y DIAGNOSTICO =======================

// En que anda el robot ahora mismo, con el detalle en castellano.
// Las etapas las define voice.h ("wait", "rec", "stt", "llm", "tts", "err"...).
void remoteStatus(const char *phase, const String &detail);

// Nivel del microfono, para la barra de la web. Va limitado por dentro a unos
// pocos mensajes por segundo: durante la grabacion esto se llama 30 veces por
// segundo y no tiene sentido mandarlas todas.
//   rms        nivel medido
//   threshold  nivel que hace falta para que cuente como voz (0 = todavia
//              midiendo el ruido de fondo)
void remoteMicLevel(int rms, int threshold);

// Una fila del panel de diagnostico. Se guardan y se le mandan enteras a cada
// celular que se conecta, asi el reporte de arranque no se pierde por haber
// abierto la pagina tarde.
//   ok = 1 anda, 0 fallo, -1 informativo (ni bien ni mal)
void remoteDiag(const char *key, const String &value, int ok);

// Manda todas las filas guardadas. Se llama sola cuando entra un cliente.
void remoteSendDiag(void *clientOrNull);

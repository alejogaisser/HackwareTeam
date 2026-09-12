/*
 * ROBOT WALL-E STYLE - ESP32
 * -----------------------------------------------------------------------------
 * - 2 motores DC con reduccion (driver tipo L298N / similar) -> movimiento autonomo
 * - Sensor ultrasonico HC-SR04 en la "cabeza" -> deteccion de obstaculos (ojitos)
 * - OLED I2C SSD1306 128x64 -> boca animada / estado de animo
 * - 2 servos -> brazos (arriba/abajo)
 * - Microfono GY-MAX9814 + amplificador MAX98357A -> conversacion por voz
 * - WiFi -> STT, LLM, TTS y ademas el control remoto desde el celular
 *
 * El circuito de la voz: se pide un turno (boton, monitor serie o el celular),
 * el robot ESPERA A ESCUCHAR VOZ (VAD), manda el audio a transcribir, le pasa el
 * texto al LLM, y reproduce la respuesta EN VOZ ALTA por el parlante mientras
 * gesticula. Todo eso vive en voice.cpp y audio.cpp.
 *
 * DOS MODOS, que se eligen desde la web:
 *   CONTROL   el celular maneja: joystick para los motores, un slider por brazo
 *             y un boton de boxeo. El ultrasonico sigue frenando igual: es una
 *             capa de seguridad aparte, no el esquive autonomo.
 *   AUTONOMO  el de siempre: anda solo esquivando obstaculos, y cuando se le
 *             pide un turno de charla contesta hablando, se balancea y mueve los
 *             brazos mientras habla.
 *
 * DONDE CORRE CADA COSA:
 *   loop()            movimiento, cara, servos, turnos de voz. Tarea principal.
 *   tarea de AsyncTCP el servidor web y el WebSocket (remote.cpp). NUNCA toca
 *                     hardware: solo anota lo que pidio el celular.
 * Por eso un turno de voz puede bloquear loop() varios segundos sin que la
 * pagina del celular se congele.
 *
 * LOS PINES ESTAN EN pins.h - es el unico archivo a tocar si cambia el cableado.
 * -----------------------------------------------------------------------------
 */

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ESP32Servo.h>
#include <WiFi.h>

#include "pins.h"
#include "config.h"
#include "audio.h"
#include "voice.h"
#include "remote.h"

#include <esp_system.h>

// ======================= PWM DE LOS MOTORES =======================
//
// OJO con esto: ESP32Servo reparte canales LEDC por su cuenta y NO ve los que
// reservamos aca con ledcSetup(). Con los motores en los canales 0 y 1 -los
// primeros que la libreria le da a los servos- los brazos terminaban pisando
// los canales de los motores.
//
// ESP32Servo agrupa los 16 canales en 4 "timers" asi:
//   timer 0 -> canales 0, 1, 8, 9      timer 2 -> canales 4, 5, 12, 13
//   timer 1 -> canales 2, 3, 10, 11    timer 3 -> canales 6, 7, 14, 15
// y en setupArms() reservamos SOLO los timers 0 y 1 para los brazos
// (ESP32PWM::allocateTimer), lo que deja los timers 2 y 3 marcados como no
// disponibles para la libreria. Por eso los motores van en 4 y 6: son canales
// que ESP32Servo nunca va a repartir.
//
// Si alguna vez sacan las llamadas a allocateTimer(), esta garantia se cae.
#define PWM_FREQ      1000
#define PWM_RES_BITS  8
#define PWM_CH_A      4    // timer 2, fuera del alcance de ESP32Servo
#define PWM_CH_B      6    // timer 3, fuera del alcance de ESP32Servo

// --- Umbral de distancia para esquivar (cm) ---
#define OBSTACLE_DISTANCE_CM   20

// ======================= OBJETOS GLOBALES =======================

Adafruit_SSD1306 display(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);
Servo armL;
Servo armR;

// ======================= ESTADOS DEL ROBOT =======================

enum RobotState {
  MOVING_FORWARD,
  TURNING,
  BACKING_UP
};

RobotState currentState = MOVING_FORWARD;
unsigned long stateChangedAt = 0;
int turnDirection = 1; // 1 = derecha, -1 = izquierda

// ======================= MOTORES =======================

void setupMotors() {
  // Los IN van a LOW apenas se puede: entre el reset y esta linea los pines
  // quedan flotando, y algunos L298N leen eso como "andar". El robot podia
  // pegar un tiron al encenderlo, antes de que el firmware decidiera nada.
  pinMode(MOTOR_A_IN1, OUTPUT); digitalWrite(MOTOR_A_IN1, LOW);
  pinMode(MOTOR_A_IN2, OUTPUT); digitalWrite(MOTOR_A_IN2, LOW);
  pinMode(MOTOR_B_IN1, OUTPUT); digitalWrite(MOTOR_B_IN1, LOW);
  pinMode(MOTOR_B_IN2, OUTPUT); digitalWrite(MOTOR_B_IN2, LOW);

  ledcSetup(PWM_CH_A, PWM_FREQ, PWM_RES_BITS);
  ledcAttachPin(MOTOR_A_EN, PWM_CH_A);

  ledcSetup(PWM_CH_B, PWM_FREQ, PWM_RES_BITS);
  ledcAttachPin(MOTOR_B_EN, PWM_CH_B);

  ledcWrite(PWM_CH_A, 0);
  ledcWrite(PWM_CH_B, 0);
}

// speed: -255 a 255 (negativo = reversa)
void setMotorA(int speedVal) {
  digitalWrite(MOTOR_A_IN1, speedVal >= 0 ? HIGH : LOW);
  digitalWrite(MOTOR_A_IN2, speedVal >= 0 ? LOW : HIGH);
  ledcWrite(PWM_CH_A, abs(speedVal));
}

void setMotorB(int speedVal) {
  digitalWrite(MOTOR_B_IN1, speedVal >= 0 ? HIGH : LOW);
  digitalWrite(MOTOR_B_IN2, speedVal >= 0 ? LOW : HIGH);
  ledcWrite(PWM_CH_B, abs(speedVal));
}

void driveForward(int speedVal = 180) {
  setMotorA(speedVal);
  setMotorB(speedVal);
}

void driveBackward(int speedVal = 180) {
  setMotorA(-speedVal);
  setMotorB(-speedVal);
}

void turnInPlace(int direction, int speedVal = 160) {
  // direction: 1 = derecha, -1 = izquierda
  setMotorA(direction * speedVal);
  setMotorB(-direction * speedVal);
}

void stopMotors() {
  setMotorA(0);
  setMotorB(0);
}

// ======================= ULTRASONICO =======================
//
// Un ping es bloqueante (pulseIn), asi que se dispara como mucho uno por ciclo
// y espaciado en el tiempo: el HC-SR04 necesita ~60ms entre disparos para que
// no le entre el eco del ping anterior. Se guardan las ultimas 3 lecturas y se
// usa la MEDIANA, para que un rebote raro no frene al robot de golpe.

#define DISTANCE_SAMPLES   3      // la mediana de abajo asume exactamente 3
#define PING_INTERVAL_MS   60
#define DISTANCE_FAR       999    // "no hay nada cerca"

// Timeout del pulseIn. Para esquivar no interesa nada mas alla de ~1.5m, y
// recortarlo baja el peor caso de bloqueo de 30ms a ~9ms. Eso importa cuando
// esta sonando el audio: cada bloqueo largo le corta el I2S.
#define ECHO_TIMEOUT_US    8700

long distSamples[DISTANCE_SAMPLES];
uint8_t distIndex = 0;
uint8_t distFilled = 0;
unsigned long lastPingAt = 0;
long lastGoodDistance = -1;
unsigned long lastGoodAt = 0;

void setupUltrasonic() {
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  digitalWrite(TRIG_PIN, LOW);
  for (uint8_t i = 0; i < DISTANCE_SAMPLES; i++) {
    distSamples[i] = DISTANCE_FAR;
  }
}

// Un disparo. Devuelve cm, o -1 si no hubo eco.
long pingOnce() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  long duration = pulseIn(ECHO_PIN, HIGH, ECHO_TIMEOUT_US);
  if (duration == 0) return -1;
  return duration * 0.0343 / 2;
}

void updateDistance(unsigned long now) {
  if (now - lastPingAt < PING_INTERVAL_MS) return;
  lastPingAt = now;

  long raw = pingOnce();
  long sample;

  if (raw > 0) {
    lastGoodDistance = raw;
    lastGoodAt = now;
    sample = raw;
  } else if (lastGoodDistance > 0 &&
             lastGoodDistance < OBSTACLE_DISTANCE_CM &&
             now - lastGoodAt < 500) {
    // "Sin eco" NO siempre significa camino libre: el HC-SR04 tambien se queda
    // mudo cuando el objeto esta a menos de ~2cm o cuando la superficie devuelve
    // el sonido en angulo. Si la ultima lectura buena y reciente era corta,
    // seguimos tratandolo como obstaculo en vez de avanzar contra la pared.
    sample = lastGoodDistance;
  } else {
    sample = DISTANCE_FAR;
  }

  distSamples[distIndex] = sample;
  distIndex = (distIndex + 1) % DISTANCE_SAMPLES;
  if (distFilled < DISTANCE_SAMPLES) distFilled++;
}

// Mediana de las ultimas 3 lecturas. Nunca devuelve -1: mientras no haya datos
// suficientes reporta DISTANCE_FAR.
long getDistanceCM() {
  if (distFilled < DISTANCE_SAMPLES) return DISTANCE_FAR;
  long a = distSamples[0], b = distSamples[1], c = distSamples[2];
  long t;
  if (a > b) { t = a; a = b; b = t; }
  if (b > c) { t = b; b = c; c = t; }
  if (a > b) { t = a; a = b; b = t; }
  return b;
}

// ======================= OLED - CARAS CYBER =======================
//
// La cara se redibuja SOLO cuando cambia la expresion, o cuando una expresion
// animada avanza de cuadro. Cada display() manda 1024 bytes por I2C: a 400kHz
// son ~25ms por cuadro. Redibujar en cada vuelta del loop hacia que el robot
// siguiera avanzando un buen rato despues de ver el obstaculo, y ademas le
// corta el audio al I2S, que necesita que le alimenten el buffer sin pausas.
//
// El estilo es a proposito geometrico: barras, chevrones, bloques. Una boca
// "realista" en 128x64 monocromo queda como una mancha; con formas rectas se
// lee la expresion de un vistazo y desde lejos.

enum Face {
  FACE_OFF,         // modo apagado: no se mueve nada
  FACE_IDLE,        // en reposo
  FACE_HAPPY,       // arranque, o esquivo resuelto
  FACE_ALERT,       // obstaculo cerca
  FACE_LISTENING,   // escuchando por el microfono
  FACE_THINKING,    // esperando la respuesta del LLM
  FACE_TALKING,     // reproduciendo la respuesta
  FACE_CONTROL,     // manejado desde el celular
  FACE_BOXING,      // modo boxeo
  FACE_MIC_ON,      // icono momentaneo: microfono activo
  FACE_MIC_OFF      // icono momentaneo: microfono silenciado
};

bool oledOk = false;              // false = la pantalla no arranco
Face currentFace = FACE_IDLE;
bool faceNeedsRedraw = true;
int faceFrame = 0;
unsigned long lastFaceFrame = 0;

// Icono momentaneo que tapa la cara un rato y despues se va solo. Se usa para
// el mute: el que aprieta el boton en el celular tiene que poder confirmar que
// el robot lo registro sin mirar la pantalla del telefono.
Face flashFace = FACE_MIC_ON;
unsigned long flashUntil = 0;

// Nombre corto para mandarle a la web.
const char *faceName(Face f) {
  switch (f) {
    case FACE_HAPPY:     return "happy";
    case FACE_ALERT:     return "alert";
    case FACE_LISTENING: return "listening";
    case FACE_THINKING:  return "thinking";
    case FACE_TALKING:   return "talking";
    case FACE_CONTROL:   return "control";
    case FACE_BOXING:    return "boxing";
    case FACE_OFF:       return "off";
    default:             return "idle";
  }
}

// Recorre el bus I2C y lista lo que contesta. Es lo primero que hay que mirar
// cuando la pantalla no prende: separa "no esta cableada" (no contesta nadie)
// de "esta en otra direccion" (contesta 0x3D en vez de 0x3C, que pasa seguido
// con los modulos que traen el puente de direccion soldado del otro lado).
uint8_t scanI2C(char *out, size_t outSize) {
  uint8_t found = 0;
  size_t used = 0;
  out[0] = 0;
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() != 0) continue;
    found++;
    if (used + 6 < outSize) {
      used += snprintf(out + used, outSize - used, "%s0x%02X", used ? " " : "", addr);
    }
  }
  return found;
}

bool oledPresent = false;   // contesto en el bus, mas alla de si init bien
uint8_t oledAddress = 0;    // direccion detectada; 0 = ninguna

void setupOled() {
  Wire.begin(OLED_SDA, OLED_SCL);
  Wire.setClock(400000);   // 4x el default; el SSD1306 lo banca sin problema

  // Antes se llamaba derecho a display.begin(). El problema es que begin()
  // manda los comandos de init y devuelve true SIN verificar que del otro lado
  // haya alguien: con la pantalla desconectada, oledOk quedaba en true y el
  // robot reportaba que la OLED andaba. Un ping al bus lo resuelve.
  oledOk = false;
  oledPresent = false;
  oledAddress = 0;
  const uint8_t candidates[] = {OLED_ADDR, OLED_ADDR == 0x3C ? 0x3D : 0x3C};
  for (uint8_t addr : candidates) {
    Wire.beginTransmission(addr);
    const uint8_t error = Wire.endTransmission();
    Serial.printf("OLED: probando 0x%02X -> I2C=%u\n", addr, error);
    if (error == 0 && !oledPresent) {
      oledAddress = addr;
      oledPresent = true;
    }
  }

  if (!oledPresent) {
    oledOk = false;
    Serial.printf("OLED: nadie contesta en 0x3C ni 0x3D (SDA=%d SCL=%d).\n",
                  OLED_SDA, OLED_SCL);
    char found[64];
    const uint8_t n = scanI2C(found, sizeof(found));
    if (n == 0) {
      Serial.println("     No contesta NADIE en todo el bus I2C. Eso apunta a");
      Serial.println("     alimentacion o a SDA/SCL cruzados o sueltos.");
    } else {
      Serial.printf("     Si contestan: %s. Si esta el modulo, revisar OLED_ADDR.\n", found);
    }
    return;
  }

  // Wire ya esta configurado con los pines del proyecto.
  oledOk = display.begin(SSD1306_SWITCHCAPVCC, oledAddress, true, false);
  if (!oledOk) {
    // Si begin() fallo por falta de memoria, el buffer interno queda en NULL y
    // el primer drawRoundRect() escribiria en un puntero nulo: panic y reinicio
    // en loop. Por eso se corta aca y renderFace() chequea la bandera.
    Serial.println("OLED no encontrada. El robot sigue andando, pero sin cara.");
    return;
  }
  display.clearDisplay();
  display.display();
  Serial.printf("OLED: inicializada en 0x%02X\n", oledAddress);
}

// --- la pantalla es SOLO la boca ---
//
// Los ojos del robot son los dos cilindros del sensor ultrasonico, que estan en
// la cabeza. Dibujar ojos ademas en la OLED daba una cara con cuatro ojos. Asi
// que aca va unicamente la boca, y justamente por eso puede ocupar los 128x64
// enteros: cada forma es del doble de grande que antes y se lee desde lejos.
//
// Todas las bocas estan centradas en el mismo punto y usan mas o menos el mismo
// ancho. Eso es lo que hace que se sientan la misma boca cambiando de gesto, en
// vez de siete dibujos distintos.

#define MOUTH_CY   32    // linea media de la boca
#define MOUTH_L    8     // margen izquierdo
#define MOUTH_R    120   // margen derecho

// Boca base: la del modo apagado, y la primera que se dibuja al encender.
// Es a proposito la mas simple y la mas "cerrada" de todas: sirve de prueba de
// pantalla -si esto no aparece al enchufar, la OLED no esta andando- y se lee
// como "prendido pero dormido", no como "colgado".
static void drawOff() {
  display.drawRoundRect(12, 18, 104, 28, 13, SSD1306_WHITE);
  display.drawFastHLine(30, MOUTH_CY, 68, SSD1306_WHITE);
}

// Reposo: una linea partida por un bloque al medio.
static void drawIdle() {
  display.fillRect(MOUTH_L, MOUTH_CY - 2, 44, 5, SSD1306_WHITE);
  display.fillRect(72, MOUTH_CY - 2, 44, 5, SSD1306_WHITE);
  display.drawRect(54, MOUTH_CY - 11, 20, 22, SSD1306_WHITE);
}

// Contento: bloques que bajan hacia el centro, formando una U.
// El orden importa: con el arco al reves se lee como cara de fastidio.
static void drawHappy() {
  static const uint8_t baja[9] = {0, 6, 11, 15, 16, 15, 11, 6, 0};
  for (int i = 0; i < 9; i++) {
    display.fillRect(9 + i * 13, 16 + baja[i], 11, 10, SSD1306_WHITE);
  }
}

// Alerta: zigzag de punta a punta.
static void drawAlert() {
  int x = MOUTH_L, y = 44;
  for (int i = 0; i < 8; i++) {
    const int nx = x + 14;
    const int ny = (i % 2) ? 44 : 20;
    for (int k = 0; k < 3; k++) display.drawLine(x, y + k, nx, ny + k, SSD1306_WHITE);
    x = nx;
    y = ny;
  }
}

// Escuchando: boca chica y quieta, con barras de nivel que se encienden de
// adentro hacia afuera.
//
// Antes esto eran ondas circulares saliendo de los costados, pero dos circulos
// grandes en una pantalla sin ojos se leen justamente como ojos, que es lo que
// se quiso sacar. Las barras se leen como señal entrando, no como cara.
static void drawListening(int frame) {
  display.fillRect(52, MOUTH_CY - 3, 24, 7, SSD1306_WHITE);
  for (int k = 0; k < 3; k++) {
    const int h = 10 + k * 10;                  // 10, 20, 30
    const int y = MOUTH_CY - h / 2;
    const bool encendida = (k <= (frame % 3));
    const int xi = 44 - k * 12;                 // hacia la izquierda
    const int xd = 80 + k * 12;                 // hacia la derecha
    if (encendida) {
      display.fillRect(xi, y, 6, h, SSD1306_WHITE);
      display.fillRect(xd, y, 6, h, SSD1306_WHITE);
    } else {
      display.drawRect(xi, y, 6, h, SSD1306_WHITE);
      display.drawRect(xd, y, 6, h, SSD1306_WHITE);
    }
  }
}

// Pensando: tres puntos que se van llenando de a uno.
static void drawThinking(int frame) {
  for (int i = 0; i < 3; i++) {
    const int cx = 34 + i * 30;
    if (i == (frame % 3)) display.fillCircle(cx, MOUTH_CY, 9, SSD1306_WHITE);
    else                  display.drawCircle(cx, MOUTH_CY, 9, SSD1306_WHITE);
  }
}

// Hablando: ecualizador de ocho barras, simetrico respecto de la linea media.
static void drawTalking(int frame) {
  // Alturas de una tabla y no con random(): con random() la boca titila como
  // ruido, y con un patron ciclico se lee como habla.
  static const uint8_t H[8][8] = {
    {10, 22, 38, 26, 14, 30, 18,  8}, {18, 34, 12, 40, 22, 10, 28, 16},
    {30, 12, 26, 16, 38, 24, 10, 22}, {12, 28, 44, 18,  8, 20, 34, 14},
    {24,  8, 18, 36, 16, 42, 12, 26}, { 8, 32, 14, 28, 40, 16, 22, 10},
    {28, 18, 34, 10, 24, 12, 44, 20}, {16, 40, 22, 14, 30, 26,  8, 32}
  };
  const uint8_t *h = H[frame & 7];
  for (int i = 0; i < 8; i++) {
    display.fillRect(MOUTH_L + i * 15, MOUTH_CY - h[i] / 2, 11, h[i], SSD1306_WHITE);
  }
}

// Modo control: una cruz de direcciones, que es lo que esta haciendo el que
// mira -manejarlo- en vez de una expresion.
static void drawControl() {
  display.fillRect(28, MOUTH_CY - 1, 72, 3, SSD1306_WHITE);
  display.fillRect(63, 14, 3, 36, SSD1306_WHITE);
  display.fillTriangle(64,  6, 53, 20, 75, 20, SSD1306_WHITE);   // adelante
  display.fillTriangle(64, 58, 53, 44, 75, 44, SSD1306_WHITE);   // atras
  display.fillTriangle(12, MOUTH_CY, 28, 21, 28, 43, SSD1306_WHITE);    // izquierda
  display.fillTriangle(116, MOUTH_CY, 100, 21, 100, 43, SSD1306_WHITE); // derecha
}

// Boxeo: boca apretada, con los dientes marcados. Late entre dos anchos.
static void drawBoxing(int frame) {
  const int w = (frame & 1) ? 96 : 76;
  const int x0 = 64 - w / 2;
  display.fillRect(x0, 20, w, 24, SSD1306_WHITE);
  display.drawFastHLine(x0, MOUTH_CY, w, SSD1306_BLACK);
  for (int x = x0 + 11; x < x0 + w - 4; x += 12) {
    display.drawFastVLine(x, 20, 24, SSD1306_BLACK);
  }
}

// Icono de microfono. crossed = tachado (silenciado).
static void drawMicIcon(bool crossed) {
  // capsula
  display.fillRoundRect(58, 8, 14, 26, 7, SSD1306_WHITE);
  // arco y pie
  display.drawCircle(65, 32, 14, SSD1306_WHITE);
  display.fillRect(40, 8, 50, 24, SSD1306_BLACK);   // recorta la mitad de arriba del arco
  display.fillRoundRect(58, 8, 14, 26, 7, SSD1306_WHITE);
  display.drawFastVLine(65, 46, 8, SSD1306_WHITE);
  display.drawFastHLine(56, 54, 18, SSD1306_WHITE);

  if (crossed) {
    // La barra tachada va con un contorno negro alrededor para que se despegue
    // del cuerpo blanco del microfono; si no, se pierde adentro de la mancha.
    for (int k = -1; k <= 1; k++) {
      display.drawLine(38 + k, 4, 92 + k, 58, SSD1306_BLACK);
    }
    for (int k = 0; k < 3; k++) {
      display.drawLine(40 + k, 4, 94 + k, 58, SSD1306_WHITE);
    }
  }
}

// Que cara toca dibujar ahora mismo: el icono momentaneo le gana a la cara.
Face effectiveFace(unsigned long now) {
  return (now < flashUntil) ? flashFace : currentFace;
}

void renderFace(unsigned long now) {
  if (!oledOk) return;   // un solo chequeo, y ninguna primitiva toca NULL

  display.clearDisplay();
  switch (effectiveFace(now)) {
    case FACE_OFF:       drawOff();                 break;
    case FACE_IDLE:      drawIdle();                break;
    case FACE_HAPPY:     drawHappy();               break;
    case FACE_ALERT:     drawAlert();               break;
    case FACE_LISTENING: drawListening(faceFrame);  break;
    case FACE_THINKING:  drawThinking(faceFrame);   break;
    case FACE_TALKING:   drawTalking(faceFrame);    break;
    case FACE_CONTROL:   drawControl();             break;
    case FACE_BOXING:    drawBoxing(faceFrame);     break;
    case FACE_MIC_ON:    drawMicIcon(false);        break;
    case FACE_MIC_OFF:   drawMicIcon(true);         break;
  }
  display.display();
}

// Pedir una expresion. Si ya es la actual no cuesta absolutamente nada, asi que
// se puede llamar desde adentro del loop sin pensarlo.
void setFace(Face f) {
  if (f == currentFace) return;
  currentFace = f;
  faceFrame = 0;
  faceNeedsRedraw = true;
}

// Tapar la cara con un icono durante un rato.
void flashIcon(Face f, unsigned long ms) {
  flashFace = f;
  flashUntil = millis() + ms;
  faceNeedsRedraw = true;
}

void updateFace(unsigned long now) {
  // Cuanto dura cada cuadro de las caras animadas.
  const unsigned long TALK_FRAME_MS  = 120;
  const unsigned long LISTEN_FRAME_MS = 160;
  const unsigned long THINK_FRAME_MS = 260;

  const Face shown = effectiveFace(now);

  unsigned long frameMs = 0;
  if      (shown == FACE_TALKING || shown == FACE_BOXING) frameMs = TALK_FRAME_MS;
  else if (shown == FACE_LISTENING)                       frameMs = LISTEN_FRAME_MS;
  else if (shown == FACE_THINKING)                        frameMs = THINK_FRAME_MS;

  if (frameMs && now - lastFaceFrame >= frameMs) {
    lastFaceFrame = now;
    faceFrame++;
    faceNeedsRedraw = true;
  }

  // Cuando se termina el icono momentaneo hay que volver a dibujar la cara que
  // habia debajo. Sin esto el microfono tachado se quedaba pegado en pantalla
  // hasta el proximo cambio de expresion.
  static bool flashWasShowing = false;
  const bool flashShowing = (now < flashUntil);
  if (flashWasShowing != flashShowing) {
    flashWasShowing = flashShowing;
    faceNeedsRedraw = true;
  }

  if (faceNeedsRedraw) {
    renderFace(now);
    faceNeedsRedraw = false;
  }
}

// ======================= SERVOS - BRAZOS =======================
//
// TODO el resto del programa habla en angulos LOGICOS:
//
//     -10  = un poco hacia atras del cuerpo
//       0  = brazo colgando, pegado al cuerpo, mirando al piso
//      90  = brazo levantado, horizontal al piso
//
// La conversion al valor que acepta la libreria (que no admite negativos) pasa
// unicamente aca adentro, y aca tambien se recortan los limites: cualquiera
// puede pedir 500 grados desde el celular y el brazo no se va a pasar de 90.

int appliedArmL = -999;   // -999 = todavia no se escribio nada
int appliedArmR = -999;

int armClampLogical(int deg) {
  if (deg < ARM_DEG_MIN) return ARM_DEG_MIN;
  if (deg > ARM_DEG_MAX) return ARM_DEG_MAX;
  return deg;
}

void armWriteLogical(Servo &s, int logicalDeg, bool right) {
  const int d = armClampLogical(logicalDeg);
  // El izquierdo sube sumando y el derecho restando: estan montados espejados.
  const int physical = right ? (ARM_R_SERVO_ZERO - d) : (ARM_L_SERVO_ZERO + d);
  s.write(constrain(physical, 0, 180));
}

// Mueve los dos brazos a angulos logicos y deja la web al tanto. Es el UNICO
// camino para tocar los servos: asi los sliders del celular nunca quedan
// mostrando una posicion que el brazo ya no tiene.
void armsApply(int lDeg, int rDeg) {
  lDeg = armClampLogical(lDeg);
  rDeg = armClampLogical(rDeg);

  if (lDeg != appliedArmL) {
    armWriteLogical(armL, lDeg, false);
    appliedArmL = lDeg;
    remote.armL = (int16_t)lDeg;
  }
  if (rDeg != appliedArmR) {
    armWriteLogical(armR, rDeg, true);
    appliedArmR = rDeg;
    remote.armR = (int16_t)rDeg;
  }
}

void setupArms() {
  ESP32PWM::allocateTimer(0);
  ESP32PWM::allocateTimer(1);
  armL.setPeriodHertz(50);
  armR.setPeriodHertz(50);
  armL.attach(SERVO_ARM_L, ARM_SERVO_MIN_US, ARM_SERVO_MAX_US);
  armR.attach(SERVO_ARM_R, ARM_SERVO_MIN_US, ARM_SERVO_MAX_US);

  // Los dos brazos se colocan de a UNO, con una pausa en el medio. Arrancar los
  // dos servos en el mismo instante es el pico de corriente mas grande de todo
  // el arranque, y con un powerbank justo eso alcanza para bajar la tension y
  // resetear la placa: el robot entra en ciclo de reinicio y parece que "no
  // arranca nada". Separarlos 400ms cuesta menos de un segundo, una sola vez.
  armWriteLogical(armL, 0, false);
  delay(400);
  armWriteLogical(armR, 0, true);
  delay(400);
  appliedArmL = 0;
  appliedArmR = 0;
  remote.armL = 0;
  remote.armR = 0;
}

// Animacion de reposo del modo autonomo: los brazos se mueven solos cada tanto.
unsigned long lastArmMove = 0;
bool armsUp = false;

void updateIdleArms(unsigned long now) {
  const unsigned long ARM_INTERVAL_MS = 1200;
  if (now - lastArmMove < ARM_INTERVAL_MS) return;
  lastArmMove = now;
  armsUp = !armsUp;
  const int deg = armsUp ? 60 : 0;
  armsApply(deg, deg);
}

// En modo control los brazos siguen a los sliders del celular. Se compara
// contra lo ya aplicado para no reescribir el servo 200 veces por segundo con
// el mismo valor.
void updateArmsFromWeb() {
  armsApply(remote.armL, remote.armR);
}

// ======================= WIFI =======================

void connectWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(ROBOT_HOSTNAME);
  // Sin esto el WiFi entra en ahorro de energia entre baliza y baliza, y los
  // mensajes del joystick llegan con 100 a 200ms de retraso a veces si y a
  // veces no. Se siente como que el robot "duda". Consume mas, pero el robot
  // ya tiene dos motores andando: el WiFi no es lo que le gasta la bateria.
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Conectando a WiFi");
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(300);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi conectado. IP: " + WiFi.localIP().toString());
  } else {
    Serial.println("\nNo se pudo conectar al WiFi (revisar credenciales / continuar sin voz)");
  }
}

// ======================= GESTOS MIENTRAS HABLA =======================
//
// Estos tick() se pasan como callback a ttsSpeak(): la libreria los llama por
// cada bloque de audio que baja del servidor, o sea unas 30 a 60 veces por
// segundo. Son el unico lugar donde corre codigo mientras suena la voz.
//
// REGLA DE ORO: nada de lo que pase aca adentro puede bloquear mas de unos
// pocos milisegundos. El buffer del I2S son ~85ms de audio; si un tick se
// cuelga mas que eso, el parlante hace un chasquido. Por eso aca no hay ningun
// delay(), la OLED se redibuja como mucho cada 120ms, y el ping del
// ultrasonico -que es lo unico que bloquea, hasta 8.7ms- va espaciado por
// PING_INTERVAL_MS.

unsigned long lastRockAt = 0;
uint8_t rockPhase = 0;
unsigned long lastGestureArmAt = 0;

void gestureReset(unsigned long now) {
  lastRockAt = now;
  rockPhase = 0;
  lastGestureArmAt = now;
}

// Balanceo: empuja un toque para adelante, para atras, y asi. Son pulsos
// cortos a baja velocidad, con pausas en el medio: el robot cabecea en el
// lugar en vez de irse caminando mientras contesta.
static void rockTick(unsigned long now) {
  const bool blocked = (getDistanceCM() < OBSTACLE_DISTANCE_CM);

  switch (rockPhase) {
    case 0:   // empuje hacia adelante
      // El freno de seguridad tambien vale para el balanceo: si tiene algo
      // pegado adelante, esta fase se salta y solo cabecea hacia atras.
      if (blocked) stopMotors();
      else         driveForward(TALK_ROCK_PWM);
      if (now - lastRockAt >= TALK_ROCK_MS) { stopMotors(); rockPhase = 1; lastRockAt = now; }
      break;
    case 1:   // pausa
      if (now - lastRockAt >= TALK_ROCK_GAP_MS) { rockPhase = 2; lastRockAt = now; }
      break;
    case 2:   // empuje hacia atras
      driveBackward(TALK_ROCK_PWM);
      if (now - lastRockAt >= TALK_ROCK_MS) { stopMotors(); rockPhase = 3; lastRockAt = now; }
      break;
    default:  // pausa
      if (now - lastRockAt >= TALK_ROCK_GAP_MS) { rockPhase = 0; lastRockAt = now; }
      break;
  }
}

// Brazos gesticulando. Los dos oscilan con periodos distintos y no multiplos
// entre si, asi que se cruzan en fases siempre distintas y nunca se ven
// sincronizados. Con periodos iguales parecia un ejercicio de gimnasia.
static void gestureArmsTick(unsigned long now) {
  if (now - lastGestureArmAt < TALK_ARM_STEP_MS) return;
  lastGestureArmAt = now;

  // Se usa millis() absoluto y no el tiempo desde que arranco a hablar: al
  // arrancar los dos senos valdrian 0 y los brazos saldrian juntos, que es
  // justo lo que se quiere evitar.
  const float phL = (float)(now % TALK_ARM_L_MS) / (float)TALK_ARM_L_MS;
  const float phR = (float)(now % TALK_ARM_R_MS) / (float)TALK_ARM_R_MS;

  const int l = TALK_ARM_BASE_DEG + (int)(TALK_ARM_AMP_DEG * sinf(phL * TWO_PI));
  const int r = TALK_ARM_BASE_DEG + (int)(TALK_ARM_AMP_DEG * sinf(phR * TWO_PI));
  armsApply(l, r);
}

// Callback para mientras suena la respuesta del LLM.
void talkTick() {
  const unsigned long now = millis();
  updateDistance(now);     // el freno de seguridad no se apaga nunca

  // En modo apagado el robot contesta igual, pero SIN moverse: apagado quiere
  // decir apagado. La cara se sigue animando, que es lo unico que no mueve nada.
  if (remote.mode != MODE_OFF) {
    rockTick(now);
    gestureArmsTick(now);
  }
  updateFace(now);
}

// Callback para mientras suena la frase del modo boxeo: en vez de balancearse,
// tira puñetazos y avanza a los tirones.
uint8_t boxSide = 0;
unsigned long lastBoxArmAt = 0;
unsigned long lastBoxStepAt = 0;
bool boxPushing = false;

void boxeoTick() {
  const unsigned long now = millis();
  updateDistance(now);

  // Puños alternados: uno arriba del todo, el otro a media altura.
  if (now - lastBoxArmAt >= 130) {
    lastBoxArmAt = now;
    boxSide ^= 1;
    armsApply(boxSide ? ARM_DEG_MAX : 45, boxSide ? 45 : ARM_DEG_MAX);
  }

  // Avance a tironcitos, con el mismo freno de seguridad de siempre.
  const bool blocked = (getDistanceCM() < OBSTACLE_DISTANCE_CM);
  if (boxPushing) {
    if (blocked || now - lastBoxStepAt >= BOXEO_PULSE_MS) {
      stopMotors();
      boxPushing = false;
      lastBoxStepAt = now;
    }
  } else if (now - lastBoxStepAt >= BOXEO_GAP_MS) {
    if (!blocked) {
      driveForward(BOXEO_PWM);
      boxPushing = true;
    }
    lastBoxStepAt = now;
  }

  updateFace(now);
}

// Callback para mientras escucha y graba: aca el robot tiene que estar QUIETO.
// Cualquier motor andando le mete al microfono un zumbido que tapa la voz y le
// arruina la transcripcion.
void listenTick() {
  updateFace(millis());
}

// ======================= MOVIMIENTO MANUAL (MODO CONTROL) =======================

// -100..100 del joystick -> PWM del motor.
//
// Hay dos cosas raras a proposito: una zona muerta al lado del cero (el dedo
// nunca deja el joystick exactamente en el centro, y sin zona muerta el robot
// se arrastra solo), y un piso de MANUAL_PWM_MIN (por debajo de eso el motor
// no llega a arrancar: solo zumba y se calienta).
#define JOY_DEADZONE  12

static int manualPwm(int v) {
  if (v > 100)  v = 100;
  if (v < -100) v = -100;
  const int a = abs(v);
  if (a < JOY_DEADZONE) return 0;
  const int pwm = MANUAL_PWM_MIN +
                  (a - JOY_DEADZONE) * (MANUAL_PWM_MAX - MANUAL_PWM_MIN) /
                      (100 - JOY_DEADZONE);
  return (v < 0) ? -pwm : pwm;
}

void updateManualMovement(unsigned long now) {
  // WATCHDOG. Si el celular dejo de mandar posiciones -se bloqueo la pantalla,
  // se cerro el navegador, se fue del WiFi- el robot frena solo. Sin esto, un
  // telefono que se apaga con el joystick a fondo deja al robot andando hasta
  // que choque contra algo o se quede sin bateria.
  if (remote.joyAt == 0 || now - remote.joyAt > JOYSTICK_TIMEOUT_MS) {
    stopMotors();
    setFace(FACE_CONTROL);
    return;
  }

  int x = remote.joyX;   // giro
  int y = remote.joyY;   // avance

  if (abs(x) < JOY_DEADZONE && abs(y) < JOY_DEADZONE) {
    stopMotors();
    setFace(FACE_CONTROL);
    return;
  }

  // FRENO DE SEGURIDAD. El ultrasonico sigue mandando aunque el modo sea
  // manual: es una capa aparte del esquive autonomo, no la misma. Si hay algo
  // cerca se anula SOLO la componente de avance; girar en el lugar se puede
  // seguir haciendo, que es justo lo que hace falta para salir del paso.
  if (getDistanceCM() < OBSTACLE_DISTANCE_CM && y > 0) {
    y = 0;
    setFace(FACE_ALERT);
  } else {
    setFace(FACE_CONTROL);
  }

  if (abs(x) < JOY_DEADZONE && abs(y) < JOY_DEADZONE) {
    stopMotors();
    return;
  }

  // Mezcla diferencial: avance mas giro para un lado, avance menos giro para el
  // otro. Puede pasarse de 100, asi que se re-escalan los dos juntos para no
  // deformar la direccion (recortarlos por separado hace que el robot doble
  // distinto a fondo que a media velocidad).
  int left  = y + x;
  int right = y - x;
  const int m = max(abs(left), abs(right));
  if (m > 100) {
    left  = left  * 100 / m;
    right = right * 100 / m;
  }

  setMotorA(manualPwm(left));
  setMotorB(manualPwm(right));
}

// ======================= CONVERSACION =======================
//
// Un turno completo de charla. Es bloqueante a proposito: mientras el robot
// escucha y contesta se queda quieto, que es lo que uno espera de el.
//
// Bloquear loop() no frena el servidor web: AsyncWebServer atiende en su propia
// tarea, asi que el celular sigue viendo la pagina y puede mutear el microfono
// en el medio. Lo unico que se congela es el movimiento, que es lo buscado.

bool robotBusy = false;   // hablando o boxeando: la web apaga los botones

// La cara de reposo depende del modo: en OFF vuelve a dormirse, en los demas
// vuelve a la cara normal. Se usa cada vez que termina algo.
static Face caraDeReposo() {
  return (remote.mode == MODE_OFF) ? FACE_OFF : FACE_IDLE;
}

// ---- segunda mitad del turno: pensar y contestar hablando ----
//
// Esta separada del microfono a proposito. Los tres caminos de entrada -el
// microfono del robot, el microfono del celular y el texto escrito- terminan
// todos aca, asi que la respuesta hablada, los gestos y la cara se comportan
// igual sin importar de donde vino lo que se dijo.
void speakAnswer(const String &heard) {
  setFace(FACE_THINKING);
  updateFace(millis());

  String answer = askLLM(heard);
  Serial.println("Responde: " + answer);
  remoteLog("ro", answer);

  setFace(FACE_TALKING);
  updateFace(millis());

  gestureReset(millis());
  ttsSpeak(answer, talkTick);

  stopMotors();
  if (remote.mode != MODE_OFF) armsApply(0, 0);
  setFace(caraDeReposo());
  updateFace(millis());

  remoteStatus("idle", "");

  // Arrancar el movimiento de cero, sin arrastrar el estado de antes de hablar.
  currentState = MOVING_FORWARD;
  stateChangedAt = millis();
}

// Chequeos comunes a cualquier turno de voz. Devuelve false si no hay que
// seguir.
static bool puedeHablar() {
  if (remote.micMuted) {
    Serial.println("Turno de voz ignorado: el microfono esta silenciado.");
    flashIcon(FACE_MIC_OFF, MUTE_ICON_MS);
    remoteLog("er", "El microfono esta silenciado.");
    remoteStatus("err", "El microfono esta silenciado");
    return false;
  }
  return true;
}

// ---- camino 1: microfono del robot ----
void runVoiceTurn() {
  if (!puedeHablar()) return;

  robotBusy = true;
  stopMotors();
  if (remote.mode != MODE_OFF) armsApply(0, 0);

  setFace(FACE_LISTENING);
  updateFace(millis());

  SttResult res = STT_OK;
  String heard = sttRecordAndTranscribe(RECORD_SECONDS, listenTick, &res);

  if (heard.length() == 0) {
    // Cada motivo se cuenta distinto: "no dijiste nada" y "no te escuche bien"
    // se arreglan de formas completamente distintas, y un mensaje generico
    // manda a revisar el cableado cuando el problema era que nadie hablo.
    const char *why;
    switch (res) {
      case STT_NO_VOICE:   why = "No te escuche hablar. Proba de nuevo, mas cerca."; break;
      case STT_TOO_QUIET:  why = "Se escucho muy bajito. Subi la ganancia del microfono."; break;
      case STT_NO_WIFI:    why = "Me quede sin WiFi."; break;
      case STT_NO_CONNECT: why = "No pude conectarme al servicio de voz."; break;
      case STT_API_ERROR:  why = "El servicio de voz devolvio un error (ver monitor serie)."; break;
      default:             why = "No entendi nada."; break;
    }
    Serial.println(why);
    remoteLog("er", why);
    // El detalle exacto del fallo ya lo mando voice.cpp por el hook de fase;
    // aca solo se vuelve a la cara de reposo.
    setFace(caraDeReposo());
    updateFace(millis());
    robotBusy = false;
    return;
  }

  Serial.println("Escuche: " + heard);
  remoteLog("yo", heard);

  speakAnswer(heard);
  robotBusy = false;
}

// ---- camino 2: microfono del celular ----
//
// El navegador ya grabo y subio el archivo; aca solo se reenvia a transcribir.
// El buffer viene con la propiedad cedida: hay que liberarlo si o si, incluso
// cuando la transcripcion falla, o se pierden 20KB de heap por intento.
void runPhoneTurn() {
  uint8_t *buf = nullptr;
  size_t len = 0;
  char nombre[24], mime[24];
  if (!remoteTakePhoneAudio(&buf, &len, nombre, sizeof(nombre), mime, sizeof(mime)))
    return;

  if (!puedeHablar()) {
    free(buf);
    return;
  }

  robotBusy = true;
  stopMotors();

  setFace(FACE_THINKING);
  updateFace(millis());
  Serial.printf("Audio del celular: %u bytes\n", (unsigned)len);

  SttResult res = STT_OK;
  String heard = sttTranscribeBuffer(buf, len, nombre, mime, &res);
  free(buf);

  if (heard.length() == 0) {
    remoteLog("er", "No se entendio lo que grabaste con el celular.");
    setFace(caraDeReposo());
    updateFace(millis());
    robotBusy = false;
    return;
  }

  Serial.println("Escuche (celular): " + heard);
  remoteLog("yo", heard);

  speakAnswer(heard);
  robotBusy = false;
}

// ---- camino 3: texto escrito ----
//
// El ultimo recurso: no depende de ningun microfono ni de la transcripcion. Si
// esto contesta y los otros dos no, el problema esta en la captura de audio y
// no en la red, en la key ni en el modelo.
void runSayTurn() {
  const String txt = remoteTakeSayText();
  if (txt.length() == 0) return;

  robotBusy = true;
  stopMotors();

  Serial.println("Texto escrito: " + txt);
  remoteLog("yo", txt);

  speakAnswer(txt);
  robotBusy = false;
}

// ======================= MODO BOXEO =======================
//
// Brazos arriba, una frase corta y unos pasos al frente. La frase NO pasa por
// el LLM: es fija y va derecho al TTS, para que salga en el momento. Un turno
// completo con LLM tarda 3 o 4 segundos y el gesto pierde toda la gracia.

void runBoxeo() {
  if (remote.mode == MODE_OFF) {
    remoteLog("er", "El robot esta en modo OFF: elegi CONTROL para que se mueva.");
    remoteStatus("err", "En modo OFF el robot no se mueve");
    return;
  }

  robotBusy = true;
  stopMotors();

  setFace(FACE_BOXING);
  updateFace(millis());

  // Guardia arriba antes de decir nada.
  armsApply(ARM_DEG_MAX, ARM_DEG_MAX);

  Serial.println("Modo boxeo!");
  remoteLog("ro", BOXEO_PHRASE);

  boxSide = 0;
  lastBoxArmAt = millis();
  lastBoxStepAt = millis();
  boxPushing = false;

  // Los puñetazos y el avance pasan adentro del callback, mientras suena la
  // frase: asi el robot se mueve y habla a la vez en vez de hacer una cosa
  // despues de la otra.
  ttsSpeak(BOXEO_PHRASE, boxeoTick);

  // Un par de tironcitos mas al final, ya sin audio, para cerrar el gesto.
  for (int i = 0; i < 2; i++) {
    if (getDistanceCM() >= OBSTACLE_DISTANCE_CM) {
      driveForward(BOXEO_PWM);
      delay(BOXEO_PULSE_MS);
    }
    stopMotors();
    delay(BOXEO_GAP_MS);
    updateDistance(millis());
  }

  stopMotors();
  armsApply(0, 0);
  setFace(caraDeReposo());
  updateFace(millis());
  robotBusy = false;

  currentState = MOVING_FORWARD;
  stateChangedAt = millis();
}

// ======================= REPORTES HACIA LA WEB =======================
//
// voice.cpp avisa por estos dos hooks en que anda cada turno. Sin ellos, desde
// el celular un turno fallido se ve siempre igual -no pasa nada- sin importar
// si el microfono no capto, si la transcripcion vino vacia o si la API tiro
// 429 por falta de credito. Los tres se arreglan distinto.

void onVoicePhase(const char *p, const String &detail) {
  Serial.printf("[%s] %s\n", p, detail.c_str());
  remoteStatus(p, detail);

  // La cara acompaña la etapa, asi el que mira el robot -y no el celular-
  // tambien sabe en que anda.
  if      (!strcmp(p, "wait") || !strcmp(p, "rec"))  setFace(FACE_LISTENING);
  else if (!strcmp(p, "stt")  || !strcmp(p, "llm"))  setFace(FACE_THINKING);
  else if (!strcmp(p, "talk"))                       setFace(FACE_TALKING);
  updateFace(millis());
}

void onVoiceLevel(int rms, int threshold, bool voice) {
  (void)voice;
  remoteMicLevel(rms, threshold);
}

// ======================= AUTOTESTS =======================
//
// Cada uno prueba UN periferico solo, y dice en castellano que vio. La idea es
// poder contestar "es el codigo o es el cable" desde el celular, sin reflashear
// y sin el monitor serie. El resultado queda fijo en el panel de diagnostico.

void runMicTest() {
  robotBusy = true;
  remoteStatus("test", "Midiendo el microfono (2 segundos, hacele ruido)...");
  Serial.println("Autotest: microfono");

  uint16_t rest = 0, pp = 0;
  const bool ok = micSelfTest(2000, &rest, &pp);

  if (!ok) {
    // El driver ni arranco: eso no es el cable del microfono.
    const String why = String("el driver I2S no arranco (") + micLastError() + ")";
    remoteDiag("MICROFONO", why, 0);
    remoteLog("er", "Microfono: " + why);
  } else {
    // Los rangos salen de la tabla de MIC_TEST_MODE, que ya estaba calibrada
    // para este modulo. Lo unico nuevo es que ahora se leen desde el celular.
    const char *veredicto;
    int estado = 1;
    if (rest < 100) {
      veredicto = "SIN SENAL: revisar VDD y el cable OUT del modulo"; estado = 0;
    } else if (rest > 3900) {
      veredicto = "PEGADO ARRIBA: OUT en corto con VDD"; estado = 0;
    } else if (rest < 1000) {
      veredicto = "reposo bajo: revisar la alimentacion del modulo"; estado = 0;
    } else if (rest > 2400) {
      veredicto = "reposo alto: puede estar alimentado con 5V en vez de 3.3V"; estado = 0;
    } else if (pp < 100) {
      veredicto = "no capta nada: pasar el pin GAIN al aire (60 dB)"; estado = 0;
    } else if (pp < 300) {
      veredicto = "capta flojo: pasar GAIN al aire (60 dB)"; estado = 0;
    } else if (pp > 3500) {
      veredicto = "satura: pasar GAIN a VDD (40 dB)"; estado = 0;
    } else {
      veredicto = "nivel correcto"; estado = 1;
    }
    const String txt = "reposo " + String(rest) + " / pico-pico " + String(pp) +
                       " -> " + veredicto;
    remoteDiag("MICROFONO", txt, estado);
    remoteLog(estado ? "ro" : "er", "Microfono: " + txt);
    Serial.println("Autotest microfono: " + txt);
  }

  remoteStatus("idle", "");
  robotBusy = false;
}

void runArmsTest() {
  robotBusy = true;
  remoteStatus("test", "Moviendo los brazos de 0 a 90 grados...");
  Serial.println("Autotest: brazos");

  if (!armL.attached() || !armR.attached()) {
    remoteDiag("SERVOS", "los servos no quedaron enganchados a ningun canal", 0);
    remoteLog("er", "Servos: no engancharon. Es un problema de codigo, no de cable.");
    remoteStatus("idle", "");
    robotBusy = false;
    return;
  }

  // De a uno y despacio: si se mueve uno solo, ya sabemos cual es el que falla.
  // Todos juntos y de golpe, un bajon de tension los deja quietos a los dos y
  // parece que ninguno anda.
  for (int deg = 0; deg <= ARM_DEG_MAX; deg += 10) { armsApply(deg, appliedArmR); delay(90); }
  delay(250);
  for (int deg = ARM_DEG_MAX; deg >= 0; deg -= 10) { armsApply(deg, appliedArmR); delay(90); }
  delay(400);
  for (int deg = 0; deg <= ARM_DEG_MAX; deg += 10) { armsApply(appliedArmL, deg); delay(90); }
  delay(250);
  for (int deg = ARM_DEG_MAX; deg >= 0; deg -= 10) { armsApply(appliedArmL, deg); delay(90); }

  armsApply(0, 0);
  remoteDiag("SERVOS", "pulso enviado (izq " + String(ARM_L_SERVO_ZERO) + "-" +
                           String(ARM_L_SERVO_ZERO + ARM_DEG_MAX) + ", der " +
                           String(ARM_R_SERVO_ZERO) + "-" +
                           String(ARM_R_SERVO_ZERO - ARM_DEG_MAX) + ")", -1);
  remoteLog("ro", "Brazos: primero el izquierdo, despues el derecho. "
                  "El que no se movio es el que hay que revisar.");
  remoteStatus("idle", "");
  robotBusy = false;
}

// Sonido generado DENTRO de la placa. Es el test que hay que hacer primero
// cuando no se escucha nada: no toca la red, ni la API, ni la key. Si esto no
// suena, el problema esta en el MAX98357A, en los tres cables del I2S o en el
// parlante, y no tiene sentido revisar nada mas.
void runToneTest() {
  robotBusy = true;
  remoteStatus("test", "Sonando... (deberias escuchar una voz robotica corta)");
  Serial.println("Autotest: sonido generado en la placa");

  speakerRobotSound();

  remoteDiag("PARLANTE", "sonido interno reproducido (sin usar la red)", -1);
  remoteLog("ro", "Si escuchaste el sonido robot, el parlante y el I2S andan. "
                  "Si no, revisar BCLK/LRC/DIN, el pin SD del modulo y el parlante.");
  remoteStatus("idle", "");
  robotBusy = false;
}

void runSpeakerTest() {
  robotBusy = true;
  remoteStatus("test", "Probando el parlante...");
  Serial.println("Autotest: parlante");

  const bool ok = ttsSpeak("Probando. Uno, dos, tres.", []() { updateFace(millis()); });

  remoteDiag("PARLANTE", ok ? "audio enviado al I2S" : "no se pudo generar el audio",
             ok ? -1 : 0);
  remoteLog(ok ? "ro" : "er",
            ok ? "Parlante: si no escuchaste nada, revisar BCLK/LRC/DIN, el pin SD "
                 "del modulo y los cables del parlante."
               : "Parlante: fallo antes de llegar al audio (ver el detalle de arriba).");
  remoteStatus("idle", "");
  robotBusy = false;
}

void runOledTest() {
  robotBusy = true;
  remoteStatus("test", "Probando la pantalla...");
  Serial.println("Autotest: OLED");
  setupOled();  // permite reintentar tras corregir cableado o direccion

  char found[64];
  const uint8_t n = scanI2C(found, sizeof(found));

  if (!oledOk) {
    const String txt = (n == 0)
        ? String("no contesta NADIE en el bus I2C: revisar 3.3V, GND, SDA y SCL")
        : String(oledPresent ? "fallo la inicializacion" : "no contesta en 0x3C ni 0x3D") +
              "; en el bus si contestan: " + found;
    remoteDiag("OLED", txt, 0);
    remoteLog("er", "Pantalla: " + txt);
  } else {
    // Pasa por todas las caras para que se vea que dibuja, no solo que responde.
    const Face secuencia[] = {FACE_HAPPY, FACE_ALERT, FACE_LISTENING,
                              FACE_THINKING, FACE_TALKING, FACE_CONTROL};
    for (uint8_t i = 0; i < 6; i++) {
      setFace(secuencia[i]);
      for (uint8_t k = 0; k < 4; k++) { updateFace(millis()); delay(130); }
    }
    setFace(FACE_IDLE);
    updateFace(millis());
    remoteDiag("OLED", String("responde en 0x") + String(oledAddress, HEX) +
                           "; secuencia enviada, confirmar imagen (bus: " + found + ")", 1);
    remoteLog("ro", "Pantalla: pasaron seis caras. Si no viste ninguna, el modulo "
                    "contesta pero no muestra (revisar contraste o el modulo).");
  }
  remoteStatus("idle", "");
  robotBusy = false;
}

// ======================= AUTOTEST DE ARRANQUE =======================
//
// Se corre una vez al final del setup y deja el resultado guardado. Cuando el
// celular se conecta -siempre despues del arranque- recibe el reporte completo.

static const char *motivoDeReinicio(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:  return "encendido normal";
    case ESP_RST_SW:       return "reinicio por software";
    case ESP_RST_PANIC:    return "CRASH del firmware (excepcion)";
    case ESP_RST_INT_WDT:  return "watchdog de interrupciones";
    case ESP_RST_TASK_WDT: return "watchdog de tarea";
    case ESP_RST_WDT:      return "watchdog";
    case ESP_RST_BROWNOUT: return "BAJON DE TENSION (fuente insuficiente)";
    case ESP_RST_EXT:      return "boton de reset";
    default:               return "desconocido";
  }
}

void bootSelfTest() {
  Serial.println();
  Serial.println("========== AUTOTEST DE ARRANQUE ==========");

  // Motivo del ultimo reinicio. Es el dato mas valioso de todos: si dice bajon
  // de tension o crash, no hay que revisar ni un cable de señal, el problema es
  // la alimentacion o el firmware.
  const esp_reset_reason_t rr = esp_reset_reason();
  const bool malReinicio = (rr == ESP_RST_BROWNOUT || rr == ESP_RST_PANIC ||
                            rr == ESP_RST_INT_WDT || rr == ESP_RST_TASK_WDT);
  remoteDiag("ULTIMO ARRANQUE", motivoDeReinicio(rr), malReinicio ? 0 : -1);
  Serial.printf("  Ultimo arranque : %s\n", motivoDeReinicio(rr));

  // I2C / pantalla
  char found[64];
  const uint8_t n = scanI2C(found, sizeof(found));
  if (oledOk) {
    remoteDiag("OLED", String("inicializada en 0x") + String(oledAddress, HEX), 1);
  } else if (n == 0) {
    remoteDiag("OLED", "no contesta nadie en el bus I2C (revisar 3.3V/GND/SDA/SCL)", 0);
  } else {
    remoteDiag("OLED", String(oledPresent ? "fallo la inicializacion" : "no esta en 0x3C ni 0x3D") +
                           ", pero en el bus contestan: " + found, 0);
  }
  Serial.printf("  Bus I2C         : %s\n", n ? found : "vacio");

  // Servos
  const bool servosOk = armL.attached() && armR.attached();
  remoteDiag("SERVOS", servosOk ? "enganchados a los canales PWM"
                                : "NO se pudieron enganchar (canales PWM agotados)",
             servosOk ? 1 : 0);
  Serial.printf("  Servos          : %s\n", servosOk ? "ok" : "NO engancharon");

  // Microfono: medicion corta, sin red y sin gastar API.
  uint16_t rest = 0, pp = 0;
  if (!micSelfTest(400, &rest, &pp)) {
    remoteDiag("MICROFONO", String("el driver I2S no arranco (") + micLastError() + ")", 0);
    Serial.println("  Microfono       : el driver I2S no arranco");
  } else {
    const bool micOk = (rest >= 1000 && rest <= 2400);
    remoteDiag("MICROFONO",
               "reposo " + String(rest) +
                   (micOk ? " (correcto)" : " (fuera de rango: revisar alimentacion)"),
               micOk ? 1 : 0);
    Serial.printf("  Microfono       : reposo %u %s\n", rest,
                  micOk ? "(correcto)" : "(FUERA DE RANGO)");
  }

  // Red y memoria
  if (WiFi.status() == WL_CONNECTED) {
    remoteDiag("WIFI", WiFi.localIP().toString() + "  (" + String(WiFi.RSSI()) + " dBm)", 1);
  } else {
    remoteDiag("WIFI", "sin conexion", 0);
  }
  remoteDiag("RAM LIBRE", String(ESP.getFreeHeap() / 1024) + " KB", -1);
  Serial.printf("  RAM libre       : %u KB\n", ESP.getFreeHeap() / 1024);
  Serial.println("==========================================");
  Serial.println();
}

// ======================= DISPARADORES =======================

// El estado del mute que vio la OLED la ultima vez. Cuando cambia se muestra el
// icono, venga el cambio del celular o de donde sea.
bool lastMuteShown = false;

void updateMuteIcon() {
  const bool m = remote.micMuted;
  if (m == lastMuteShown) return;
  lastMuteShown = m;
  flashIcon(m ? FACE_MIC_OFF : FACE_MIC_ON, MUTE_ICON_MS);
  Serial.println(m ? "Microfono SILENCIADO" : "Microfono ACTIVO");
}

void updateTriggers() {
  // Boton fisico de "push to talk".
  bool pressed = (digitalRead(BUTTON_PIN) == LOW);

  // Tambien se dispara mandando cualquier cosa por el monitor serie: sirve para
  // probar todo el circuito antes de tener el boton soldado.
  bool serialTrigger = false;
  while (Serial.available()) {
    Serial.read();
    serialTrigger = true;
  }

  // Los pedidos que llegaron del celular se consumen: se leen y se bajan de
  // una, para que un mensaje no dispare dos turnos si el loop pasa dos veces.
  const bool webTalk = remote.talkRequested;
  if (webTalk) remote.talkRequested = false;

  const bool webBox = remote.boxeoRequested;
  if (webBox) remote.boxeoRequested = false;

  // Los autotests van primero y salen de una: son bloqueantes, y no tiene
  // sentido encimarlos con un turno de voz.
  if (remote.testMicRequested)     { remote.testMicRequested = false;     runMicTest();     return; }
  if (remote.testArmsRequested)    { remote.testArmsRequested = false;    runArmsTest();    return; }
  if (remote.testSpeakerRequested) { remote.testSpeakerRequested = false; runSpeakerTest(); return; }
  if (remote.testOledRequested)    { remote.testOledRequested = false;    runOledTest();    return; }
  if (remote.testToneRequested)    { remote.testToneRequested = false;    runToneTest();    return; }

  // Caminos alternativos de voz. Van antes que el turno normal por la misma
  // razon que los autotests: son bloqueantes y no se enciman.
  if (remote.phoneAudioReady) { runPhoneTurn(); return; }
  if (remote.sayRequested)    { runSayTurn();   return; }

  if (webBox) {
    runBoxeo();
    return;   // uno por vuelta: los dos son bloqueantes
  }

  if (pressed || serialTrigger || webTalk) {
    // El boton fisico y el monitor serie usan SIEMPRE el microfono del robot,
    // aunque en la web este elegido el del celular: son el camino que tiene que
    // seguir funcionando cuando el celular no esta a mano.
    if ((pressed || serialTrigger) && remote.micSource == MIC_PHONE) {
      remoteLog("er", "Boton fisico: se usa el microfono del robot "
                      "(el del celular solo se puede disparar desde la web).");
    }
    runVoiceTurn();
  }
}

// ======================= COMPORTAMIENTO AUTONOMO =======================

void updateAutonomousMovement(unsigned long now) {
  // Ya viene filtrada por updateDistance() y no bloquea. Nunca es -1: cuando no
  // hay dato confiable devuelve DISTANCE_FAR.
  long distance = getDistanceCM();

  switch (currentState) {
    case MOVING_FORWARD:
      if (distance < OBSTACLE_DISTANCE_CM) {
        // Obstaculo: frenar, cara de alerta, empezar a retroceder
        stopMotors();
        setFace(FACE_ALERT);
        currentState = BACKING_UP;
        stateChangedAt = now;
      } else {
        driveForward();
        setFace(FACE_IDLE);
      }
      break;

    case BACKING_UP:
      driveBackward();
      if (now - stateChangedAt > 500) { // retrocede medio segundo
        stopMotors();
        turnDirection = random(0, 2) == 0 ? 1 : -1; // elige lado al azar
        currentState = TURNING;
        stateChangedAt = now;
      }
      break;

    case TURNING:
      turnInPlace(turnDirection);
      if (now - stateChangedAt > 400) { // gira un ratito
        stopMotors();
        setFace(FACE_HAPPY);
        currentState = MOVING_FORWARD;
        stateChangedAt = now;
      }
      break;
  }
}

// ======================= MODO PRUEBA DE MICROFONO =======================
//
// Se activa poniendo MIC_TEST_MODE en 1 dentro de config.h.
//
// Lee el microfono por el MISMO camino que usa la grabacion real (I2S con el
// ADC por DMA), pero muestra los valores crudos de 12 bits sin filtrar. La idea
// es separar "el microfono esta mal conectado" de "la red o la API fallan",
// que desde afuera se parecen: en los dos casos la transcripcion vuelve vacia.

#if MIC_TEST_MODE

// Con el MAX9814 alimentado a 3.3V la salida en reposo ronda 1.25V. Con el
// conversor de 12 bits eso cae por ahi de 1500, pero el ADC del ESP32 no es
// lineal y varia bastante de placa a placa: lo que importa no es acertar el
// numero exacto sino que sea ESTABLE y este en la zona media, ni en 0 ni en 4095.
#define MIC_REST_MIN   1000
#define MIC_REST_MAX   2400

void micTestSetup() {
  micStart();

  Serial.println();
  Serial.println("=========== MODO PRUEBA DE MICROFONO ===========");
  Serial.printf("Microfono en GPIO %d. El robot no se mueve en este modo.\n", MIC_PIN);
  Serial.println();
  Serial.println("VALOR DE REPOSO (con silencio alrededor):");
  Serial.println("   1000 a 2400   bien conectado y alimentado a 3.3V");
  Serial.println("   cerca de 0    sin alimentacion, o el cable OUT suelto");
  Serial.println("   cerca de 4095 el cable OUT esta en corto con VDD");
  Serial.println("   por encima    puede estar alimentado con 5V: bajalo a 3.3V");
  Serial.println();
  Serial.println("PICO A PICO (hablandole de cerca):");
  Serial.println("   menos de 100  no te escucha: pasa GAIN al aire (60 dB)");
  Serial.println("   300 a 2500    perfecto, dejalo asi");
  Serial.println("   mas de 3500   satura: pasa GAIN a VDD (40 dB)");
  Serial.println("===============================================");
  Serial.println();
}

// Barra de nivel en la OLED, para poder ajustar la ganancia mirando el robot en
// vez de la pantalla de la compu.
void micTestDraw(uint16_t pp) {
  if (!oledOk) return;

  int ancho = (int)((uint32_t)pp * 118 / 2048);
  if (ancho > 118) ancho = 118;

  display.clearDisplay();
  display.drawRect(4, 24, 120, 18, SSD1306_WHITE);
  if (ancho > 0) display.fillRect(5, 25, ancho, 16, SSD1306_WHITE);
  display.display();
}

void micTestTick() {
  static uint16_t buf[512];

  uint16_t minV = 4095, maxV = 0;
  uint32_t suma = 0, total = 0;

  // 4 bloques de 512 muestras a 16kHz = ~130ms de audio por medicion
  for (int i = 0; i < 4; i++) {
    size_t n = micReadRaw(buf, 512);
    if (n == 0) break;
    for (size_t j = 0; j < n; j++) {
      uint16_t v = buf[j];
      if (v < minV) minV = v;
      if (v > maxV) maxV = v;
      suma += v;
    }
    total += n;
  }

  if (total == 0) {
    Serial.println("No llegan muestras: el driver I2S no arranco.");
    delay(1000);
    return;
  }

  uint16_t promedio = (uint16_t)(suma / total);
  uint16_t pp = maxV - minV;

  // Barra de 24 caracteres, a fondo de escala 2048 de pico a pico
  char barra[25];
  int llenos = (int)((uint32_t)pp * 24 / 2048);
  if (llenos > 24) llenos = 24;
  for (int i = 0; i < 24; i++) barra[i] = (i < llenos) ? '#' : '.';
  barra[24] = 0;

  const char *veredicto;
  if (promedio < 100)                  veredicto = "SIN SENAL: revisa VDD y el cable OUT";
  else if (promedio > 3900)            veredicto = "PEGADO ARRIBA: OUT en corto con VDD";
  else if (promedio < MIC_REST_MIN)    veredicto = "reposo bajo: revisa la alimentacion";
  else if (promedio > MIC_REST_MAX)    veredicto = "reposo alto: puede estar a 5V";
  else if (maxV >= 4090 || minV <= 5)  veredicto = "SATURA: pasa GAIN a VDD";
  else if (pp < 100)                   veredicto = "silencio";
  else if (pp < 300)                   veredicto = "flojo: pasa GAIN al aire";
  else if (pp <= 2500)                 veredicto = "nivel perfecto";
  else                                 veredicto = "muy fuerte, al borde de saturar";

  Serial.printf("reposo %4u | pico-pico %4u | [%s] %s\n",
                promedio, pp, barra, veredicto);

  micTestDraw(pp);
}

#endif  // MIC_TEST_MODE

// ======================= VOLCADO DE AUDIO POR SERIE =======================
//
// Se activa poniendo MIC_DUMP_MODE en 1 dentro de config.h.
//
// Graba primero a memoria y recien despues transmite. Hacer las dos cosas a la
// vez no funciona: el base64 de un bloque tarda por serie casi lo mismo que el
// bloque en llenarse, y la grabacion sale con huecos. En este modo no se
// levanta el WiFi, asi que hay heap de sobra para el buffer.

#if MIC_DUMP_MODE

static const char B64_ABC[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static uint8_t b64Group[3];
static uint8_t b64Have = 0;
static char b64Line[80];
static uint8_t b64Col = 0;

static void b64Emit(char c) {
  b64Line[b64Col++] = c;
  if (b64Col >= 76) {
    b64Line[b64Col] = 0;
    Serial.println(b64Line);
    b64Col = 0;
  }
}

static void b64Byte(uint8_t b) {
  b64Group[b64Have++] = b;
  if (b64Have < 3) return;
  uint32_t v = ((uint32_t)b64Group[0] << 16) |
               ((uint32_t)b64Group[1] << 8) | b64Group[2];
  b64Emit(B64_ABC[(v >> 18) & 63]);
  b64Emit(B64_ABC[(v >> 12) & 63]);
  b64Emit(B64_ABC[(v >> 6) & 63]);
  b64Emit(B64_ABC[v & 63]);
  b64Have = 0;
}

static void b64Flush() {
  if (b64Have == 1) {
    uint32_t v = (uint32_t)b64Group[0] << 16;
    b64Emit(B64_ABC[(v >> 18) & 63]);
    b64Emit(B64_ABC[(v >> 12) & 63]);
    b64Emit('=');
    b64Emit('=');
  } else if (b64Have == 2) {
    uint32_t v = ((uint32_t)b64Group[0] << 16) | ((uint32_t)b64Group[1] << 8);
    b64Emit(B64_ABC[(v >> 18) & 63]);
    b64Emit(B64_ABC[(v >> 12) & 63]);
    b64Emit(B64_ABC[(v >> 6) & 63]);
    b64Emit('=');
  }
  b64Have = 0;
  if (b64Col > 0) {
    b64Line[b64Col] = 0;
    Serial.println(b64Line);
    b64Col = 0;
  }
}

static void putLE16d(uint8_t *p, uint16_t v) { p[0] = v & 0xFF; p[1] = v >> 8; }
static void putLE32d(uint8_t *p, uint32_t v) {
  p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF;
  p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;
}

void micDumpRun() {
  const uint32_t total = (uint32_t)MIC_SAMPLE_RATE * MIC_DUMP_SECONDS;
  const uint32_t dataBytes = total * 2;

  Serial.println();
  Serial.println("======== VOLCADO DE AUDIO ========");
  Serial.printf("Grabando %d segundos a %d Hz...\n", MIC_DUMP_SECONDS, MIC_SAMPLE_RATE);

  int16_t *rec = (int16_t *)malloc(dataBytes);
  if (!rec) {
    Serial.println("Sin memoria. Bajar MIC_DUMP_SECONDS en config.h.");
    return;
  }

  uint32_t got = 0;
  const unsigned long t0 = millis();
  micStart();
  while (got < total) {
    uint32_t want = total - got;
    if (want > 512) want = 512;
    size_t n = micRead(rec + got, want);
    if (n == 0) break;
    got += n;
  }
  micStop();
  const unsigned long dt = millis() - t0;

  Serial.printf("Listo: %u muestras en %lu ms (esperado ~%u ms)\n",
                (unsigned)got, dt, (unsigned)MIC_DUMP_SECONDS * 1000);
  if (dt < (unsigned long)MIC_DUMP_SECONDS * 700 ||
      dt > (unsigned long)MIC_DUMP_SECONDS * 1400) {
    Serial.println("OJO: la duracion no cuadra, la frecuencia real no es la declarada.");
  }

  // Las primeras muestras en crudo: si esto alterna entre extremos en vez de
  // variar suave, el formato de las muestras esta mal y no hace falta escuchar.
  Serial.print("Primeras 32 muestras:");
  for (int i = 0; i < 32 && (uint32_t)i < got; i++) {
    Serial.printf(" %d", rec[i]);
  }
  Serial.println();
  Serial.println();

  uint8_t h[44];
  memcpy(h, "RIFF", 4);
  putLE32d(h + 4, 36 + dataBytes);
  memcpy(h + 8, "WAVEfmt ", 8);
  putLE32d(h + 16, 16);
  putLE16d(h + 20, 1);
  putLE16d(h + 22, 1);
  putLE32d(h + 24, MIC_SAMPLE_RATE);
  putLE32d(h + 28, MIC_SAMPLE_RATE * 2);
  putLE16d(h + 32, 2);
  putLE16d(h + 34, 16);
  memcpy(h + 36, "data", 4);
  putLE32d(h + 40, got * 2);

  Serial.println("----WAV-BEGIN----");
  for (int i = 0; i < 44; i++) b64Byte(h[i]);
  const uint8_t *p = (const uint8_t *)rec;
  for (uint32_t i = 0; i < got * 2; i++) b64Byte(p[i]);
  b64Flush();
  Serial.println("----WAV-END----");

  free(rec);
  Serial.println("Volcado terminado.");
}

#endif  // MIC_DUMP_MODE

// ======================= SETUP / LOOP =======================

unsigned long lastTelemetryAt = 0;

// Le manda el estado al celular cada TELEMETRY_MS. Se manda siempre lo mismo
// (no solo lo que cambio) porque son 130 bytes cada 300ms: no vale la pena
// complicar el codigo para ahorrar eso, y asi un celular que se acaba de
// conectar ve el estado completo en la primera actualizacion.
void updateTelemetry(unsigned long now) {
  if (now - lastTelemetryAt < TELEMETRY_MS) return;
  lastTelemetryAt = now;
  remoteBroadcast(faceName(effectiveFace(now)), getDistanceCM(), robotBusy);
}

void setup() {
  Serial.begin(115200);

  setupMotors();      // primero: deja los motores frenados antes que nada
  setupUltrasonic();
  setupOled();

  // La boca base se dibuja YA, antes de los servos y antes del WiFi. Son hasta
  // 15 segundos de espera por la red: si la pantalla recien se encendiera
  // despues, parece que la placa no arranco. Ademas sirve de prueba: si al
  // enchufar no aparece esta boca, la OLED no esta andando y no hace falta
  // esperar a nada mas para saberlo.
  setFace(FACE_OFF);
  updateFace(millis());

#if MIC_DUMP_MODE
  // Una sola pasada: graba, vuelca y se queda quieto.
  micDumpRun();
  return;
#endif

#if MIC_TEST_MODE
  // En modo prueba no arrancamos servos ni WiFi: menos ruido electrico y nada
  // moviendose mientras se mide.
  micTestSetup();
  return;
#endif

  setupArms();

  pinMode(BUTTON_PIN, INPUT_PULLUP);

  // Un analogRead(0) para la semilla usaria GPIO0, que es pin de strapping de
  // boot y ademas cuelga de ADC2 (que se bloquea cuando arranca el WiFi).
  // esp_random() usa el generador de hardware y no toca ningun pin.
  randomSeed(esp_random());

  // El robot arranca en modo OFF y no se mueve hasta que se elija un modo desde
  // el celular, asi que se queda con la boca base que ya se dibujo arriba.

  // voice.cpp reporta por aca en que anda cada turno.
  voiceOnPhase = onVoicePhase;
  voiceOnLevel = onVoiceLevel;

  connectWifi(); // si falla, el robot igual anda: solo se queda sin voz
  remoteSetup(); // servidor web + WebSocket (no hace nada si no hay WiFi)

  bootSelfTest();

  Serial.println("Listo. Apreta el boton (o manda cualquier tecla) para hablar,");
  Serial.println("o entra desde el celular a la direccion de arriba.");
}

void loop() {
#if MIC_DUMP_MODE
  delay(1000);   // ya se volco todo en setup(), no queda nada por hacer
  return;
#endif

#if MIC_TEST_MODE
  micTestTick();
  return;
#endif

  unsigned long now = millis();

  updateDistance(now);            // como mucho un ping cada PING_INTERVAL_MS
  remoteLoop(now);                // limpieza de clientes que se fueron
  updateMuteIcon();               // muestra el icono si cambio el mute

  // El modo lo elige el celular. En control manda el joystick; en autonomo, la
  // maquina de estados de siempre. El ultrasonico corre en los dos: el freno de
  // seguridad del modo manual vive adentro de updateManualMovement().
  // Al cambiar de modo el robot frena SIEMPRE, y al entrar en OFF ademas baja
  // los brazos una vez y se queda quieto. Sin este corte, pasar de autonomo a
  // OFF dejaba los motores con el ultimo PWM que les habia puesto la maquina de
  // estados y el robot seguia andando en un modo que dice "apagado".
  static uint8_t modoAnterior = 255;
  if (remote.mode != modoAnterior) {
    modoAnterior = remote.mode;
    stopMotors();
    currentState = MOVING_FORWARD;
    stateChangedAt = now;
    if (remote.mode == MODE_OFF) armsApply(0, 0);
    Serial.printf("Modo: %s\n", remote.mode == MODE_OFF ? "OFF"
                                : remote.mode == MODE_CONTROL ? "CONTROL" : "AUTONOMO");
  }

  if (remote.mode == MODE_CONTROL) {
    updateManualMovement(now);
    updateArmsFromWeb();
  } else if (remote.mode == MODE_AUTO) {
    updateAutonomousMovement(now);
    updateIdleArms(now);
  } else {
    // MODE_OFF: nada. Ni motores, ni brazos, ni maquina de estados. Lo unico
    // que sigue vivo es la voz, la cara y el servidor web.
    stopMotors();
    setFace(FACE_OFF);
  }

  updateFace(now);                // solo toca el I2C si la cara cambio
  updateTelemetry(now);
  updateTriggers();               // boton, monitor serie y pedidos de la web

  delay(5);
}

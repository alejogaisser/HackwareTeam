/*
 * PINES - todos juntos en un solo lugar
 * -----------------------------------------------------------------------------
 * Este es el unico archivo que hay que tocar cuando cambie el cableado.
 *
 * Al re-mapear segun lo que este soldado, evitar:
 *   GPIO 6-11    -> conectados a la flash SPI, la placa no arranca si los usas
 *   GPIO 34-39   -> son SOLO ENTRADA: sirven para sensores y botones, pero no
 *                   pueden manejar motores, servos, TRIG ni I2S
 *   GPIO 0,2,12,15 -> pines de strapping (boot); si algo los fuerza en el
 *                   encendido la placa puede no bootear o entrar en modo flash
 *   GPIO 1,3     -> son TX/RX del monitor serie
 *   GPIO 16,17   -> en los modulos ESP32-WROVER los usa la PSRAM. La placa de
 *                   ESTE robot es WROOM (verificado), asi que estan libres.
 * -----------------------------------------------------------------------------
 */

#pragma once

// --- Motor A (izquierdo) ---
#define MOTOR_A_IN1   26
#define MOTOR_A_IN2   27
#define MOTOR_A_EN    14   // PWM (velocidad)

// --- Motor B (derecho) ---
#define MOTOR_B_IN1   25
#define MOTOR_B_IN2   33
#define MOTOR_B_EN    32   // PWM (velocidad)

// --- Ultrasonico HC-SR04 (cabeza) ---
// !! CABLEADO OBLIGATORIO !!
// El HC-SR04 alimentado a 5V devuelve el pulso de ECHO a 5V, y los GPIO del
// ESP32 NO toleran 5V. Va SI O SI un divisor resistivo entre ECHO y ECHO_PIN:
//
//   ECHO del sensor ---[ 1k ]---+--- ECHO_PIN del ESP32
//                               |
//                             [ 2k ]
//                               |
//                              GND
//
// Sin el divisor el pin funciona un rato y despues se degrada hasta morir.
// (TRIG es salida del ESP32 hacia el sensor, ese no necesita nada.)
#define TRIG_PIN      5
#define ECHO_PIN      18

// --- OLED (boca) I2C ---
#define OLED_SDA      21
#define OLED_SCL      22
#define OLED_WIDTH    128
#define OLED_HEIGHT   64
#define OLED_ADDR     0x3C  // preferida: setupOled prueba tambien 0x3D

// --- Servos brazos ---
#define SERVO_ARM_L   13
#define SERVO_ARM_R   19

// --- Microfono GY-MAX9814 (salida ANALOGICA) ---
//
// El MAX9814 no es I2S: entrega una senal analogica, asi que se lee por ADC.
// TIENE que ir en un pin del ADC1 (32,33,34,35,36,39). El ADC2 queda inutilizable
// apenas arranca el WiFi, y este robot vive conectado, asi que ADC2 no es opcion.
// 34-39 son de solo-entrada, que es exactamente lo que necesitamos aca.
//
// El canal de abajo TIENE que corresponder al pin. Tabla:
//   GPIO 36 -> ADC1_CHANNEL_0     GPIO 34 -> ADC1_CHANNEL_6
//   GPIO 39 -> ADC1_CHANNEL_3     GPIO 35 -> ADC1_CHANNEL_7
//   GPIO 32 -> ADC1_CHANNEL_4     GPIO 33 -> ADC1_CHANNEL_5
//
// Cableado: VDD a 3.3V (no a 5V, o la salida se sale del rango del ADC),
// GND a GND, OUT al MIC_PIN. El pin GAIN del modulo: al aire = 60dB,
// a GND = 50dB, a VDD = 40dB. Para hablarle de cerca conviene 40 o 50dB;
// con 60dB satura y la transcripcion sale peor.
#define MIC_PIN            35
#define MIC_ADC_CHANNEL    ADC1_CHANNEL_7

// --- Amplificador MAX98357A (I2S, parlante) ---
//
// Este si es I2S digital. Necesita 3 pines de salida.
// Cableado: VIN a 5V, GND a GND, BCLK/LRC/DIN a los pines de abajo,
// y el parlante (4 a 8 ohm) a los bornes + y -.
// El pin SD del modulo enciende el amplificador: en la mayoria de los breakouts
// ya viene con pull-up y queda prendido solo. Si te queda mudo, revisa ese pin.
#define I2S_BCLK_PIN       16
#define I2S_LRC_PIN        17
#define I2S_DOUT_PIN       4

// --- Boton de "push to talk" ---
// Va entre el pin y GND, sin resistencia: se usa el pull-up interno.
#define BUTTON_PIN         23

// -----------------------------------------------------------------------------
// MAPA ALTERNATIVO - NO hace falta con la placa actual
// -----------------------------------------------------------------------------
// La placa de este robot es un ESP32-WROOM, asi que los GPIO 16 y 17 estan
// libres y el mapa de arriba va tal cual. Esto queda anotado solo por si alguna
// vez se reemplaza la placa por un WROVER, donde esos dos pines los usa la
// PSRAM. En ese caso, y solo en ese caso, cambiar a:
//
//   #define ECHO_PIN      34     // solo-entrada, perfecto para el echo
//   #define I2S_BCLK_PIN  18     // queda libre al mover el ECHO
//   #define I2S_LRC_PIN   23
//   #define I2S_DOUT_PIN  4
//   #define BUTTON_PIN    39     // solo-entrada, alcanza para un boton
//
// OJO con el boton en 39: los pines 34-39 NO tienen pull-up interno, asi que
// el INPUT_PULLUP del setup() no hace nada ahi. En ese caso hay que agregar
// una resistencia de 10k entre el pin y 3.3V.
//
// Para distinguirlos: el texto grabado en la latita metalica del modulo, o su
// largo (WROOM 25.5mm, WROVER 31.4mm; los dos miden 18mm de ancho).

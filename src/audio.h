/*
 * AUDIO - microfono y parlante
 * -----------------------------------------------------------------------------
 * El hardware es asimetrico y eso define todo el diseno:
 *
 *   GY-MAX9814  -> ANALOGICO. Se lee por ADC1, usando el periferico I2S0 en
 *                  modo "built-in ADC", que muestrea a frecuencia fija por DMA.
 *                  Sin DMA habria que leer el ADC a mano en un timer y el
 *                  jitter arruinaria el audio.
 *   MAX98357A   -> I2S DIGITAL. Se maneja con el periferico I2S1.
 *
 * Usar dos perifericos distintos (I2S0 para entrar, I2S1 para salir) evita
 * reconfigurar el mismo bloque cada vez que se pasa de escuchar a hablar.
 * -----------------------------------------------------------------------------
 */

#pragma once

#include <Arduino.h>

// Whisper trabaja a 16kHz. Grabar mas alto solo agrega bytes que el servicio
// tira igual.
#define MIC_SAMPLE_RATE   16000

// La respuesta de voz de OpenAI en formato "pcm" viene a 24kHz, 16 bits, mono,
// sin cabecera. Se manda derecho al I2S sin decodificar nada.
#define TTS_SAMPLE_RATE   24000

// --- microfono ---
// micStart() devuelve false si el driver I2S no arranco. Ese caso hay que
// distinguirlo de "arranco pero no se escucha nada": el primero es un problema
// de software (un pin invalido, el driver ya instalado), el segundo es de
// cableado, y desde afuera los dos se ven igual (transcripcion vacia).
bool micStart();
void micStop();
bool micIsRunning();

// Ultimo motivo por el que fallo el microfono, en texto. "" si todo bien.
const char *micLastError();

// Lee un bloque, ya convertido a PCM 16 bits con signo y sin componente
// continua. Devuelve cuantas muestras escribio en dest.
size_t micRead(int16_t *dest, size_t maxSamples);

// Lectura CRUDA, para diagnostico. Usa el mismo camino que micRead() -I2S con
// ADC por DMA- pero devuelve los valores de 12 bits tal como salen del
// conversor (0 a 4095), sin quitar la continua ni escalar. Si estos numeros
// estan bien, la grabacion de verdad va a funcionar.
size_t micReadRaw(uint16_t *dest, size_t maxSamples);

// --- parlante ---
// Sonido de prueba, generado DENTRO de la placa: no usa WiFi, ni la API, ni un
// centavo de credito. Es el unico test que aisla de verdad la etapa de audio:
// si esto suena, el MAX98357A, el cableado I2S y el parlante estan bien, y
// cualquier problema de voz esta mas arriba (red, key, transcripcion).
// Si no suena, ni vale la pena mirar la red.
void speakerRobotSound();

void speakerStart(uint32_t sampleRate);
void speakerStop();
void speakerWrite(const uint8_t *data, size_t bytes);

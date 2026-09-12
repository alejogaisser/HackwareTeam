/*
 * VOZ - el circuito completo: hablo -> STT -> LLM -> TTS -> parlante
 * -----------------------------------------------------------------------------
 * La restriccion que manda aca es la RAM. El ESP32 tiene 320KB y despues de
 * levantar WiFi y TLS quedan libres unos 150KB. Cinco segundos de audio a 16kHz
 * y 16 bits son 160KB: no entran. Por eso NADA se guarda entero en memoria:
 *
 *   - Al grabar, el audio se manda al servicio de transcripcion a medida que
 *     sale del microfono. Como la duracion es fija, el Content-Length se puede
 *     calcular antes de empezar, que es lo unico que hacia falta para poder
 *     transmitir en streaming.
 *   - Al reproducir, la respuesta se pide en PCM crudo y se vuelca al I2S a
 *     medida que baja. Sin decodificador de MP3 y sin buffer grande.
 *
 * El resultado es que toda la etapa de audio usa unos pocos KB de RAM.
 * -----------------------------------------------------------------------------
 */

#pragma once

#include <Arduino.h>

// Por que fallo el ultimo turno de voz. Sirve para que la web muestre algo util
// en vez de un "no se entendio" generico: "no dijiste nada" y "no te escuche
// bien" son problemas distintos y se arreglan distinto.
enum SttResult {
  STT_OK,
  STT_NO_WIFI,
  STT_NO_CONNECT,
  STT_NO_VOICE,      // el VAD nunca escucho voz: no se grabo nada
  STT_TOO_QUIET,     // se grabo, pero salio practicamente silencio
  STT_API_ERROR
};

// ======================= REPORTE DE AVANCE =======================
//
// Sin esto, un turno de voz es una caja negra: se aprieta el boton, pasan cinco
// segundos y sale un texto o no sale nada. Cuando no sale nada, no hay forma de
// saber si el microfono no capto, si la transcripcion vino vacia o si el LLM
// tiro error. Estos dos hooks abren la caja.

// En que etapa esta el turno.
//   "wait"  esperando a escuchar voz (VAD)
//   "rec"   grabando y subiendo
//   "stt"   esperando la transcripcion
//   "llm"   esperando la respuesta del modelo
//   "tts"   bajando y reproduciendo el audio de la respuesta
//   "err"   fallo algo; el detalle dice que
typedef void (*VoicePhaseFn)(const char *phase, const String &detail);
extern VoicePhaseFn voiceOnPhase;

// Nivel del microfono, cuadro por cuadro, mientras espera y graba.
// Es LA herramienta para separar "no me escucha" de "la API falla": si esto se
// queda en cero, el problema esta antes de la red.
typedef void (*VoiceLevelFn)(int rms, int threshold, bool voice);
extern VoiceLevelFn voiceOnLevel;

// Mide el microfono durante 'ms' SIN llamar a ninguna API ni gastar creditos.
// Devuelve false si el driver I2S no arranco (eso ya es un problema de
// software o de pines, no de cableado del microfono).
//   restOut  nivel de reposo, en cuentas del ADC de 12 bits (0..4095)
//   ppOut    pico a pico, que es lo que sube cuando alguien habla
bool micSelfTest(uint16_t ms, uint16_t *restOut, uint16_t *ppOut);

// Graba y transcribe.
//
//   seconds    duracion de la grabacion, una vez que arranco a hablar
//   onTick     (opcional) se llama seguido durante la espera y la grabacion.
//              Sirve para animar la cara sin frenar el audio.
//   resultOut  (opcional) por que fallo, si fallo.
//
// Antes de grabar espera a escuchar voz de verdad (VAD, ver config.h). Si en
// VAD_WAIT_MS nadie habla, cancela el turno y devuelve un String vacio sin
// gastar una sola llamada a la API.
String sttRecordAndTranscribe(uint16_t seconds,
                              void (*onTick)() = nullptr,
                              SttResult *resultOut = nullptr);

// Transcribe un audio que YA esta en memoria, en vez de grabarlo del microfono.
// Es el camino del microfono del celular: el navegador graba, sube el archivo al
// ESP32, y esto lo reenvia al servicio de transcripcion tal cual, sin decodificar
// nada en la placa.
//
//   data/len     el archivo entero (webm, wav, mp3... lo que haya grabado)
//   filename     nombre con extension: de ahi saca el formato el servicio
//   contentType  MIME que corresponda a esa extension
String sttTranscribeBuffer(const uint8_t *data, size_t len, const char *filename,
                           const char *contentType, SttResult *resultOut = nullptr);

// Manda texto al LLM y devuelve la respuesta.
String askLLM(const String &userText);

// Reproduce el texto por el parlante. onProgress (opcional) se llama seguido
// durante la reproduccion: sirve para animar la boca y mover el robot sin
// frenar el audio.
bool ttsSpeak(const String &text, void (*onProgress)() = nullptr);

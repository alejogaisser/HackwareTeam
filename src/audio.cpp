#include "audio.h"
#include "pins.h"
#include "config.h"

#include <driver/i2s.h>
#include <driver/adc.h>

static bool micRunning = false;
static bool speakerRunning = false;
static const char *micError = "";

// Estimacion de la componente continua del microfono.
//
// El MAX9814 centra su salida en 1.25V, pero el punto medio del ADC (con
// atenuacion de 11dB) esta cerca de 1.65V. Si restaramos un 2048 fijo, todo el
// audio quedaria corrido y sonaria saturado para un lado. Este promedio movil
// muy lento sigue el nivel real de reposo y lo resta: es un filtro pasa-altos
// de un polo, que ademas compensa si cambia la alimentacion o la temperatura.
static float dcEstimate = 2048.0f;

// ======================= MICROFONO =======================

bool micStart() {
  if (micRunning) return true;
  micError = "";

  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_ADC_BUILT_IN);
  cfg.sample_rate = MIC_SAMPLE_RATE;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count = 4;
  cfg.dma_buf_len = 512;
  cfg.use_apll = false;

  if (i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL) != ESP_OK) {
    micError = "no se pudo instalar el driver I2S0";
    Serial.println("Mic: no se pudo instalar el driver I2S0");
    return false;
  }

  adc1_config_width(ADC_WIDTH_BIT_12);
  // DB_12 es el rango completo (~0 a 3.3V). El nombre viejo ADC_ATTEN_DB_11
  // apunta al mismo valor pero esta deprecado.
  adc1_config_channel_atten(MIC_ADC_CHANNEL, ADC_ATTEN_DB_12);
  i2s_set_adc_mode(ADC_UNIT_1, MIC_ADC_CHANNEL);

  // i2s_adc_enable() puede fallar sin que falle el install: pasa, por ejemplo,
  // si el canal de ADC no corresponde al MIC_PIN declarado en pins.h. Antes
  // eso se ignoraba y la grabacion salia muda, que es indistinguible de un
  // cable suelto. Ahora queda dicho.
  if (i2s_adc_enable(I2S_NUM_0) != ESP_OK) {
    micError = "el ADC no se pudo enganchar al I2S (revisar MIC_ADC_CHANNEL)";
    Serial.println("Mic: i2s_adc_enable fallo. Revisar que MIC_ADC_CHANNEL");
    Serial.println("     corresponda al MIC_PIN declarado en pins.h.");
    i2s_driver_uninstall(I2S_NUM_0);
    return false;
  }

  micRunning = true;
  return true;
}

bool micIsRunning() { return micRunning; }

const char *micLastError() { return micError; }

void micStop() {
  if (!micRunning) return;
  i2s_adc_disable(I2S_NUM_0);
  i2s_driver_uninstall(I2S_NUM_0);
  micRunning = false;
}

size_t micRead(int16_t *dest, size_t maxSamples) {
  if (!micRunning) return 0;

  size_t bytesRead = 0;
  if (i2s_read(I2S_NUM_0, dest, maxSamples * sizeof(int16_t),
               &bytesRead, portMAX_DELAY) != ESP_OK) {
    return 0;
  }

  const size_t samples = bytesRead / sizeof(int16_t);

  // El ADC built-in entrega, en cada palabra de 16 bits, el valor de 12 bits en
  // la parte baja y el numero de canal en los 4 bits altos. Hay que enmascarar
  // antes de usarlo. La conversion es in-place: uint16_t y int16_t ocupan lo
  // mismo, y siempre se lee una posicion antes de escribirla.
  uint16_t *raw = (uint16_t *)dest;
  for (size_t i = 0; i < samples; i++) {
    float value = (float)(raw[i] & 0x0FFF);

    dcEstimate += (value - dcEstimate) * 0.0005f;

    // 12 bits -> 16 bits: el x16 recupera el rango completo del formato.
    int32_t centered = (int32_t)((value - dcEstimate) * 16.0f);
    if (centered > 32767) centered = 32767;
    if (centered < -32768) centered = -32768;
    dest[i] = (int16_t)centered;
  }

#if MIC_SWAP_SAMPLE_PAIRS
  // Reordenar los pares. El periferico I2S empaqueta de a 32 bits y el ADC
  // llena primero la mitad alta, asi que las muestras salen como s1,s0,s3,s2...
  for (size_t i = 0; i + 1 < samples; i += 2) {
    int16_t t = dest[i];
    dest[i] = dest[i + 1];
    dest[i + 1] = t;
  }
#endif

  return samples;
}

size_t micReadRaw(uint16_t *dest, size_t maxSamples) {
  if (!micRunning) return 0;

  size_t bytesRead = 0;
  if (i2s_read(I2S_NUM_0, dest, maxSamples * sizeof(uint16_t),
               &bytesRead, portMAX_DELAY) != ESP_OK) {
    return 0;
  }

  const size_t samples = bytesRead / sizeof(uint16_t);
  for (size_t i = 0; i < samples; i++) {
    dest[i] &= 0x0FFF;   // los 4 bits altos son el numero de canal, no audio
  }
  return samples;
}

// ======================= PARLANTE =======================

void speakerStart(uint32_t sampleRate) {
  if (speakerRunning) return;

  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  cfg.sample_rate = sampleRate;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  // 8 buffers de 256 muestras son ~85ms de audio en cola a 24kHz. Ese colchon
  // es lo que permite redibujar la boca en la OLED sin que se corte el sonido.
  cfg.dma_buf_count = 8;
  cfg.dma_buf_len = 256;
  cfg.use_apll = false;
  cfg.tx_desc_auto_clear = true;

  if (i2s_driver_install(I2S_NUM_1, &cfg, 0, NULL) != ESP_OK) {
    Serial.println("Parlante: no se pudo instalar el driver I2S1");
    return;
  }

  i2s_pin_config_t pins = {};
  pins.mck_io_num = I2S_PIN_NO_CHANGE;
  pins.bck_io_num = I2S_BCLK_PIN;
  pins.ws_io_num = I2S_LRC_PIN;
  pins.data_out_num = I2S_DOUT_PIN;
  pins.data_in_num = I2S_PIN_NO_CHANGE;
  i2s_set_pin(I2S_NUM_1, &pins);

  i2s_zero_dma_buffer(I2S_NUM_1);
  speakerRunning = true;
}

void speakerStop() {
  if (!speakerRunning) return;
  // Vaciar antes de cerrar evita el "plop" del ultimo pedazo de DMA.
  i2s_zero_dma_buffer(I2S_NUM_1);
  i2s_driver_uninstall(I2S_NUM_1);
  speakerRunning = false;
}

// ======================= SONIDO DE PRUEBA =======================
//
// Un "hola" robotico sintetizado a mano. La receta del timbre son dos cosas:
//
//   1. Onda cuadrada en vez de senoidal: los armonicos impares son los que le
//      dan el zumbido metalico. Una senoidal pura suena a pitido de microondas.
//   2. Modulacion en anillo: la cuadrada se multiplica por otra mucho mas lenta
//      (~55Hz). Ese trémolo tan rapido es LA marca del robot de dibujitos; sin
//      el, queda una alarma cualquiera.
//
// Cada nota ademas desliza la frecuencia de principio a fin, que es lo que hace
// que "hable" en vez de pitar.

void speakerRobotSound() {
  const uint32_t SR = 22050;

  // Cada tramo: {frecuencia inicial, frecuencia final, duracion en ms}.
  // La secuencia sube, baja y vuelve a subir: leida como entonacion, suena a
  // pregunta contestada, no a alarma.
  static const struct { uint16_t f0, f1, ms; } notas[] = {
      {320, 520, 130}, {520, 430, 110}, {430, 700, 150},
      {700, 610, 110}, {380, 380,  70}, {610, 900, 190},
  };

  speakerStart(SR);

  static int16_t buf[256];
  float phase = 0.0f;    // portadora
  float ring = 0.0f;     // modulador

  for (uint8_t n = 0; n < sizeof(notas) / sizeof(notas[0]); n++) {
    const uint32_t total = (uint32_t)SR * notas[n].ms / 1000;
    uint32_t done = 0;

    while (done < total) {
      size_t chunk = total - done;
      if (chunk > 256) chunk = 256;

      for (size_t i = 0; i < chunk; i++) {
        const float t = (float)(done + i) / (float)total;   // 0..1 dentro de la nota
        const float freq = notas[n].f0 + (notas[n].f1 - notas[n].f0) * t;

        phase += freq / (float)SR;
        if (phase >= 1.0f) phase -= 1.0f;
        ring += 55.0f / (float)SR;
        if (ring >= 1.0f) ring -= 1.0f;

        const float carrier = (phase < 0.5f) ? 1.0f : -1.0f;
        const float mod     = (ring  < 0.5f) ? 1.0f : -1.0f;

        // Envolvente trapezoidal: 12% de subida y 12% de bajada. Sin esto, cada
        // nota arranca y corta de golpe y se escucha un "click" que puede
        // confundirse con un problema del amplificador.
        float env = 1.0f;
        if (t < 0.12f)      env = t / 0.12f;
        else if (t > 0.88f) env = (1.0f - t) / 0.12f;

        buf[i] = (int16_t)(carrier * mod * env * 7000.0f);
      }

      speakerWrite((const uint8_t *)buf, chunk * sizeof(int16_t));
      done += chunk;
    }
  }

  // Un ultimo bloque en silencio para que el DMA no repita la cola de la ultima
  // nota mientras se apaga.
  memset(buf, 0, sizeof(buf));
  speakerWrite((const uint8_t *)buf, sizeof(buf));

  speakerStop();
}

void speakerWrite(const uint8_t *data, size_t bytes) {
  if (!speakerRunning || bytes == 0) return;
  size_t written = 0;
  // i2s_write bloquea hasta que el DMA tenga lugar. Eso es justo lo que
  // queremos: le pone el ritmo a la descarga del audio desde el servidor.
  i2s_write(I2S_NUM_1, data, bytes, &written, portMAX_DELAY);
}

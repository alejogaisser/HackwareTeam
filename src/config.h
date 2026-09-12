/*
 * Credenciales y endpoints.
 * Los valores reales viven en secrets.h, que no se sube a ningun repo.
 */

#pragma once

#if __has_include("secrets.h")
  #include "secrets.h"
#else
  #warning "No existe secrets.h - copia secrets.h.example y completa tus datos"
  // OJO: este bloque es SOLO para que el proyecto compile sin secrets.h.
  // Nunca pegar credenciales de verdad aca: a diferencia de secrets.h, este
  // archivo NO esta en el .gitignore y termina en el repositorio.
  #define WIFI_SSID     "TU_RED_WIFI"
  #define WIFI_PASSWORD "TU_PASSWORD_WIFI"
  #define LLM_API_KEY   "TU_API_KEY_ACA"
  #define API_HOST      "api.openai.com"
  #define LLM_API_URL   "https://api.openai.com/v1/chat/completions"
  #define STT_API_PATH  "/v1/audio/transcriptions"
  #define TTS_API_URL   "https://api.openai.com/v1/audio/speech"
#endif

// Modelos. Se pueden cambiar sin tocar nada mas.
#define LLM_MODEL        "gpt-4o-mini"

// Transcripcion. gpt-4o-mini-transcribe reemplaza a whisper-1: entiende
// bastante mejor con ruido de fondo y con el microfono lejos, que es
// exactamente el caso de este robot. Usa el MISMO endpoint
// (/v1/audio/transcriptions) y devuelve el mismo {"text": "..."}, asi que el
// resto de voice.cpp no cambia.
//
// Una diferencia si: estos modelos NO aceptan response_format=verbose_json ni
// timestamps, solo "json" (el default) y "text". Aca se usa el default.
//
// Para mas calidad a mas costo: "gpt-4o-transcribe".
// Para volver atras: "whisper-1".
#define STT_MODEL        "gpt-4o-mini-transcribe"

#define TTS_MODEL        "tts-1"
#define TTS_VOICE        "alloy"
#define STT_LANGUAGE     "es"

// Personalidad del robot. Va como system prompt en cada consulta.
#define ROBOT_SYSTEM_PROMPT \
  "Sos un robot de juguete estilo WALL-E. Respondes en espanol rioplatense, " \
  "con frases cortas y simpaticas. Maximo dos oraciones, porque tu respuesta " \
  "se lee en voz alta por un parlante chico."

// El camino I2S + ADC del ESP32 entrega las muestras de a pares dados vuelta:
// llega antes la segunda que la primera. Eso no cambia el nivel -por eso el
// modo de prueba se ve perfecto- pero destroza la forma de onda, y la voz le
// llega al transcriptor como ruido.
// En 1 se reordenan al leer. Si la transcripcion sigue siendo basura con esto
// encendido, probar poniendolo en 0.
#define MIC_SWAP_SAMPLE_PAIRS   1

// Pico minimo de audio para dar por bueno lo que se grabo.
// Por debajo de esto la grabacion es practicamente silencio, y la transcripcion
// no es confiable: los servicios de voz a texto, cuando no escuchan nada,
// tienden a devolver frases que sacaron de sus datos de entrenamiento en vez de
// admitir que no hay audio. Mejor cortar antes que creerle a eso.
//
// Este es el chequeo DE SALIDA, sobre lo ya grabado. El de ENTRADA es el VAD de
// aca abajo, que directamente no deja que la grabacion empiece.
#define MIC_SILENCE_PEAK   500

// ======================= VAD (deteccion de voz) =======================
//
// Antes de mandar nada a transcribir, el robot espera a escuchar una voz de
// verdad. Sin esto, cada turno subia 5 segundos de lo que hubiera -ventilador,
// motores, nada- y el transcriptor contestaba con frases inventadas, que es su
// forma tipica de fallar cuando no hay voz en el audio.
//
// El criterio no es un umbral fijo sino relativo al ambiente: durante la espera
// se mide el piso de ruido y se pide que la voz lo supere por VAD_SNR_FACTOR.
// Asi el mismo numero sirve en una pieza callada y al lado de una ventana.
#define VAD_ENABLED           1

// Cuanto espera a que alguien empiece a hablar antes de cancelar el turno.
#define VAD_WAIT_MS           4000

// Piso absoluto: por debajo de este RMS no cuenta como voz ni aunque el
// ambiente este mudo. Es lo que evita que un lugar MUY silencioso deje el piso
// de ruido tan bajo que cualquier crujido pase el filtro relativo.
#define VAD_RMS_MIN           220

// Cuantas veces tiene que superar al ruido de fondo para contar como voz.
#define VAD_SNR_FACTOR        2.5f

// Cuadros seguidos (de 32ms) por encima del umbral para dar por arrancada la
// frase. 3 cuadros son ~100ms: filtra golpes y chasquidos, no una silaba.
#define VAD_TRIGGER_FRAMES    3

// Cuadros iniciales que se descartan. El estimador de continua del microfono
// (ver audio.cpp) tarda un poco en engancharse despues de micStart(), y esos
// primeros cuadros dan un nivel que no es el real.
#define VAD_WARMUP_FRAMES     12

// ======================= BRAZOS =======================
//
// Cero fisico del servo = brazo pegado al cuerpo, apuntando al piso.
// El recorrido util va de ahi hasta 90 grados (brazo horizontal al piso), con
// un poco de juego hacia atras, que se expresa como angulos negativos.
//
// Servo::write() no acepta negativos, asi que el rango logico va corrido: al
// angulo logico se le suma (o se le resta, en el brazo espejado) el valor de
// calibracion de aca abajo.
//
// Todo el resto del codigo habla SIEMPRE en angulos logicos; la conversion pasa
// en un solo lugar -armWriteLogical(), en main.cpp- y ahi mismo se recortan los
// limites, asi no hay forma de pasarse de 90 arriba ni de 10 hacia atras.
#define ARM_DEG_MIN        (-10)   // un poco hacia atras del cuerpo
#define ARM_DEG_MAX        90      // brazo horizontal al piso

// Valor que se le manda al servo para el 0 logico. Son constantes de
// CALIBRACION: hay que ajustarlas una vez, mirando el robot.
//
// El brazo izquierdo SUBE cuando el numero crece; el derecho esta montado
// espejado, asi que SUBE cuando el numero baja. De ahi que uno sume y el otro
// reste. (Antes esto se hacia con un "180 menos el valor", que es lo mismo
// pero deja el brazo derecho pegado al tope de la libreria.)
//
// POR QUE NO ARRANCAN EN 0 NI EN 180:
// La libreria mapea write(0..180) sobre el ancho de pulso de abajo. Con el 0
// logico en 10, el servo izquierdo recibia ~700us y el derecho ~2300us: los dos
// contra el extremo del recorrido. La mayoria de los SG90/MG90 clonados NO
// llegan ahi; se quedan trabados haciendo fuerza, zumbando, tirando de a ~700mA
// cada uno y sin moverse. Dejandolos en 25 y 145, el recorrido util queda en la
// zona comoda del servo y nunca se apoya contra el tope mecanico.
#define ARM_L_SERVO_ZERO   25
#define ARM_R_SERVO_ZERO   145

// Ancho de pulso del servo, en microsegundos. 500us es mas de lo que aceptan
// muchos servos baratos; 600 es un piso seguro. Si tus servos llegan mas lejos
// y queres mas recorrido, se puede bajar, pero probando de a poco.
#define ARM_SERVO_MIN_US   600
#define ARM_SERVO_MAX_US   2400

// ======================= MICROFONO DEL CELULAR (RESPALDO) =======================
//
// Si el microfono del robot no anda, se puede grabar con el del celular y
// subirle el audio al ESP32, que lo reenvia a transcribir. El resto del
// circuito (LLM y voz de respuesta) no cambia.
//
// El navegador graba en webm/opus, que a 5 segundos son unos 15 a 20KB. Por eso
// esto entra comodo en RAM y no hace falta streaming: el WAV crudo equivalente
// serian 160KB y no habria forma de sostenerlo.
//
// El tope esta para que un pedido mal formado -o alguien jugando con curl- no
// pueda pedir un malloc gigante y tirar la placa por falta de memoria.
#define PHONE_AUDIO_MAX_BYTES   (96 * 1024)

// Nombre y tipo con que se le declara el archivo al servicio de transcripcion.
// Tiene que coincidir con lo que graba el navegador (ver webui.h).
#define PHONE_AUDIO_FILENAME    "audio.webm"
#define PHONE_AUDIO_MIME        "audio/webm"

// ======================= WEB / CONTROL REMOTO =======================

// Puerto del servidor web. 80 para poder entrar escribiendo solo la IP.
#define WEB_PORT           80

// Nombre mDNS: ademas de por IP, el robot responde en http://walle.local/
// (funciona de una en iPhone y Mac; en Android depende de la version).
#define ROBOT_HOSTNAME     "walle"

// Si el celular deja de mandar la posicion del joystick por mas de esto, los
// motores frenan solos. Es la red de seguridad para cuando el telefono se
// bloquea, se va de la red o se cierra el navegador con el robot andando.
#define JOYSTICK_TIMEOUT_MS   600

// Velocidades del modo control.
#define MANUAL_PWM_MIN     110   // por debajo de esto el motor zumba y no gira
#define MANUAL_PWM_MAX     255

// Cada cuanto el robot le manda el estado (distancia, modo, cara) al celular.
#define TELEMETRY_MS       300

// ======================= MODO BOXEO =======================
//
// La frase es fija y va derecho al TTS, sin pasar por el LLM: pasarla por el
// LLM agregaria un par de segundos de espera y el gesto perderia toda la
// gracia. Igual hay que estar conectado, porque el TTS es un servicio web.
#define BOXEO_PHRASE       "A ver si te animas! Vamos, vamos!"
#define BOXEO_PWM          200   // fuerza de los tironcitos de avance
#define BOXEO_PULSE_MS     180   // cuanto dura cada tironcito
#define BOXEO_GAP_MS       220   // pausa entre tironcitos

// ======================= GESTOS AL HABLAR =======================
//
// Mientras suena la respuesta el robot se balancea y mueve los brazos. Los
// motores se usan en pulsos cortos y a baja velocidad: la idea es que cabecee
// en el lugar, no que se vaya caminando mientras contesta.
#define TALK_ROCK_PWM      130   // velocidad del balanceo
#define TALK_ROCK_MS       130   // cuanto empuja en cada sentido
#define TALK_ROCK_GAP_MS   170   // pausa entre empujon y empujon

// Los brazos oscilan con periodos distintos y a proposito no multiplos entre
// si: con periodos iguales los dos brazos suben juntos y parece una clase de
// gimnasia, no alguien gesticulando mientras habla.
#define TALK_ARM_BASE_DEG  25    // altura alrededor de la cual oscilan
#define TALK_ARM_AMP_DEG   20    // cuanto suben y bajan respecto de la base
#define TALK_ARM_L_MS      760   // periodo del brazo izquierdo
#define TALK_ARM_R_MS      1130  // periodo del derecho
#define TALK_ARM_STEP_MS   60    // cada cuanto se recalcula la posicion

// Cuanto queda el icono de microfono en la OLED al mutear o desmutear.
#define MUTE_ICON_MS       1200

// Volcado del audio por el puerto serie, para ESCUCHAR lo que graba el robot.
// En 1: al arrancar graba MIC_DUMP_SECONDS segundos, los guarda en memoria y
// los escupe en base64 entre dos marcas. El script tools/grabar_wav.py los
// convierte en un .wav que se puede reproducir.
// Es la unica forma de distinguir "no capta", "capta ruido" y "capta a la
// velocidad equivocada", que desde la transcripcion se ven todos iguales.
#define MIC_DUMP_MODE      0
#define MIC_DUMP_SECONDS   3

// Modo de prueba del microfono.
// En 1: el robot NO se mueve, no levanta los servos y no se conecta al WiFi.
// Solo lee el microfono y reporta el nivel por el monitor serie y en la OLED,
// para verificar el cableado y la ganancia sin gastar llamadas a la API.
// Volver a 0 para el funcionamiento normal.
#define MIC_TEST_MODE    0

// Cuantos segundos graba cada vez que se pide un turno de charla.
// El audio se manda a medida que se graba, asi que subir esto no gasta RAM,
// pero si alarga la espera antes de que conteste.
#define RECORD_SECONDS   5

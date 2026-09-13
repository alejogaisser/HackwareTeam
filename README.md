# HackwareTeam

Hackware Event: used for making THE project.

---

# WALL-E: robot con ESP32, control desde el celular y charla por voz

Un robot de juguete estilo WALL-E sobre un ESP32. Anda solo esquivando cosas,
se puede manejar desde el celular como con un joystick, y se le puede hablar:
escucha, piensa la respuesta con un modelo de lenguaje y **contesta en voz alta**
mientras mueve los brazos y la boca.

Este README cuenta lo que se logró en esta etapa **con las fallas incluidas**.
Hay cosas que compilan y se ven bien en la simulación pero que todavía no se
pudieron confirmar sobre el robot de verdad, y están marcadas como tales.

---

## Estado en una línea

**El firmware compila y la web funciona en el navegador, pero en la última
prueba con el hardware real el micrófono no captó, los servos no se movieron y
la pantalla no prendió.** Se encontraron y arreglaron varios defectos de código
que pueden explicarlo, y se agregó un panel de diagnóstico para confirmarlo,
pero **esos arreglos todavía no se probaron sobre el robot**.

---

## Qué se logró

### Control desde el celular
- El ESP32 sirve una página web propia. No hay app que instalar: se abre con el
  navegador del celular.
- Todo viaja por **WebSocket** (no por polling HTTP), así el joystick responde
  sin retraso.
- **Protegido con contraseña** (ver [Seguridad](#seguridad)).
- La página va embebida en el firmware, así nunca queda desfasada del código.
- Pensada para el dedo: botones grandes, sin zoom. Verificada sin solapamientos
  en pantallas de 360×600 a 393×852.

### Tres modos
| Modo | Qué hace |
|---|---|
| **OFF** | **Es el de arranque.** No se mueve nada. Igual escucha y contesta. |
| **CONTROL** | Joystick para los motores, un control por brazo y botón de boxeo. |
| **AUTO** | Anda solo esquivando obstáculos, y conversa. |

Al cambiar de modo el robot frena siempre.

### Seguridad del movimiento
- **El ultrasónico frena aunque el joystick diga que avance.** Se anula solo el
  avance: girar para salir del paso se puede.
- **Watchdog del joystick:** si el celular deja de mandar posiciones por 600 ms
  (se bloqueó la pantalla, se fue del WiFi), los motores frenan solos.
- **Brazos con límites lógicos:** de −10° (un poco hacia atrás del cuerpo) a 90°
  (horizontal al piso). El recorte pasa en un solo lugar y no hay forma de
  pasarse, se pida lo que se pida desde la web.

### Voz
- **Transcripción con `gpt-4o-mini-transcribe`**, que entiende mejor que
  `whisper-1` con ruido de fondo.
- **Detección de voz (VAD)** antes de mandar nada: si nadie habla, el turno se
  cancela sin gastar una llamada a la API. El umbral es relativo al ruido del
  ambiente, y se guardan unos cuadros previos para no cortar la primera sílaba.
- **Respuesta hablada** por TTS, reproducida por el parlante I2S a medida que
  baja (sin guardar el audio entero: no entraría en la RAM).
- **Gestos al hablar:** balanceo sutil adelante-atrás y brazos que suben y bajan
  sin sincronizarse entre sí. En modo OFF contesta sin moverse.
- **Modo boxeo:** levanta los brazos, dice una frase fija (sin pasar por el
  modelo, para que salga al toque) y avanza a los tirones.
- **Tres formas de hablarle**, para tener respaldo si una falla:
  1. el micrófono del robot,
  2. el micrófono del celular,
  3. escribirle el texto.

### Pantalla
- **Solo boca.** Los ojos del robot son el sensor ultrasónico; dibujar ojos en
  la pantalla daba una cara de cuatro ojos. Cada boca usa los 128×64 enteros.
- Bocas para: reposo, apagado, contento, alerta, escuchando, pensando, hablando,
  control y boxeo. Estilo geométrico.
- **La boca base aparece apenas se enchufa**, antes de esperar al WiFi. Sirve de
  prueba inmediata de que la pantalla anda.
- Ícono de micrófono (normal o tachado) al silenciar o activar el micrófono.

### Diagnóstico (pestaña DIAG)
- **Línea de estado en vivo:** conectando, escuchando, grabando, transcribiendo,
  pensando, hablando, y los errores con su código HTTP.
- **Barra de nivel del micrófono en vivo.** Si no se mueve al hablar, el problema
  está en el micrófono y no en la red.
- **SONIDO ROBOT:** un sonido generado dentro de la placa, **sin internet**. Es el
  único test que aísla de verdad el parlante.
- Pruebas de micrófono, brazos (de a uno), voz TTS y pantalla.
- **Autotest de arranque** con el **motivo del último reinicio**, la hora (NTP) y
  el estado de la verificación de certificados.

---

## Seguridad

El repositorio es público. Esto es lo que se hizo para que eso no exponga nada,
y — igual de importante — **lo que todavía no está cubierto**.

### Qué está protegido

**Ningún secreto en el repositorio.**
- La red WiFi, la API key y la contraseña web viven en `src/secrets.h`, que está
  en el `.gitignore`. En el repo solo está la plantilla `src/secrets.h.example`.
- **Chequeo automático:** en cada push y pull request, GitHub corre
  `tools/check_secrets.py`, que falla si aparece algo con forma de API key, una
  clave privada, `secrets.h`, o una contraseña con un valor real.
- En GitHub además están activados el escaneo de secretos y la protección que
  rechaza pushes con secretos.

**El control web pide contraseña.**
- Sin autenticarse, un celular no puede mandar ningún comando ni recibe nada: ni
  telemetría, ni diagnóstico, ni el diálogo.
- **La contraseña nunca viaja por la red**, ni siquiera cifrada. El robot manda un
  número al azar en cada conexión y el celular responde con
  `HMAC-SHA256(contraseña, "walle-auth:" + número)`. Quien espíe la WiFi no ve la
  contraseña, y lo que ve no le sirve para la próxima conexión.
- **Si la contraseña no está configurada** (o tiene menos de 8 caracteres), el
  control web queda **bloqueado**, no abierto.
- **5 intentos fallidos bloquean esa IP 30 segundos.** El bloqueo es por IP y no
  general, para que nadie pueda dejar afuera al dueño fallando a propósito.
- Las conexiones que no se autentican en 60 segundos se cortan, para que nadie
  ocupe todos los lugares del servidor.
- La subida de audio del celular exige un token de sesión en una cabecera HTTP.
- La página no se puede meter dentro de otro sitio (cabeceras contra clickjacking).

**La API key solo se le manda al servidor real.**
- Las conexiones con OpenAI **verifican el certificado** del servidor. Antes no
  lo hacían: en una red compartida alguien podía hacerse pasar por
  `api.openai.com` y quedarse con la key.
- La hora se sincroniza por NTP al arrancar, porque la verificación puede
  necesitar la fecha real.

### Qué NO está cubierto

- **El tráfico del celular al robot no está cifrado.** La página se sirve por
  HTTP: los comandos, los textos de la charla y el token de sesión viajan en
  claro por la WiFi. La contraseña no.
- **Una contraseña corta se puede adivinar.** Con lo que se ve de un login, alguien
  puede probar contraseñas sin conexión. Contra una de 12 caracteres al azar eso
  no es práctico; contra `12345678`, sí.
- **La API key está dentro del firmware.** Quien tenga el robot en la mano puede
  leerla de la memoria. **Usar una key exclusiva para el robot, con límite de
  gasto**, y revocarla si se pierde.
- **El certificado está fijado.** Si OpenAI cambia de autoridad certificante, las
  llamadas del robot fallan con un error claro hasta correr
  `tools/actualizar_certs.py`. Los certificados actuales vencen en enero de 2028
  y febrero de 2029.
- **"Recordar en este celular"** guarda la contraseña en el navegador de ese
  teléfono.
- **Alguien en la misma red puede molestar:** no puede manejar el robot, pero sí
  abrir conexiones una y otra vez. Hay límites, no una solución total.
- **Nada de esto se probó todavía en el ESP32 real** (ver abajo).

### Si se filtra una clave

Borrarla del repositorio **no alcanza**: queda en el historial y en copias. Hay
que **revocarla** en el panel de OpenAI (o cambiar la contraseña del WiFi / la
web) y generar una nueva.

---

## Fallas conocidas y cosas sin resolver

### 1. En el hardware real, micrófono, servos y pantalla no funcionaron
Es lo más importante de este README. En la última prueba sobre el robot:
- el micrófono no captó,
- los servos no se movieron,
- la pantalla OLED no prendió.

**No se llegó a confirmar la causa.** Lo que sí se hizo:

- **Bug encontrado y corregido en los servos — sin probar en hardware.** Una
  versión intermedia de este código mandaba los dos servos contra el extremo de
  su recorrido (~600 µs y ~2300 µs) **al mismo tiempo, en el arranque**. Muchos
  servos baratos no llegan ahí: se traban haciendo fuerza y consumen ~700 mA
  cada uno. Eso puede bajar la tensión de un powerbank y, de rebote, hacer que
  la pantalla no inicialice y el micrófono lea basura: un solo bug que explicaría
  los tres síntomas. Se corrigió, pero **es una hipótesis hasta probarla**.
- **La pantalla podía reportar que andaba sin estar conectada.** La librería
  devuelve OK sin verificar que haya alguien del otro lado. Ahora se hace un
  ping real al bus I2C y, si no contesta, se listan las direcciones que sí.
- **Una falla del micrófono se ignoraba en silencio** (`i2s_adc_enable` sin
  chequear). Ahora se reporta como problema de configuración.

**Sospecha abierta: la alimentación.** Con un powerbank, los picos de motores y
servos pueden resetear la placa. El autotest de arranque lo va a mostrar.

### 2. El micrófono del celular no anda por HTTP
`getUserMedia` exige conexión segura (HTTPS) y la página se sirve por
`http://<ip>`. **Ningún navegador da el micrófono así por defecto.**
- **Chrome en Android:** se puede habilitar agregando la IP del robot en
  `chrome://flags/#unsafely-treat-insecure-origin-as-secure`. Ojo: eso le da a
  esa dirección los permisos de un sitio seguro; usarlo solo con la IP del robot.
- **iPhone:** no hay forma. Ahí el respaldo es escribirle.

### 3. La calibración de los brazos es un punto de partida
Los valores del cero de cada brazo (`ARM_L_SERVO_ZERO = 25`,
`ARM_R_SERVO_ZERO = 145` en `src/config.h`) son razonables pero **no están
medidos sobre este robot**. Hay que ajustarlos mirando los brazos.

### 4. Otras limitaciones
- **El turno de voz es bloqueante:** mientras escucha y contesta, el robot se
  queda quieto (la web sigue respondiendo).
- **Silenciar el micrófono no corta un turno ya empezado**; aplica desde el
  siguiente.
- **El robot no crea su propio WiFi.** En redes con aislamiento de clientes
  (típico de redes de invitados) el celular no lo ve.
- **`http://walle.local/`** anda bien en iPhone y Mac; en Android depende de la
  versión. La IP siempre funciona.

### Cómo se verificó lo que se verificó
- **Compilación:** `pio run` sin errores ni warnings en el código del proyecto.
- **Web:** probada en un navegador con viewports de celular, simulando los
  mensajes del robot. Sintaxis del JavaScript verificada con Node.
- **Criptografía del login:** el HMAC-SHA256 escrito en JavaScript se comparó
  contra el de Python en 18 casos, incluido el vector oficial RFC 4231, los
  bordes del relleno de SHA-256 y contraseñas con caracteres no ASCII.
- **Certificados:** tomados de la cadena real que presenta hoy `api.openai.com`.
- **Bocas de la pantalla:** con un simulador que reimplementa las mismas
  primitivas de dibujo.
- **Sobre el robot real:** las funciones nuevas, incluido el login, **no** se
  probaron todavía.

---

## Cómo usarlo

### 1. Credenciales
```
cp src/secrets.h.example src/secrets.h
```
Completar en `src/secrets.h`:
- la red WiFi,
- la API key de OpenAI (**mejor una exclusiva para el robot, con límite de gasto**),
- `WEB_PASSWORD`: la contraseña del control web. **Obligatoria**, 8 caracteres o
  más; recomendado 12 o más, al azar y distinta de la del WiFi.

**`secrets.h` está en el `.gitignore` y no se sube nunca.**

### 2. Compilar y subir
```
pio run --target upload
pio device monitor
```

### 3. Entrar desde el celular
- El celular tiene que estar en **la misma red WiFi** que el robot, y esa red
  tiene que ser de **2.4 GHz** (el ESP32 no usa 5 GHz).
- El monitor serie imprime la dirección al arrancar, por ejemplo
  `http://192.168.1.47/`. También responde en `http://walle.local/`.
- Escribirla **con `http://` adelante**; con `https://` no funciona.
- La página pide la contraseña (`WEB_PASSWORD`).

### 4. Orden sugerido para probar
1. Enchufar: tiene que aparecer la boca base. Si no, la pantalla no anda.
2. Abrir la web y entrar con la contraseña: arranca en OFF.
3. **DIAG → SONIDO ROBOT** (no usa internet).
4. **DIAG → PROBAR MIC** y **PROBAR BRAZOS**.
5. Si el micrófono no capta, **escribirle**: si contesta con voz, la red, la key
   y el modelo están bien.

### 5. Antes de cada commit
```
python tools/check_secrets.py
```
Es el mismo chequeo que corre GitHub. Mejor que falle en tu compu que en el repo
público.

---

## Hardware

| Parte | Módulo | Pines |
|---|---|---|
| Placa | ESP32-WROOM | — |
| Motores | driver tipo L298N | A: IN1 26, IN2 27, EN 14 · B: IN1 25, IN2 33, EN 32 |
| Ultrasónico | HC-SR04 | TRIG 5, ECHO 18 (**con divisor resistivo**) |
| Pantalla | OLED SSD1306 128×64, I2C | SDA 21, SCL 22, dirección 0x3C |
| Brazo izquierdo | servo | GPIO 13 |
| Brazo derecho | servo | GPIO 19 |
| Micrófono | GY-MAX9814 (**analógico**, no I2S) | GPIO 35 (ADC1), a 3.3 V |
| Parlante | MAX98357A (I2S) | BCLK 16, LRC 17, DIN 4 |
| Botón hablar | pulsador a GND | GPIO 23 |

Todos los pines están en `src/pins.h`, con las notas de cableado.

---

## Estructura

```
platformio.ini          placa, librerías, particiones
src/
  main.cpp              modos, movimiento, brazos, pantalla, turnos de voz, autotests
  remote.cpp/.h         servidor web + WebSocket + login (nunca toca hardware directo)
  webui.h               la página web embebida
  voice.cpp/.h          transcripción, VAD, modelo de lenguaje, voz, TLS
  audio.cpp/.h          micrófono (ADC por I2S) y parlante (I2S)
  api_certs.h           certificados públicos para verificar api.openai.com
  config.h              modelos, umbrales, calibración, seguridad, tiempos
  pins.h                todos los pines
  secrets.h.example     plantilla de credenciales (secrets.h NO va al repo)
tools/
  check_secrets.py      busca secretos antes de commitear (también corre en GitHub)
  actualizar_certs.py   regenera api_certs.h si OpenAI cambia de certificado
  grabar_wav.py         convierte un volcado del micrófono en un .wav para escucharlo
.github/workflows/
  secretos.yml          chequeo de secretos en cada push y pull request
```

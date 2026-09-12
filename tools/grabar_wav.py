# -*- coding: utf-8 -*-
"""
Convierte el volcado de audio del robot en un archivo .wav que se puede escuchar.

El firmware, con MIC_DUMP_MODE en 1, graba unos segundos y los escupe por el
puerto serie en base64, entre las marcas ----WAV-BEGIN---- y ----WAV-END----.
Este script las busca, decodifica lo del medio y guarda un .wav.

USO
---
Leyendo directo del puerto (recomendado): hay que apretar reset en el ESP32
despues de lanzar el script, porque el volcado ocurre una sola vez al arrancar.

    python tools/grabar_wav.py COM5

Si preferis copiar la salida del monitor serie a un archivo de texto:

    python tools/grabar_wav.py salida.txt

En los dos casos escribe mic.wav en la carpeta actual.

NOTA: usar el python de PlatformIO, que ya trae pyserial instalado:
    %USERPROFILE%\\.platformio\\penv\\Scripts\\python.exe tools/grabar_wav.py COM5
"""

import base64
import os
import sys
import time

INICIO = "----WAV-BEGIN----"
FIN = "----WAV-END----"
SALIDA = "mic.wav"


def leer_de_archivo(ruta):
    with open(ruta, "r", encoding="utf-8", errors="ignore") as f:
        texto = f.read()

    if INICIO not in texto:
        sys.exit("No encontre la marca %s en %s.\n"
                 "Verifica que MIC_DUMP_MODE este en 1 y que hayas copiado "
                 "toda la salida del monitor." % (INICIO, ruta))
    if FIN not in texto:
        sys.exit("Encontre el comienzo pero no la marca %s: el volcado quedo "
                 "cortado por la mitad." % FIN)

    cuerpo = texto.split(INICIO, 1)[1].split(FIN, 1)[0]
    return "".join(cuerpo.split())


def leer_de_puerto(puerto, baud=115200, espera=90):
    try:
        import serial
    except ImportError:
        sys.exit("Falta pyserial. Usa el python de PlatformIO:\n"
                 "  %USERPROFILE%\\.platformio\\penv\\Scripts\\python.exe "
                 "tools/grabar_wav.py " + puerto)

    print("Abriendo %s a %d baudios..." % (puerto, baud))
    try:
        sp = serial.Serial(puerto, baud, timeout=1)
    except Exception as e:
        sys.exit("No pude abrir %s: %s\n"
                 "Cerra el monitor serie de VS Code, que se queda con el "
                 "puerto tomado." % (puerto, e))

    print("Escuchando. APRETA EL BOTON DE RESET del ESP32 ahora.")
    print("(el volcado se hace una sola vez, al arrancar)")

    limite = time.time() + espera
    juntando = False
    partes = []

    with sp:
        while time.time() < limite:
            linea = sp.readline().decode("utf-8", errors="ignore").strip()
            if not linea:
                continue

            if INICIO in linea:
                juntando = True
                print("Volcado detectado, recibiendo...")
                continue
            if FIN in linea:
                print("Fin del volcado.")
                return "".join(partes)

            if juntando:
                partes.append(linea)
                if len(partes) % 200 == 0:
                    print("  %d lineas..." % len(partes))
            else:
                # Mientras no empiece, mostramos lo que dice el robot: ahi salen
                # el conteo de muestras y las primeras muestras en crudo.
                print("  | " + linea)

    if juntando:
        sys.exit("Se corto a la mitad: llegue a %d lineas y nunca vino la "
                 "marca de fin." % len(partes))
    sys.exit("Pasaron %d segundos sin volcado.\n"
             "Revisa que MIC_DUMP_MODE este en 1 en config.h, que hayas "
             "cargado el firmware, y que estes apretando reset." % espera)


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)

    origen = sys.argv[1]

    if os.path.exists(origen):
        b64 = leer_de_archivo(origen)
    else:
        b64 = leer_de_puerto(origen)

    if not b64:
        sys.exit("No junte ningun dato.")

    try:
        crudo = base64.b64decode(b64)
    except Exception as e:
        sys.exit("El base64 no decodifica: %s\n"
                 "Suele pasar cuando se perdieron caracteres por el camino. "
                 "Proba de nuevo." % e)

    if len(crudo) < 45:
        sys.exit("Llegaron solo %d bytes: no alcanza ni para la cabecera WAV."
                 % len(crudo))

    with open(SALIDA, "wb") as f:
        f.write(crudo)

    muestras = (len(crudo) - 44) // 2
    frecuencia = int.from_bytes(crudo[24:28], "little")
    segundos = muestras / float(frecuencia) if frecuencia else 0

    print()
    print("Guardado: %s" % os.path.abspath(SALIDA))
    print("  %d bytes, %d muestras, %d Hz, %.2f segundos"
          % (len(crudo), muestras, frecuencia, segundos))
    print()
    print("Escuchalo y fijate:")
    print("  - se entiende tu voz     -> el microfono esta bien, el problema es otro")
    print("  - se escucha grave y lento o agudo y rapido -> la frecuencia real")
    print("    del ADC no es la que declara el WAV")
    print("  - solo ruido o zumbido   -> capta, pero no tu voz: interferencia")
    print("  - silencio total         -> no esta captando nada")


if __name__ == "__main__":
    main()

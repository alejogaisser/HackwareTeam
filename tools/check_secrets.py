# -*- coding: utf-8 -*-
"""
Busca secretos antes de que lleguen al repositorio publico.

Lo corre GitHub en cada push y en cada pull request (.github/workflows), y
conviene correrlo a mano antes de commitear:

    python tools/check_secrets.py

Revisa los archivos que git va a subir (los trackeados y los que estan en
stage). Termina con error si encuentra:
  - src/secrets.h dentro del repo,
  - algo con forma de API key de OpenAI (sk-...),
  - una clave privada,
  - WIFI_PASSWORD, LLM_API_KEY o WEB_PASSWORD con un valor que no sea de ejemplo.

No reemplaza al escaneo de secretos de GitHub: es una segunda red, pensada para
atajar el error tipico de este proyecto (pegar la key en config.h en vez de en
secrets.h).
"""

import re
import subprocess
import sys

# Valores que se consideran de ejemplo y estan permitidos, ademas del vacio.
# El vacio va aparte y NO en esta tupla: todo texto "empieza con" "", asi que
# incluirlo aca dejaba pasar cualquier contraseña real sin avisar.
PLACEHOLDERS = ("TU_", "CAMBIAME")

PATRONES = [
    ("API key de OpenAI", re.compile(r"sk-(?:proj-|svcacct-|admin-)?[A-Za-z0-9_-]{20,}")),
    ("clave privada", re.compile(r"-----BEGIN (?:RSA |EC |OPENSSH |DSA )?PRIVATE KEY-----")),
]
DEFINE = re.compile(r'#define\s+(WIFI_PASSWORD|LLM_API_KEY|WEB_PASSWORD)\s+"([^"]*)"')


def archivos():
    salida = subprocess.run(["git", "ls-files", "--cached"], capture_output=True, text=True)
    if salida.returncode != 0:
        sys.exit("Esto se corre adentro del repositorio git.")
    return [f for f in salida.stdout.splitlines() if f]


def main():
    problemas = []
    for ruta in archivos():
        if re.search(r"(^|/)secrets\.h$", ruta):
            problemas.append(f"{ruta}: secrets.h NO puede estar en el repo")
            continue
        try:
            with open(ruta, encoding="utf-8", errors="ignore") as f:
                texto = f.read()
        except (IsADirectoryError, FileNotFoundError):
            continue
        for n, linea in enumerate(texto.splitlines(), 1):
            for nombre, patron in PATRONES:
                if patron.search(linea):
                    problemas.append(f"{ruta}:{n}: parece una {nombre}")
            for var, valor in DEFINE.findall(linea):
                if valor and not valor.startswith(PLACEHOLDERS):
                    problemas.append(f"{ruta}:{n}: {var} tiene un valor real (va en secrets.h)")

    if problemas:
        print("SE ENCONTRARON POSIBLES SECRETOS:")
        for p in problemas:
            print("  -", p)
        print("\nSacarlos, mover los valores a src/secrets.h y, si llegaron a")
        print("pushearse, CAMBIAR la clave: borrarla del repo no alcanza.")
        sys.exit(1)
    print("Sin secretos a la vista.")


if __name__ == "__main__":
    main()

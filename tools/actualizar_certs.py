# -*- coding: utf-8 -*-
"""
Regenera src/api_certs.h con los certificados que presenta hoy api.openai.com.

El robot valida la conexion con la API contra esos certificados. Si OpenAI (o
quien le da el certificado) cambia de autoridad certificante, las llamadas del
robot empiezan a fallar con "No se pudo verificar la conexion segura". Ese es el
momento de correr este script, revisar lo que imprime, compilar y subir.

USO
---
    python tools/actualizar_certs.py

Necesita el comando openssl (viene con Git para Windows, y en Linux/Mac ya esta).

Que guarda y por que: TODOS los certificados de la cadena salvo el del propio
servidor. El del servidor cambia cada pocos meses; el intermedio y la raiz duran
anos, y validar contra ellos sigue funcionando cuando renuevan el del servidor.
"""

import re
import subprocess
import sys
from pathlib import Path

HOST = "api.openai.com"
DESTINO = Path(__file__).resolve().parent.parent / "src" / "api_certs.h"


def obtener_cadena():
    try:
        salida = subprocess.run(
            ["openssl", "s_client", "-connect", HOST + ":443", "-servername", HOST,
             "-showcerts"],
            input=b"", capture_output=True, timeout=30,
        ).stdout.decode("ascii", "replace")
    except FileNotFoundError:
        sys.exit("No se encontro openssl. Instalar Git para Windows o OpenSSL.")
    certs = re.findall(r"-----BEGIN CERTIFICATE-----.*?-----END CERTIFICATE-----",
                       salida, flags=re.S)
    if len(certs) < 2:
        sys.exit("No llego la cadena de certificados. Hay internet?")
    return certs


def describir(pem):
    info = subprocess.run(
        ["openssl", "x509", "-noout", "-subject", "-issuer", "-enddate"],
        input=pem.encode(), capture_output=True,
    ).stdout.decode("ascii", "replace").strip()
    return [linea.strip() for linea in info.splitlines()]


def main():
    certs = obtener_cadena()
    guardar = certs[1:]   # sin el certificado del servidor

    lineas = [
        "/*",
        " * Certificados para validar la conexion con " + HOST + ".",
        " * GENERADO por tools/actualizar_certs.py - no editar a mano.",
        " *",
        " * Son certificados PUBLICOS (no hay nada secreto aca): cualquiera los ve",
        " * al conectarse al servidor. Sirven para que el robot compruebe que habla",
        " * con el servidor real antes de mandarle la API key.",
        " *",
    ]
    for pem in guardar:
        for d in describir(pem):
            lineas.append(" *   " + d)
        lineas.append(" *")
    lineas += [" */", "", "#pragma once", "", "static const char API_ROOT_CA[] ="]
    for pem in guardar:
        for fila in pem.strip().splitlines():
            lineas.append('  "' + fila.strip() + '\\n"')
    lineas[-1] = lineas[-1] + ";"
    lineas.append("")

    DESTINO.write_text("\n".join(lineas), encoding="utf-8", newline="\n")
    print("Escrito", DESTINO)
    for pem in guardar:
        print("  -", " | ".join(describir(pem)))


if __name__ == "__main__":
    main()

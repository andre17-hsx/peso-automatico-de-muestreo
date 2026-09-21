#pragma once
#include <WebServer.h>

// Registra todas las rutas en el servidor.  Llamar antes de server.begin().
void webBegin(WebServer& srv);

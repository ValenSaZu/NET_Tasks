#ifndef SALA_H
#define SALA_H

#include <cstring>

struct Silla {
    int patas;
    bool conRespaldo;
};

struct Sillon {
    int capacidad;
    char color[20];
};

struct Cocina {
    bool electrica;
    float metrosCuadrados;
};

struct Sala {
    Silla silla;
    Sillon sillon;
    Cocina *cocina;        // se enviará el contenido apuntado, no el puntero
    int n;
    char descripcion[1000];
};

#endif

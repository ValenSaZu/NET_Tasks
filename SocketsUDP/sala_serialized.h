#include <vector>
#include <cstdint>
#include <cstring>
#include "sala.h"

std::vector<char> serializarSala(const Sala &sala) {
    std::vector<char> buffer;

    // Silla
    buffer.insert(buffer.end(),
        reinterpret_cast<const char*>(&sala.silla),
        reinterpret_cast<const char*>(&sala.silla) + sizeof(Silla));

    // Sillon
    buffer.insert(buffer.end(),
        reinterpret_cast<const char*>(&sala.sillon),
        reinterpret_cast<const char*>(&sala.sillon) + sizeof(Sillon));

    // Cocina (copiamos el contenido, no el puntero)
    if (sala.cocina) {
        buffer.insert(buffer.end(),
            reinterpret_cast<const char*>(sala.cocina),
            reinterpret_cast<const char*>(sala.cocina) + sizeof(Cocina));
    } else {
        Cocina vacia{};
        buffer.insert(buffer.end(),
            reinterpret_cast<const char*>(&vacia),
            reinterpret_cast<const char*>(&vacia) + sizeof(Cocina));
    }

    // entero n
    buffer.insert(buffer.end(),
        reinterpret_cast<const char*>(&sala.n),
        reinterpret_cast<const char*>(&sala.n) + sizeof(int));

    // descripción
    buffer.insert(buffer.end(),
        sala.descripcion,
        sala.descripcion + 1000);

    return buffer;
}

Sala deserializeSala(const std::vector<char>& buffer) {
    Sala sala;
    size_t offset = 0;

    // Silla
    memcpy(&sala.silla, &buffer[offset], sizeof(Silla));
    offset += sizeof(Silla);

    // Sillon
    memcpy(&sala.sillon, &buffer[offset], sizeof(Sillon));
    offset += sizeof(Sillon);

    // Cocina - siempre asignamos memoria
    sala.cocina = new Cocina();
    memcpy(sala.cocina, &buffer[offset], sizeof(Cocina));
    offset += sizeof(Cocina);

    // entero n
    memcpy(&sala.n, &buffer[offset], sizeof(int));
    offset += sizeof(int);

    // descripción
    memcpy(sala.descripcion, &buffer[offset], 1000);

    return sala;
}
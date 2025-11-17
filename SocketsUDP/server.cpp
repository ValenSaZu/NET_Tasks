#include <iostream>
#include <string>
#include <thread>
#include <map>
#include <vector>
#include <mutex>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <unordered_map>
#include "sala.h"
#include "sala_serialized.h"
#include <vector>
#include <algorithm>

using namespace std;

#define PORT 45000
#define MAX_DATAGRAM_SIZE 1024

int maxDatagramLength = 777;

struct ClientInfo {
    int socket_fd;
    sockaddr_in address;
    socklen_t addr_len;
};
map<string, ClientInfo> clients;
mutex clients_mutex;

// Estructura para reconstrucción de paquetes fragmentados
struct FragmentReassembly {
    vector<string> fragments;
    string fullData;
    char messageType;
    size_t totalSize;
    size_t receivedSize;
    time_t lastFragmentTime;
};

unordered_map<string, FragmentReassembly> reassemblyBuffers;
mutex reassembly_mutex;

struct Game {
    string player1;
    string player2;
    vector<char> board;
    string currentPlayer;
    bool gameActive;
};

map<pair<string, string>, Game> activeGames;
mutex games_mutex;

// Declaraciones de funciones
vector<string> buildBroadcast(const string& sender, const string& msg);
vector<string> buildToClient(const string& sender, const string& msg);
vector<string> buildFile(const string& sender, const string& filename, const char* file_data, uint64_t file_size);
vector<string> buildObject(const string& sender, const vector<char>& objectData);
vector<string> buildGameRequest(const string& sender);
vector<string> buildGameResponse(const string& sender, bool accepted);
vector<string> buildBoard(const vector<char>& board, const string& currentPlayer);
vector<string> buildGameResult(char result);
vector<string> buildError(const string& msg);
vector<string> buildList();
vector<string> buildClose();

void sendAll(const vector<string> packets, const string& sender_nickname);
void sendToClient(const string dest, const vector<string> packets);
void initializeGame(Game& game, const string& p1, const string& p2);
void processGameMove(const string& player, uint32_t position);
void processCompleteMessage(const string& client_nickname, const string& fullData, char messageType, 
                           const sockaddr_in& client_addr, socklen_t addr_len, int server_fd);

string completeDatagram(string &packet) {
    size_t len = packet.size();
    if (len < (size_t)maxDatagramLength) {
        packet.append(maxDatagramLength - len, '#');
    }
    return packet;
}

vector<string> fragmentDatagram(const string &header, const string &data, int sizeFieldBytes) {
    vector<string> datagrams;

    size_t overhead = 1 + header.size() + sizeFieldBytes;
    size_t maxDataPerPacket = maxDatagramLength - overhead;
    size_t offset = 0;
    int numPacket = 1;
    int totalPackets = (data.size() + maxDataPerPacket - 1) / maxDataPerPacket;

    while (offset < data.size()) {
        size_t remaining = data.size() - offset;
        size_t chunkSize = min(remaining, maxDataPerPacket);
        bool isLast = (offset + chunkSize >= data.size());

        string fragment;
        fragment.push_back(isLast ? 0 : (char)numPacket);

        fragment += header;

        uint32_t len = (uint32_t)chunkSize;
        for (int i = sizeFieldBytes - 1; i >= 0; --i)
            fragment.push_back((len >> (8 * i)) & 0xFF);

        fragment += data.substr(offset, chunkSize);

        if (fragment.size() < (size_t)maxDatagramLength)
            fragment = completeDatagram(fragment);

        datagrams.push_back(fragment);
        offset += chunkSize;
        numPacket++;
    }

    return datagrams;
}

// Función para reconstruir paquetes fragmentados
bool reconstructPacket(const string& client_id, const string& fragment, string& fullData, char& messageType) {
    lock_guard<mutex> lock(reassembly_mutex);
    
    if (fragment.empty()) {
        return false;
    }

    char fragmentNum = fragment[0];
    
    // Si es un fragmento (número 1-9) o último fragmento (0)
    if (fragmentNum >= 0 && fragmentNum <= 9) {
        // El tipo de mensaje está en el segundo byte
        if (fragment.size() > 1) {
            messageType = fragment[1];
        } else {
            return false;
        }

        if (fragmentNum != 0) {
            // Fragmento intermedio
            if (reassemblyBuffers.find(client_id) == reassemblyBuffers.end()) {
                FragmentReassembly reassembly;
                reassembly.fragments.push_back(fragment);
                reassembly.messageType = messageType;
                reassembly.lastFragmentTime = time(nullptr);
                reassemblyBuffers[client_id] = reassembly;
            } else {
                reassemblyBuffers[client_id].fragments.push_back(fragment);
                reassemblyBuffers[client_id].lastFragmentTime = time(nullptr);
            }
            return false;
        } else {
            // Último fragmento
            if (reassemblyBuffers.find(client_id) != reassemblyBuffers.end()) {
                auto& reassembly = reassemblyBuffers[client_id];
                reassembly.fragments.push_back(fragment);
                
                // Reconstruir en orden (no necesitamos ordenar si llegan en orden)
                for (const auto& frag : reassembly.fragments) {
                    // Saltar el byte de número de fragmento y agregar el resto
                    fullData += frag.substr(1);
                }
                
                messageType = reassembly.messageType;
                reassemblyBuffers.erase(client_id);
                return true;
            } else {
                // Si es el único fragmento (mensaje pequeño)
                fullData = fragment.substr(1);
                return true;
            }
        }
    } else {
        // No es un fragmento, es mensaje simple
        fullData = fragment;
        messageType = fragment[0];
        return true;
    }
}

// Función separada para procesar mensajes de nickname
void processNicknameMessage(int server_fd, const string& data, const sockaddr_in& client_addr, socklen_t addr_len) {
    size_t offset = 1;
    if (data.size() < offset + 2) return;
    
    uint16_t nlen;
    memcpy(&nlen, data.c_str() + offset, 2);
    nlen = ntohs(nlen);
    offset += 2;
    
    if (data.size() < offset + nlen) return;
    string nickname(data.c_str() + offset, nlen);
    
    {
        lock_guard<mutex> lock(clients_mutex);
        if (clients.count(nickname)) {
            vector<string> err = buildError("Nickname already taken");
            for (const auto& packet : err) {
                sendto(server_fd, packet.c_str(), packet.size(), 0, (struct sockaddr*)&client_addr, addr_len);
            }
            return;
        }
        ClientInfo info = {server_fd, client_addr, addr_len};
        clients[nickname] = info;
        cout << nickname << " connected" << endl;
    }
}

// Función para procesar mensajes completos (simples o reconstruidos)
void processCompleteMessage(const string& client_nickname, const string& fullData, char messageType, 
                           const sockaddr_in& client_addr, socklen_t addr_len, int server_fd) {
    
    size_t offset = 0;
    
    switch (messageType) {
        case 'm': { // Broadcast message
            if (fullData.size() < offset + 2) return;
            uint16_t slen;
            memcpy(&slen, fullData.c_str() + offset, 2);
            slen = ntohs(slen);
            offset += 2;
            
            if (fullData.size() < offset + slen) return;
            string sender(fullData.c_str() + offset, slen);
            offset += slen;
            
            if (fullData.size() < offset + 3) return;
            uint32_t mlen = ((unsigned char)fullData[offset] << 16) |
                           ((unsigned char)fullData[offset+1] << 8) |
                           (unsigned char)fullData[offset+2];
            offset += 3;
            
            if (fullData.size() < offset + mlen) return;
            string message(fullData.c_str() + offset, mlen);
            
            cout << sender << " sent broadcast: " << message << endl;
            vector<string> packets = buildBroadcast(sender, message);
            sendAll(packets, sender);
            break;
        }
        
        case 't': { // Private message
            if (fullData.size() < offset + 2) return;
            uint16_t slen;
            memcpy(&slen, fullData.c_str() + offset, 2);
            slen = ntohs(slen);
            offset += 2;
            
            if (fullData.size() < offset + slen) return;
            string sender(fullData.c_str() + offset, slen);
            offset += slen;
            
            if (fullData.size() < offset + 2) return;
            uint16_t dlen;
            memcpy(&dlen, fullData.c_str() + offset, 2);
            dlen = ntohs(dlen);
            offset += 2;
            
            if (fullData.size() < offset + dlen) return;
            string dest(fullData.c_str() + offset, dlen);
            offset += dlen;
            
            if (fullData.size() < offset + 3) return;
            uint32_t mlen = ((unsigned char)fullData[offset] << 16) |
                           ((unsigned char)fullData[offset+1] << 8) |
                           (unsigned char)fullData[offset+2];
            offset += 3;
            
            if (fullData.size() < offset + mlen) return;
            string message(fullData.c_str() + offset, mlen);
            
            cout << sender << " sent private to " << dest << ": " << message << endl;
            vector<string> packets = buildToClient(sender, message);
            sendToClient(dest, packets);
            break;
        }
        
        case 'f': { // File transfer
            if (fullData.size() < offset + 2) return;
            uint16_t slen;
            memcpy(&slen, fullData.c_str() + offset, 2);
            slen = ntohs(slen);
            offset += 2;
            
            if (fullData.size() < offset + slen) return;
            string sender(fullData.c_str() + offset, slen);
            offset += slen;
            
            if (fullData.size() < offset + 2) return;
            uint16_t dlen;
            memcpy(&dlen, fullData.c_str() + offset, 2);
            dlen = ntohs(dlen);
            offset += 2;
            
            if (fullData.size() < offset + dlen) return;
            string dest(fullData.c_str() + offset, dlen);
            offset += dlen;
            
            if (fullData.size() < offset + 3) return;
            uint32_t flen = ((unsigned char)fullData[offset] << 16) |
                           ((unsigned char)fullData[offset+1] << 8) |
                           (unsigned char)fullData[offset+2];
            offset += 3;
            
            if (fullData.size() < offset + flen) return;
            string filename(fullData.c_str() + offset, flen);
            offset += flen;
            
            if (fullData.size() < offset + 10) return;
            uint64_t file_size = 0;
            for (int i = 0; i < 10; i++) {
                file_size = (file_size << 8) | (unsigned char)fullData[offset++];
            }
            
            if (fullData.size() < offset + file_size) return;
            vector<char> file_data(fullData.begin() + offset, fullData.begin() + offset + file_size);
            
            cout << sender << " sent file to " << dest << ": " << filename << " (" << file_size << " bytes)" << endl;
            
            // Reenviar archivo al destinatario
            vector<string> packets = buildFile(sender, filename, file_data.data(), file_size);
            sendToClient(dest, packets);
            break;
        }
        
        case 'o': { // Object transfer
            cout << "DEBUG: Processing object transfer" << endl;
            if (fullData.size() < offset + 2) {
                cout << "DEBUG: Object data too short for sender length" << endl;
                return;
            }
            uint16_t slen;
            memcpy(&slen, fullData.c_str() + offset, 2);
            slen = ntohs(slen);
            offset += 2;
            
            if (fullData.size() < offset + slen) {
                cout << "DEBUG: Object data too short for sender name" << endl;
                return;
            }
            string sender(fullData.c_str() + offset, slen);
            offset += slen;
            
            cout << "DEBUG: Object from " << sender << endl;
            
            if (fullData.size() < offset + 2) {
                cout << "DEBUG: Object data too short for destination length" << endl;
                return;
            }
            uint16_t dlen;
            memcpy(&dlen, fullData.c_str() + offset, 2);
            dlen = ntohs(dlen);
            offset += 2;
            
            if (fullData.size() < offset + dlen) {
                cout << "DEBUG: Object data too short for destination name" << endl;
                return;
            }
            string dest(fullData.c_str() + offset, dlen);
            offset += dlen;
            
            if (fullData.size() < offset + 4) {
                cout << "DEBUG: Object data too short for object size" << endl;
                return;
            }
            uint32_t objSize;
            memcpy(&objSize, fullData.c_str() + offset, 4);
            objSize = ntohl(objSize);
            offset += 4;
            
            cout << "DEBUG: Object size: " << objSize << ", available: " << (fullData.size() - offset) << endl;
            
            if (fullData.size() < offset + objSize) {
                cout << "DEBUG: ERROR - Not enough data for object" << endl;
                return;
            }
            vector<char> objectData(fullData.begin() + offset, fullData.begin() + offset + objSize);
            
            cout << "DEBUG: Object data received, forwarding to " << dest << endl;
            
            // Reenviar objeto al destinatario
            vector<string> packets = buildObject(sender, objectData);
            sendToClient(dest, packets);
            break;
        }
        
        case 'J': { // Game request
            if (fullData.size() < offset + 2) return;
            uint16_t slen;
            memcpy(&slen, fullData.c_str() + offset, 2);
            slen = ntohs(slen);
            offset += 2;
            
            if (fullData.size() < offset + slen) return;
            string sender(fullData.c_str() + offset, slen);
            offset += slen;
            
            if (fullData.size() < offset + 2) return;
            uint16_t dlen;
            memcpy(&dlen, fullData.c_str() + offset, 2);
            dlen = ntohs(dlen);
            offset += 2;
            
            if (fullData.size() < offset + dlen) return;
            string dest(fullData.c_str() + offset, dlen);
            
            cout << sender << " sent game request to " << dest << endl;
            vector<string> packets = buildGameRequest(sender);
            sendToClient(dest, packets);
            break;
        }
        
        case 'j': { // Game response
            if (fullData.size() < offset + 2) return;
            uint16_t slen;
            memcpy(&slen, fullData.c_str() + offset, 2);
            slen = ntohs(slen);
            offset += 2;
            
            if (fullData.size() < offset + slen) return;
            string sender(fullData.c_str() + offset, slen);
            offset += slen;
            
            if (fullData.size() < offset + 2) return;
            uint16_t rlen;
            memcpy(&rlen, fullData.c_str() + offset, 2);
            rlen = ntohs(rlen);
            offset += 2;
            
            if (fullData.size() < offset + rlen) return;
            string responder(fullData.c_str() + offset, rlen);
            offset += rlen;
            
            if (fullData.size() < offset + 1) return;
            char response = fullData[offset];
            
            cout << sender << " responded to game request from " << responder << ": " << response << endl;
            
            vector<string> packets = buildGameResponse(sender, response == 'y');
            sendToClient(responder, packets);
            
            if (response == 'y') {
                lock_guard<mutex> lock(games_mutex);
                pair<string, string> gameKey = make_pair(min(sender, responder), max(sender, responder));
                initializeGame(activeGames[gameKey], sender, responder);
                
                vector<string> boardPackets = buildBoard(activeGames[gameKey].board, activeGames[gameKey].currentPlayer);
                sendToClient(sender, boardPackets);
                sendToClient(responder, boardPackets);
                cout << "Game started between " << sender << " and " << responder << endl;
            }
            break;
        }
        
        case 'P': { // Game move
            if (fullData.size() < offset + 2) return;
            uint16_t slen;
            memcpy(&slen, fullData.c_str() + offset, 2);
            slen = ntohs(slen);
            offset += 2;
            
            if (fullData.size() < offset + slen) return;
            string sender(fullData.c_str() + offset, slen);
            offset += slen;
            
            if (fullData.size() < offset + 4) return;
            uint32_t position = ((unsigned char)fullData[offset] << 24) |
                               ((unsigned char)fullData[offset+1] << 16) |
                               ((unsigned char)fullData[offset+2] << 8) |
                               (unsigned char)fullData[offset+3];
            
            cout << sender << " made move at position: " << position << endl;
            
            // Procesar movimiento del juego (código existente)
            processGameMove(sender, position);
            break;
        }
        
        default:
            cout << "Unknown message type: " << messageType << endl;
            break;
    }
}

// Función para procesar mensajes simples del cliente
void processSimpleClientMessage(const string& client_nickname, const string& data, char messageType, 
                               const sockaddr_in& client_addr, socklen_t addr_len, int server_fd) {
    
    switch (messageType) {
        case 'l': { // List request
            cout << client_nickname << " requested client list" << endl;
            vector<string> packets = buildList();
            sendToClient(client_nickname, packets);
            break;
        }
        
        case 'x': { // Close connection
            cout << client_nickname << " disconnected" << endl;
            {
                lock_guard<mutex> lock(clients_mutex);
                clients.erase(client_nickname);
            }
            break;
        }
        
        case 'm': // Broadcast message
        case 't': // Private message  
        case 'f': // File transfer
        case 'o': // Object transfer
        case 'J': // Game request
        case 'j': // Game response
        case 'P': // Game move
            // Procesar estos mensajes usando processCompleteMessage
            processCompleteMessage(client_nickname, data.substr(1), messageType, client_addr, addr_len, server_fd);
            break;
        
        default:
            cout << "Unknown simple message type: " << messageType << endl;
            break;
    }
}

void processDatagram(int server_fd, char* buffer, int bytes_received, const sockaddr_in& client_addr, socklen_t addr_len, string client_nickname) {
    if (bytes_received < 1) return;

    string data(buffer, bytes_received);
    
    // Verificar si es un mensaje de nickname (siempre simple)
    if (client_nickname.empty() && data[0] == 'n') {
        processNicknameMessage(server_fd, data, client_addr, addr_len);
        return;
    }

    // Si no tenemos nickname, ignorar el mensaje
    if (client_nickname.empty()) {
        return;
    }

    // Determinar si es un mensaje simple o fragmentado
    char firstByte = data[0];
    
    // Si el primer byte es un carácter de mensaje válido del cliente
    // (n, m, t, l, x, f, o, J, j, P) entonces es un mensaje simple completo
    if ((firstByte >= 'a' && firstByte <= 'z') || firstByte == 'J' || firstByte == 'j' || firstByte == 'P') {
        // Es un mensaje simple completo del cliente
        processSimpleClientMessage(client_nickname, data, firstByte, client_addr, addr_len, server_fd);
        return;
    }

    // Si llega aquí, es un mensaje fragmentado
    string fragment = data;
    string fullData;
    char messageType = 0;
    string client_id = client_nickname + ":" + to_string(client_addr.sin_port);
    
    if (reconstructPacket(client_id, fragment, fullData, messageType)) {
        // Mensaje completo reconstruido
        cout << "DEBUG: Reconstructed complete message of type: " << messageType << ", size: " << fullData.size() << endl;
        processCompleteMessage(client_nickname, fullData, messageType, client_addr, addr_len, server_fd);
    } else {
        // Fragmento recibido pero aún no está completo
        cout << "DEBUG: Received fragment from " << client_nickname << ", type: " << messageType 
             << ", total fragments: " << reassemblyBuffers[client_id].fragments.size() << endl;
    }
}

// Build close connection message
vector<string> buildClose() {
    string packet = "X";
    vector<string> packets;
    packets.push_back(completeDatagram(packet));
    return packets;
}

// send a message to everyone except who is sending
void sendAll(const vector<string> packets, const string& sender_nickname) {
    lock_guard<mutex> lock(clients_mutex);
    for (auto client : clients) {
        if (client.first != sender_nickname) {
            for (const auto& packet : packets) {
                cout << "TO " << client.first << ": " << packet.substr(0, 100) << (packet.length() > 100 ? "..." : "") << endl;
                
                sendto(client.second.socket_fd,
                       packet.c_str(),
                       packet.size(),
                       0,
                       (struct sockaddr*)&client.second.address,
                       client.second.addr_len);
            }
        }
    }
}

// send a message to a specific client
void sendToClient(const string dest, const vector<string> packets) {
    lock_guard<mutex> lock(clients_mutex);
    if (clients.count(dest)) {
        for (const auto& packet : packets) {
            cout << "TO " << dest << ": " << packet.substr(0, 100) << (packet.length() > 100 ? "..." : "") << endl;
            
            sendto(clients[dest].socket_fd,
                   packet.c_str(),
                   packet.size(),
                   0,
                   (struct sockaddr*)&clients[dest].address,
                   clients[dest].addr_len);
        }
    }
}

// Build the error message with the protocol
vector<string> buildError(const string& msg) {
    string packet = "E";
    uint32_t len = msg.size();
    packet.push_back((len >> 16) & 0xFF);
    packet.push_back((len >> 8) & 0xFF);
    packet.push_back(len & 0xFF);
    packet += msg;
    
    if (packet.size() <= (size_t)maxDatagramLength) {
        vector<string> packets;
        packets.push_back(completeDatagram(packet));
        return packets;
    } else {
        return fragmentDatagram("E", msg, 3);
    }
}

// Build the message with the protocol
vector<string> buildBroadcast(const string& sender, const string& msg) {
    string packet = "M";
    uint16_t slen = htons(sender.size());
    packet.append((char*)&slen, 2);
    packet += sender;
    uint32_t mlen = msg.size();
    packet.push_back((mlen >> 16) & 0xFF);
    packet.push_back((mlen >> 8) & 0xFF);
    packet.push_back(mlen & 0xFF);
    packet += msg;
    
    if (packet.size() <= (size_t)maxDatagramLength) {
        vector<string> packets;
        packets.push_back(completeDatagram(packet));
        return packets;
    } else {
        return fragmentDatagram("M" + string((char*)&slen, 2) + sender, msg, 3);
    }
}

// Build the message to a specific client with the protocol
vector<string> buildToClient(const string& sender, const string& msg) {
    string packet = "T";
    uint16_t slen = htons(sender.size());
    packet.append((char*)&slen, 2);
    packet += sender;
    uint32_t mlen = msg.size();
    packet.push_back((mlen >> 16) & 0xFF);
    packet.push_back((mlen >> 8) & 0xFF);
    packet.push_back(mlen & 0xFF);
    packet += msg;
    
    if (packet.size() <= (size_t)maxDatagramLength) {
        vector<string> packets;
        packets.push_back(completeDatagram(packet));
        return packets;
    } else {
        return fragmentDatagram("T" + string((char*)&slen, 2) + sender, msg, 3);
    }
}

//Build list with the protocol
vector<string> buildList() {
    lock_guard<mutex> lock(clients_mutex);
    string all;
    for (auto client : clients) {
        uint16_t nick_len = htons(client.first.size());
        all.append((char*)&nick_len, 2);
        all += client.first;
    }
    
    string packet = "L";
    uint16_t total_len = htons(all.size());
    packet.append((char*)&total_len, 2);
    packet += all;
    
    packet = completeDatagram(packet);
    vector<string> packets;
    packets.push_back(packet);
    return packets;
}

// Function to build file message
vector<string> buildFile(const string& sender, const string& filename, const char* file_data, uint64_t file_size) {
    string packet = "F";
    
    uint16_t slen = htons(sender.size());
    packet.append((char*)&slen, 2);
    packet += sender;
    
    uint32_t flen = filename.size();
    packet.push_back((flen >> 16) & 0xFF);
    packet.push_back((flen >> 8) & 0xFF);
    packet.push_back(flen & 0xFF);
    
    packet += filename;
    
    for (int i = 9; i >= 0; i--) {
        packet.push_back((file_size >> (i * 8)) & 0xFF);
    }
    
    packet.append(file_data, file_size);
    
    if (packet.size() <= (size_t)maxDatagramLength) {
        vector<string> packets;
        packets.push_back(completeDatagram(packet));
        return packets;
    } else {
        string header = "F" + string((char*)&slen, 2) + sender;
        header.push_back((flen >> 16) & 0xFF);
        header.push_back((flen >> 8) & 0xFF);
        header.push_back(flen & 0xFF);
        header += filename;
        for (int i = 9; i >= 0; i--) {
            header.push_back((file_size >> (i * 8)) & 0xFF);
        }
        
        return fragmentDatagram(header, string(file_data, file_size), 0);
    }
}

// Function to build object message
vector<string> buildObject(const string& sender, const vector<char>& objectData) {
    string packet = "O";
    
    uint16_t slen = htons(sender.size());
    packet.append((char*)&slen, 2);
    packet += sender;
    
    uint32_t objSize = htonl(static_cast<uint32_t>(objectData.size()));
    packet.append(reinterpret_cast<const char*>(&objSize), sizeof(objSize));
    
    packet.append(objectData.data(), objectData.size());
    
    if (packet.size() <= (size_t)maxDatagramLength) {
        vector<string> packets;
        packets.push_back(completeDatagram(packet));
        return packets;
    } else {
        string header = "O" + string((char*)&slen, 2) + sender;
        header.append(reinterpret_cast<const char*>(&objSize), sizeof(objSize));
        
        return fragmentDatagram(header, string(objectData.data(), objectData.size()), 0);
    }
}

vector<string> buildGameRequest(const string& sender) {
    string packet = "J";
    uint16_t slen = htons(sender.size());
    packet.append((char*)&slen, 2);
    packet += sender;
    vector<string> packets;
    packets.push_back(completeDatagram(packet));
    return packets;
}

vector<string> buildGameResponse(const string& sender, bool accepted) {
    string packet = "j";
    uint16_t slen = htons(sender.size());
    packet.append((char*)&slen, 2);
    packet += sender;
    packet.push_back(accepted ? 'y' : 'n');
    vector<string> packets;
    packets.push_back(completeDatagram(packet));
    return packets;
}

vector<string> buildBoard(const vector<char>& board, const string& currentPlayer) {
    string packet = "B";
    uint16_t board_len = htons(board.size());
    packet.append((char*)&board_len, 2);
    packet.append(board.begin(), board.end());
    
    uint16_t player_len = htons(currentPlayer.size());
    packet.append((char*)&player_len, 2);
    packet += currentPlayer;
    
    if (packet.size() <= (size_t)maxDatagramLength) {
        vector<string> packets;
        packets.push_back(completeDatagram(packet));
        return packets;
    } else {
        string header = "B" + string((char*)&board_len, 2);
        string data = string(board.begin(), board.end()) + string((char*)&player_len, 2) + currentPlayer;
        
        return fragmentDatagram(header, data, 0);
    }
}

vector<string> buildGameResult(char result) {
    string packet = "W";
    packet.push_back(result);
    vector<string> packets;
    packets.push_back(completeDatagram(packet));
    return packets;
}

bool checkWinner(const vector<char>& board, char player) {
    for (int i = 0; i < 3; i++) {
        if (board[i*3] == player && board[i*3+1] == player && board[i*3+2] == player)
            return true;
    }
    for (int i = 0; i < 3; i++) {
        if (board[i] == player && board[i+3] == player && board[i+6] == player)
            return true;
    }
    if (board[0] == player && board[4] == player && board[8] == player) return true;
    if (board[2] == player && board[4] == player && board[6] == player) return true;
    
    return false;
}

bool checkIsPositionUsed(const vector<char>& board) {
    for (char c : board) {
        if (c == ' ') return false;
    }
    return true;
}

void initializeGame(Game& game, const string& p1, const string& p2) {
    game.player1 = p1;
    game.player2 = p2;
    game.board = vector<char>(9, ' ');
    game.currentPlayer = p1;
    game.gameActive = true;
}

void processGameMove(const string& player, uint32_t position) {
    lock_guard<mutex> lock(games_mutex);
    Game* currentGame = nullptr;
    string opponent;
    pair<string, string> gameKey;
    
    for (auto& game : activeGames) {
        if ((game.second.player1 == player || game.second.player2 == player) && 
            game.second.gameActive) {
            currentGame = &game.second;
            opponent = (game.second.player1 == player) ? game.second.player2 : game.second.player1;
            gameKey = game.first;
            break;
        }
    }
    
    if (!currentGame) {
        vector<string> err = buildError("No active game found");
        sendToClient(player, err);
        return;
    }
    
    if (currentGame->currentPlayer != player) {
        vector<string> err = buildError("Not your turn");
        sendToClient(player, err);
        return;
    }
    
    if (position >= 9) {
        vector<string> err = buildError("Invalid position");
        sendToClient(player, err);
        return;
    }
    
    if (currentGame->board[position] == ' ') {
        currentGame->board[position] = (player == currentGame->player1) ? 'X' : 'O';
        
        char currentSymbol = (player == currentGame->player1) ? 'X' : 'O';
        if (checkWinner(currentGame->board, currentSymbol)) {
            vector<string> result1 = buildGameResult('1');
            vector<string> result2 = buildGameResult('0');
            sendToClient(player, result1);
            sendToClient(opponent, result2);
            currentGame->gameActive = false;
            activeGames.erase(gameKey);
            cout << "Game finished. Winner: " << player << endl;
        } else if (checkIsPositionUsed(currentGame->board)) {
            vector<string> result = buildGameResult('2');
            sendToClient(player, result);
            sendToClient(opponent, result);
            currentGame->gameActive = false;
            activeGames.erase(gameKey);
            cout << "Game finished in draw between " << player << " and " << opponent << endl;
        } else {
            currentGame->currentPlayer = opponent;
            vector<string> boardPackets = buildBoard(currentGame->board, currentGame->currentPlayer);
            sendToClient(player, boardPackets);
            sendToClient(opponent, boardPackets);
            cout << "Turn switched to: " << currentGame->currentPlayer << endl;
        }
    } else {
        vector<string> err = buildError("Position already occupied");
        sendToClient(player, err);
    }
}

void handleClient(int server_fd) {
    char buffer[MAX_DATAGRAM_SIZE];
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);
    
    int bytes_received = recvfrom(server_fd, 
                                  buffer, 
                                  MAX_DATAGRAM_SIZE, 
                                  0, 
                                  (struct sockaddr*)&client_addr, 
                                  &addr_len);

    if (bytes_received <= 0) {
        return;
    }

    string nickname = "";
    {
        lock_guard<mutex> lock(clients_mutex);
        for (auto const& [nick, info] : clients) {
            if (info.address.sin_addr.s_addr == client_addr.sin_addr.s_addr &&
                info.address.sin_port == client_addr.sin_port) {
                nickname = nick;
                break;
            }
        }
    }

    processDatagram(server_fd, buffer, bytes_received, client_addr, addr_len, nickname);
}

int main() {
    int server_fd;
    struct sockaddr_in address;
    int opt = 1;

    server_fd = socket(AF_INET, SOCK_DGRAM, 0);
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    bind(server_fd, (struct sockaddr*)&address, sizeof(address));

    cout << "Server listening on port " << PORT << endl;

    while (true) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(server_fd, &read_fds);

        if (select(server_fd + 1, &read_fds, NULL, NULL, NULL) < 0) {
            perror("select error");
            break;
        }

        if (FD_ISSET(server_fd, &read_fds)) {
            handleClient(server_fd);
        }
    }

    return 0;
}
#include <iostream>
#include <string>
#include <thread>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <vector>
#include <fstream>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <unordered_map>
#include "sala.h"
#include "sala_serialized.h"
#include <algorithm>

using namespace std;

#define PORT 45000
#define MAX_DATAGRAM_SIZE 1024

/*
    n: Nickname (client → server)
    m: Broadcast message (client → server)
    t: Private message (client → server)
    l: List of clients (client → server)
    x: Close connection (client → server)
    f: Send files (client → server)
    E: Error (server → client)
    M: Broadcast message (server → client)
    T: Private message (server → client)
    L: List of clients (server → client)
    X: Close connection (server → client)
    F: Send files (server → client)

    Tic Tac Toe
    J: Game request (client → server)
    j: Game response (client → server)
    B: Board state (server → client)
    P: Position move (client → server)
    W: Game result (server → client)
*/

atomic<bool> waitingForGameInput(false);
atomic<bool> waitingForBoardInput(false);
string gameInviter;
mutex inputMutex;
condition_variable ready;
string userInput;
bool inputReady = false;

int maxDatagramLength = 777;

// Variables globales para socket y dirección del servidor
int sock;
struct sockaddr_in serv_addr;
string nickname; // Variable global para el nickname

// Función para completar datagramas
string completeDatagram(string &packet) {
    size_t len = packet.size();
    if (len < (size_t)maxDatagramLength) {
        packet.append(maxDatagramLength - len, '#');
    }
    return packet;
}

// Función para fragmentar datagramas grandes
vector<string> fragmentDatagram(const string &header, const string &data, int sizeFieldBytes) {
    vector<string> datagrams;

    size_t overhead = 1 + header.size() + sizeFieldBytes;
    size_t maxDataPerPacket = maxDatagramLength - overhead;
    size_t offset = 0;
    int numPacket = 1;

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

// Función para enviar datagramas (UDP)
void sendDatagram(int sock, const string& packet, const sockaddr_in& dest_addr) {
    // Log del protocolo
    cout << "SEND: " << packet.substr(0, 100) << (packet.length() > 100 ? "..." : "") << endl;
    
    sendto(sock, packet.c_str(), packet.size(), 0, 
           (struct sockaddr*)&dest_addr, sizeof(dest_addr));
}

// Función para enviar múltiples paquetes
void sendPackets(int sock, const vector<string>& packets, const sockaddr_in& dest_addr) {
    for (const auto& packet : packets) {
        sendDatagram(sock, packet, dest_addr);
    }
}

void sendNickname(int sock, const string nick, const sockaddr_in& serv_addr) {
    string packet = "n";
    uint16_t len = htons(nick.size());
    packet.append((char*)&len,2);
    packet += nick;
    packet = completeDatagram(packet);
    cout << "Sending nickname: " << nick << endl;
    sendDatagram(sock, packet, serv_addr);
}

void sendBroadcast(int sock, const string& sender_nick, const string msg, const sockaddr_in& serv_addr) {
    string packet = "m";
    
    // Agregar nickname del sender
    uint16_t slen = htons(sender_nick.size());
    packet.append((char*)&slen, 2);
    packet += sender_nick;
    
    // Agregar mensaje
    uint32_t mlen = msg.size();
    packet.push_back((mlen>>16)&0xFF);
    packet.push_back((mlen>>8)&0xFF);
    packet.push_back(mlen&0xFF);
    packet += msg;
    
    if (packet.size() <= (size_t)maxDatagramLength) {
        packet = completeDatagram(packet);
        cout << "Sending broadcast: " << msg << endl;
        sendDatagram(sock, packet, serv_addr);
    } else {
        string header = "m";
        header.append((char*)&slen, 2);
        header += sender_nick;
        
        vector<string> fragments = fragmentDatagram(header, msg, 3);
        cout << "Sending fragmented broadcast: " << msg << endl;
        sendPackets(sock, fragments, serv_addr);
    }
}

void sendToClient(int sock, const string& sender_nick, const string dest, const string msg, const sockaddr_in& serv_addr) {
    string packet = "t";
    
    // Agregar nickname del sender
    uint16_t slen = htons(sender_nick.size());
    packet.append((char*)&slen, 2);
    packet += sender_nick;
    
    // Agregar destino
    uint16_t dlen = htons(dest.size());
    packet.append((char*)&dlen, 2);
    packet += dest;
    
    // Agregar mensaje
    uint32_t mlen = msg.size();
    packet.push_back((mlen>>16)&0xFF);
    packet.push_back((mlen>>8)&0xFF);
    packet.push_back(mlen&0xFF);
    packet += msg;
    
    if (packet.size() <= (size_t)maxDatagramLength) {
        packet = completeDatagram(packet);
        cout << "Sending private to " << dest << ": " << msg << endl;
        sendDatagram(sock, packet, serv_addr);
    } else {
        string header = "t";
        header.append((char*)&slen, 2);
        header += sender_nick;
        header.append((char*)&dlen, 2);
        header += dest;
        
        vector<string> fragments = fragmentDatagram(header, msg, 3);
        cout << "Sending fragmented private to " << dest << ": " << msg << endl;
        sendPackets(sock, fragments, serv_addr);
    }
}

void requestList(int sock, const sockaddr_in& serv_addr) {
    string packet = "l";
    packet = completeDatagram(packet);
    cout << "Requesting client list" << endl;
    sendDatagram(sock, packet, serv_addr);
}

void sendClose(int sock, const sockaddr_in& serv_addr) {
    string packet = "x";
    packet = completeDatagram(packet);
    cout << "Sending close connection" << endl;
    sendDatagram(sock, packet, serv_addr);
}

// Parse list response
void parseListResponse(const string& listData) {
    size_t pos = 0;
    vector<string> clients;
    
    while (pos < listData.size()) {
        if (pos + 2 > listData.size()) break;
        
        uint16_t nick_len;
        memcpy(&nick_len, listData.c_str() + pos, 2);
        nick_len = ntohs(nick_len);
        pos += 2;
        
        if (pos + nick_len > listData.size()) break;
        
        string nick(listData.c_str() + pos, nick_len);
        clients.push_back(nick);
        pos += nick_len;
    }
    
    cout << "[Clients] ";
    for (size_t i = 0; i < clients.size(); i++) {
        if (i > 0) cout << ", ";
        cout << clients[i];
    }
    cout << endl;
}

void sendFile(int sock, const string& sender_nick, string dest, const string& filename, const sockaddr_in& serv_addr) {
    // Read file
    ifstream file(filename, ios::binary | ios::ate);
    if (!file.is_open()) {
        cout << "Error: Could not open file " << filename << endl;
        return;
    }
    
    // get the length
    streamsize file_size = file.tellg();
    file.seekg(0, ios::beg);
    
    // Read the content
    vector<char> file_data(file_size);
    if (!file.read(file_data.data(), file_size)) {
        cout << "Error: Could not read file" << endl;
        return;
    }
    file.close();
    
    string packet = "f";
    
    // Agregar nickname del sender
    uint16_t slen = htons(sender_nick.size());
    packet.append((char*)&slen, 2);
    packet += sender_nick;
    
    // destination
    uint16_t dlen = htons(dest.size());
    packet.append((char*)&dlen, 2);
    packet += dest;
    
    // filename
    uint32_t flen = filename.size();
    packet.push_back((flen >> 16) & 0xFF);
    packet.push_back((flen >> 8) & 0xFF);
    packet.push_back(flen & 0xFF);
    
    packet += filename;
    
    // file size
    uint64_t fsize = file_size;
    for (int i = 9; i >= 0; i--) {
        packet.push_back((fsize >> (i * 8)) & 0xFF);
    }
    
    // file data
    packet.append(file_data.data(), file_size);
    
    if (packet.size() <= (size_t)maxDatagramLength) {
        packet = completeDatagram(packet);
        cout << "Sending file to " << dest << ": " << filename << " (" << file_size << " bytes)" << endl;
        sendDatagram(sock, packet, serv_addr);
    } else {
        string header = "f";
        header.append((char*)&slen, 2);
        header += sender_nick;
        header.append((char*)&dlen, 2);
        header += dest;
        header.push_back((flen >> 16) & 0xFF);
        header.push_back((flen >> 8) & 0xFF);
        header.push_back(flen & 0xFF);
        header += filename;
        for (int i = 9; i >= 0; i--) {
            header.push_back((fsize >> (i * 8)) & 0xFF);
        }
        
        vector<string> fragments = fragmentDatagram(header, string(file_data.data(), file_size), 0);
        cout << "Sending fragmented file to " << dest << ": " << filename << " (" << file_size << " bytes)" << endl;
        sendPackets(sock, fragments, serv_addr);
    }
}

void sendObject(int sock, const string& sender_nick, const string &dest, const Sala &sala, const sockaddr_in& serv_addr) {
    string packet;

    packet.push_back('o');

    // Agregar nickname del sender
    uint16_t slen = htons(sender_nick.size());
    packet.append(reinterpret_cast<char*>(&slen), sizeof(slen));
    packet += sender_nick;

    uint16_t dlen = htons(static_cast<uint16_t>(dest.size()));
    packet.append(reinterpret_cast<char*>(&dlen), sizeof(dlen));
    packet += dest;

    vector<char> objectContent = serializarSala(sala);

    // 4 bytes for object size
    uint32_t objSize = htonl(static_cast<uint32_t>(objectContent.size()));
    packet.append(reinterpret_cast<char*>(&objSize), sizeof(objSize));

    // object content
    packet.insert(packet.end(), objectContent.begin(), objectContent.end());

    if (packet.size() <= (size_t)maxDatagramLength) {
        packet = completeDatagram(packet);
        cout << "Sending object to " << dest << " (" << objectContent.size() << " bytes)" << endl;
        sendDatagram(sock, packet, serv_addr);
    } else {
        string header = "o";
        header.append(reinterpret_cast<const char*>(&slen), sizeof(slen));
        header += sender_nick;
        header.append(reinterpret_cast<const char*>(&dlen), sizeof(dlen));
        header += dest;
        header.append(reinterpret_cast<const char*>(&objSize), sizeof(objSize));
        
        vector<string> fragments = fragmentDatagram(header, string(objectContent.data(), objectContent.size()), 0);
        cout << "Sending fragmented object to " << dest << " (" << objectContent.size() << " bytes)" << endl;
        sendPackets(sock, fragments, serv_addr);
    }
}

void sendGameRequest(int sock, const string& sender_nick, const string& dest, const sockaddr_in& serv_addr) {
    string packet = "J";
    
    // Agregar nickname del sender
    uint16_t slen = htons(sender_nick.size());
    packet.append((char*)&slen, 2);
    packet += sender_nick;
    
    uint16_t dlen = htons(dest.size());
    packet.append((char*)&dlen, 2);
    packet += dest;
    packet = completeDatagram(packet);
    cout << "Sending game request to " << dest << endl;
    sendDatagram(sock, packet, serv_addr);
}

void sendGameResponse(int sock, const string& sender_nick, const string& sender, bool accept, const sockaddr_in& serv_addr) {
    string packet = "j";
    
    // Agregar nickname del sender
    uint16_t slen = htons(sender_nick.size());
    packet.append((char*)&slen, 2);
    packet += sender_nick;
    
    uint16_t rlen = htons(sender.size());
    packet.append((char*)&rlen, 2);
    packet += sender;
    packet.push_back(accept ? 'y' : 'n');
    packet = completeDatagram(packet);
    cout << "Sending game response to " << sender << ": " << (accept ? "accept" : "decline") << endl;
    sendDatagram(sock, packet, serv_addr);
}

void sendBoardPosition(int sock, const string& sender_nick, int position, const sockaddr_in& serv_addr) {
    string packet = "P";
    
    // Agregar nickname del sender
    uint16_t slen = htons(sender_nick.size());
    packet.append((char*)&slen, 2);
    packet += sender_nick;
    
    uint32_t pos = position;
    packet.push_back((pos >> 24) & 0xFF);
    packet.push_back((pos >> 16) & 0xFF);
    packet.push_back((pos >> 8) & 0xFF);
    packet.push_back(pos & 0xFF);
    packet = completeDatagram(packet);
    cout << "Sending board position: " << position << endl;
    sendDatagram(sock, packet, serv_addr);
}

void printBoard(const vector<char>& board, const string& currentPlayer, const string& myNickname) {
    cout << "_____________" << endl;
    cout << "|   |   |   |" << endl;
    for (int i = 0; i < 3; i++) {
        cout << "|";
        for (int j = 0; j < 3; j++) {
            char c = board[i * 3 + j];
            if (c == ' ') cout << " " << i * 3 + j << " ";
            else cout << " " << c << " ";
            cout << "│";
        }
        cout << endl;
        if (i < 2) cout << "|---|---|---|" << endl;
    }
    cout << "|___|___|___|" << endl;
    
    if (currentPlayer == myNickname) {
        cout << ">>> It's YOUR turn! <<<" << endl;
    } else {
        cout << ">>> Waiting for " << currentPlayer << "'s move... <<<" << endl;
    }
}

// Function to get user input
string getGameInput(const string& prompt) {
    cout << prompt;
    
    {
        lock_guard<mutex> lock(inputMutex);
        inputReady = false;
    }
    
    waitingForGameInput = true;
    
    // Wait for input from main thread
    unique_lock<mutex> lock(inputMutex);
    ready.wait(lock, []{ return inputReady; });
    
    waitingForGameInput = false;
    return userInput;
}

// Function to get board position input
string getBoardInput(const string& prompt) {
    cout << prompt;
    
    {
        lock_guard<mutex> lock(inputMutex);
        inputReady = false;
    }
    
    waitingForBoardInput = true;
    
    // Wait for input from main thread
    unique_lock<mutex> lock(inputMutex);
    ready.wait(lock, []{ return inputReady; });
    
    waitingForBoardInput = false;
    return userInput;
}

// Estructura para reconstrucción de paquetes fragmentados
struct FragmentReassembly {
    vector<string> fragments;
    string fullData;
    char messageType;
    time_t lastFragmentTime;
};

unordered_map<string, FragmentReassembly> reassemblyBuffers;
mutex reassembly_mutex;

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

// Función para procesar mensajes completos
void processCompleteMessage(const string& fullData, char messageType, const string& nickname) {
    size_t offset = 0;
    
    switch (messageType) {
        case 'E': { // Error
            if (fullData.size() < offset + 3) return;
            uint32_t len = ((unsigned char)fullData[offset] << 16) |
                          ((unsigned char)fullData[offset+1] << 8) |
                          (unsigned char)fullData[offset+2];
            offset += 3;
            
            if (fullData.size() < offset + len) return;
            string errorMsg(fullData.c_str() + offset, len);
            
            cout << "[Error] " << errorMsg << endl;
            break;
        }
        
        case 'M': { // Broadcast message
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
            
            cout << "[Broadcast from " << sender << "] " << message << endl;
            break;
        }
        
        case 'T': { // Private message
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
            
            cout << "[Private from " << sender << "] " << message << endl;
            break;
        }
        
        case 'L': { // List of clients
            if (fullData.size() < offset + 2) return;
            uint16_t total_len;
            memcpy(&total_len, fullData.c_str() + offset, 2);
            total_len = ntohs(total_len);
            offset += 2;
            
            if (fullData.size() < offset + total_len) return;
            string listData(fullData.c_str() + offset, total_len);
            
            parseListResponse(listData);
            break;
        }
        
        case 'X': { // Close connection
            cout << "Server closed the connection. Goodbye!" << endl;
            break;
        }
        
        case 'F': { // File transfer
            if (fullData.size() < offset + 2) return;
            uint16_t slen;
            memcpy(&slen, fullData.c_str() + offset, 2);
            slen = ntohs(slen);
            offset += 2;
            
            if (fullData.size() < offset + slen) return;
            string sender(fullData.c_str() + offset, slen);
            offset += slen;
            
            if (fullData.size() < offset + 3) return;
            uint32_t flen = ((unsigned char)fullData[offset] << 16) |
                           ((unsigned char)fullData[offset+1] << 8) |
                           (unsigned char)fullData[offset+2];
            offset += 3;
            
            if (fullData.size() < offset + flen) return;
            string filename(fullData.c_str() + offset, flen);
            offset += flen;
            
            if (fullData.size() < offset + 10) return;
            uint64_t fsize = 0;
            for (int i = 0; i < 10; i++) {
                fsize = (fsize << 8) | (unsigned char)fullData[offset++];
            }
            
            if (fullData.size() < offset + fsize) return;
            vector<char> file_data(fullData.begin() + offset, fullData.begin() + offset + fsize);
            
            // Save file
            size_t dot_pos = filename.find_last_of(".");
            string new_filename;
            if (dot_pos != string::npos) {
                new_filename = filename.substr(0, dot_pos) + "_dest" + filename.substr(dot_pos);
            } else {
                new_filename = filename + "_dest";
            }
            
            ofstream out_file(new_filename, ios::binary);
            if (out_file.is_open()) {
                out_file.write(file_data.data(), fsize);
                out_file.close();
                cout << "[File received from " << sender << "] Saved as: " << new_filename 
                    << " (" << fsize << " bytes)" << endl;
            } else {
                cout << "[Error] Could not save file: " << new_filename << endl;
            }
            break;
        }
        
        case 'O': { // Object transfer
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
            
            cout << "DEBUG: Object data received, deserializing..." << endl;
            
            try {
                Sala sala = deserializeSala(objectData);
                
                cout << "Sala object received from: " << sender << endl;
                cout << "Chair: " << sala.silla.patas << " legs, " 
                    << (sala.silla.conRespaldo ? "with backrest" : "without backrest") << endl;
                cout << "Sofa: capacity " << sala.sillon.capacidad << ", color " << sala.sillon.color << endl;
                cout << "Kitchen: " << (sala.cocina->electrica ? "electric" : "non-electric") 
                    << ", " << sala.cocina->metrosCuadrados << " m²" << endl;
                cout << "n: " << sala.n << endl;
                cout << "Description: " << sala.descripcion << endl;

                delete sala.cocina;
            } catch (const exception& e) {
                cout << "ERROR deserializing object: " << e.what() << endl;
            }
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
            
            // Get game response from user
            string response = getGameInput(sender + " is inviting you to play Tic Tac Toe\nDo you accept? (y/n): ");
            bool accept = (response == "y" || response == "Y" || response == "s" || response == "S");
            sendGameResponse(sock, nickname, sender, accept, serv_addr);
            
            if (accept) {
                cout << "Starting game with " << sender << "..." << endl;
            } else {
                cout << "Invitation declined." << endl;
            }
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
            
            if (fullData.size() < offset + 1) return;
            char response = fullData[offset];
            
            if (response == 'y') {
                cout << sender << " accepted your game invitation!" << endl;
            } else {
                cout << sender << " declined your game invitation." << endl;
            }
            break;
        }
        
        case 'B': { // Board state
            if (fullData.size() < offset + 2) return;
            uint16_t board_len;
            memcpy(&board_len, fullData.c_str() + offset, 2);
            board_len = ntohs(board_len);
            offset += 2;
            
            if (fullData.size() < offset + board_len) return;
            vector<char> board(fullData.begin() + offset, fullData.begin() + offset + board_len);
            offset += board_len;
            
            if (fullData.size() < offset + 2) return;
            uint16_t player_len;
            memcpy(&player_len, fullData.c_str() + offset, 2);
            player_len = ntohs(player_len);
            offset += 2;
            
            if (fullData.size() < offset + player_len) return;
            string currentPlayer(fullData.c_str() + offset, player_len);
            
            cout << "Current board:" << endl;
            printBoard(board, currentPlayer, nickname);

            if (currentPlayer == nickname) {
                string move = getBoardInput("Select a position (0-8): ");
                
                try {
                    int position = stoi(move);
                    if (position >= 0 && position <= 8) {
                        sendBoardPosition(sock, nickname, position, serv_addr);
                    } else {
                        cout << "Invalid position. Must be between 0 and 8." << endl;
                    }
                } catch (...) {
                    cout << "Invalid input." << endl;
                }
            } else {
                cout << "Please wait for " << currentPlayer << " to make a move..." << endl;
            }
            break;
        }
        
        case 'W': { // Game result
            if (fullData.size() < offset + 1) return;
            char result = fullData[offset];
            
            if (result == '1') {
                cout << "You win!" << endl;
            } else if (result == '0') {
                cout << "You lose!" << endl;
            } else if (result == '2') {
                cout << "It's a tie!" << endl;
            } else if (result == '3') {
                cout << "Game ended: opponent disconnected" << endl;
            }
            break;
        }
        
        default:
            cout << "Unknown message type: " << messageType << endl;
            break;
    }
}

// Receiver thread
void receiveMessages(int sock, const string& nickname, const sockaddr_in& serv_addr) {
    char buffer[MAX_DATAGRAM_SIZE];
    struct sockaddr_in from_addr;
    socklen_t from_len = sizeof(from_addr);
    
    while (true) {
        int bytes_received = recvfrom(sock, buffer, MAX_DATAGRAM_SIZE, 0, 
                                     (struct sockaddr*)&from_addr, &from_len);
        
        if (bytes_received <= 0) {
            cout << "Disconnected." << endl;
            break;
        }

        string data(buffer, bytes_received);
        
        // Log del protocolo recibido
        cout << "RECV: " << data.substr(0, 100) << (data.length() > 100 ? "..." : "") << endl;

        // Determinar si es un mensaje simple o fragmentado
        char firstByte = data[0];
        
        // Si el primer byte es un carácter de mensaje válido del servidor
        // (E, M, T, L, X, F, O, J, j, B, W) entonces es un mensaje simple completo
        if ((firstByte >= 'A' && firstByte <= 'Z') || firstByte == 'j') {
            // Es un mensaje simple completo del servidor
            processCompleteMessage(data.substr(1), firstByte, nickname);
            continue;
        }

        // Si llega aquí, es un mensaje fragmentado o con formato incorrecto
        string fragment = data;
        string fullData;
        char messageType = 0;
        string client_id = nickname + "_client";
        
        if (reconstructPacket(client_id, fragment, fullData, messageType)) {
            // Mensaje completo reconstruido
            processCompleteMessage(fullData, messageType, nickname);
        } else {
            // Fragmento recibido pero aún no está completo
            cout << "Received fragment, type: " << messageType << endl;
        }
    }
}

int main() {
    // Cambiar a socket UDP
    sock = socket(AF_INET, SOCK_DGRAM, 0);
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(PORT);
    inet_pton(AF_INET,"127.0.0.1",&serv_addr.sin_addr);

    // En UDP no hay connect(), pero guardamos la dirección del servidor
    cout << "UDP Client started" << endl;

    cout << "Enter nickname: ";
    getline(cin,nickname);
    sendNickname(sock, nickname, serv_addr);

    thread t(receiveMessages, sock, nickname, serv_addr);

    cout << "Commands:" << endl
     << "  /all msg   -> broadcast message" << endl
     << "  /to user msg -> private message" << endl
     << "  /list      -> show users" << endl
     << "  /exit      -> quit" << endl
     << "  /file dest file -> send files" << endl
     << "  /object dest -> send Sala object" << endl
     << "  /play dest -> invite to play tic tac toe" << endl;

    string line;
    while (getline(cin,line)) {
        // Check if we're waiting for game input
        if (waitingForGameInput || waitingForBoardInput) {
            {
                lock_guard<mutex> lock(inputMutex);
                userInput = line;
                inputReady = true;
            }
            ready.notify_one();
            continue;
        }
        
        if (line == "/exit") {
            sendClose(sock, serv_addr);
            break;
        }
        else if (line.rfind("/all ", 0) == 0 && line.length() > 5) {
            sendBroadcast(sock, nickname, line.substr(5), serv_addr);
        }
        else if (line.rfind("/to ", 0) == 0) {
            size_t sp = line.find(' ', 4);
            if (sp != string::npos && sp + 1 < line.length()) {
                string dest = line.substr(4, sp - 4);
                string msg = line.substr(sp + 1);
                sendToClient(sock, nickname, dest, msg, serv_addr);
            } else {
                cout << "Usage: /to username message" << endl;
            }
        }
        else if (line == "/list") {
            requestList(sock, serv_addr);
        }
        else if (line.rfind("/file ", 0) == 0) {
            size_t sp = line.find(' ', 6);
            if (sp != string::npos && sp + 1 < line.length()) {
                string dest = line.substr(6, sp - 6);
                string filename = line.substr(sp + 1);
                sendFile(sock, nickname, dest, filename, serv_addr);
            } else {
                cout << "Usage: /file destination file_path" << endl;
            }
        }
        else if (line.rfind("/object ", 0) == 0) {
            size_t sp = line.find(' ', 8);
            if (sp != string::npos && sp + 1 < line.length()) {
                string dest = line.substr(8, sp - 8);
                
                // Create a sample Sala object
                Sala sala;
                sala.silla.patas = 4;
                sala.silla.conRespaldo = true;
                sala.sillon.capacidad = 3;
                strcpy(sala.sillon.color, "red");
                sala.cocina = new Cocina();
                sala.cocina->electrica = true;
                sala.cocina->metrosCuadrados = 10.5f;
                sala.n = 42;
                strcpy(sala.descripcion, "This is a sample room for testing");
                
                sendObject(sock, nickname, dest, sala, serv_addr);
                
                // Clean up memory
                delete sala.cocina;
            } else {
                cout << "Usage: /object destination" << endl;
            }
        }
        else if (line.rfind("/play ", 0) == 0) {
            if (line.length() > 6) {
                string dest = line.substr(6);
                sendGameRequest(sock, nickname, dest, serv_addr);
            } else {
                cout << "Usage: /play username" << endl;
            }
        }
        else {
            cout << "Unknown command. Available: /all, /to, /list, /exit, /file, /object, /play" << endl;
        }
    }

    close(sock);
    if (t.joinable()) {
        t.join();
    }
    return 0;
}
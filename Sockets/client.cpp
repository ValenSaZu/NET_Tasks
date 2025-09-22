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
#include "sala.h"
#include "sala_serialized.h"


using namespace std;

#define PORT 45000

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
*/

// Helper function to print protocol data in hex
string formatProtocol(const string& data) {
    stringstream ss;
    for (char c : data) {
        if (isprint(c) && c != ' ') {
            ss << c;
        } else {
            ss << "\\x" << hex << setw(2) << setfill('0') << (int)(unsigned char)c;
        }
    }
    return ss.str();
}

void sendNickname(int sock, const string nick) {
    string packet = "n";
    uint16_t len = htons(nick.size());
    packet.append((char*)&len,2);
    packet += nick;
    cout << "Protocol sending: " << formatProtocol(packet) << endl;
    send(sock, packet.c_str(), packet.size(), 0);
}

void sendBroadcast(int sock, const string msg) {
    string packet = "m";
    uint32_t len = msg.size();
    packet.push_back((len>>16)&0xFF);
    packet.push_back((len>>8)&0xFF);
    packet.push_back(len&0xFF);
    packet += msg;
    cout << "Protocol sending: " << formatProtocol(packet) << endl;
    send(sock, packet.c_str(), packet.size(), 0);
}

void sendToClient(int sock, const string dest, const string msg) {
    string packet = "t";
    uint16_t dlen = htons(dest.size());
    packet.append((char*)&dlen,2);
    packet += dest;
    uint32_t mlen = msg.size();
    packet.push_back((mlen>>16)&0xFF);
    packet.push_back((mlen>>8)&0xFF);
    packet.push_back(mlen&0xFF);
    packet += msg;
    cout << "Protocol sending: " << formatProtocol(packet) << endl;
    send(sock, packet.c_str(), packet.size(), 0);
}

void requestList(int sock) {
    string packet = "l";
    cout << "Protocol sending: " << formatProtocol(packet) << endl;
    send(sock, packet.c_str(), 1, 0);
}

void sendClose(int sock) {
    string packet = "x";
    cout << "Protocol sending: " << formatProtocol(packet) << endl;
    send(sock, packet.c_str(), 1, 0);
}

// Parse list response
void parseListResponse(char* buf, int len) {
    int pos = 0;
    vector<string> clients;
    
    while (pos < len) {
        if (pos + 2 > len) break;
        
        uint16_t nick_len;
        memcpy(&nick_len, buf + pos, 2);
        nick_len = ntohs(nick_len);
        pos += 2;
        
        if (pos + nick_len > len) break;
        
        string nick(buf + pos, nick_len);
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

void sendFile(int sock, string dest, const string& filename) {
    // Read file
    ifstream file(filename, ios::binary | ios::ate);
    if (!file.is_open()) {
        cout << "Error: No se pudo abrir el archivo " << filename << endl;
        return;
    }
    
    // get the length
    streamsize file_size = file.tellg();
    file.seekg(0, ios::beg);
    
    // Read the content
    vector<char> file_data(file_size);
    if (!file.read(file_data.data(), file_size)) {
        cout << "Error: No se pudo leer el archivo" << endl;
        return;
    }
    file.close();
    
    string packet = "f";
    
    // nickname
    uint16_t dlen = htons(dest.size());
    packet.append((char*)&dlen, 2);
    packet += dest;
    
    // filename
    uint32_t flen = filename.size();
    packet.push_back((flen >> 16) & 0xFF);
    packet.push_back((flen >> 8) & 0xFF);
    packet.push_back(flen & 0xFF);
    
    packet += filename;
    
    // file
    uint64_t fsize = file_size;

    for (int i = 9; i >= 0; i--) {
        packet.push_back((fsize >> (i * 8)) & 0xFF);
    }
    
    packet.append(file_data.data(), file_size);
    
    cout << "Protocol sending: " << formatProtocol(packet.substr(0, 50)) << "..." << endl;
    send(sock, packet.c_str(), packet.size(), 0);
}

void sendObject(int sock, const string &dest, const Sala &sala) {
    string packet;

    packet.push_back('o');

    uint16_t dlen = htons(static_cast<uint16_t>(dest.size()));
    packet.append(reinterpret_cast<char*>(&dlen), sizeof(dlen));
    packet += dest;

    vector<char> objectContent = serializarSala(sala);

    // 4 bytes
    uint32_t objSize = htonl(static_cast<uint32_t>(objectContent.size()));
    packet.append(reinterpret_cast<char*>(&objSize), sizeof(objSize));

    // object content
    packet.insert(packet.end(), objectContent.begin(), objectContent.end());

    send(sock, packet.data(), packet.size(), 0);
}


// Receiver thread
void receiveMessages(int sock) {
    char header[4];
    while (true) {
        int r = recv(sock, header, 1, 0);
        if (r<=0) { cout << "Disconnected." << endl; break; }
        char type = header[0];

        if (type=='E') {
            recv(sock, header, 3, 0);
            int len = ((unsigned char)header[0] << 16) |
                    ((unsigned char)header[1] << 8) |
                    (unsigned char)header[2];
            char* buf = new char[len+1];
            recv(sock, buf, len, 0);
            buf[len]='\0';
            
            string errorPacket = "E";
            errorPacket += string(header, 3);
            errorPacket += string(buf, len);
            cout << "Protocol received: " << formatProtocol(errorPacket) << endl;
            
            cout << "[Error] " << buf << endl;
            delete[] buf;
            break;
        }
        else if (type=='M') {
            // read sender
            recv(sock, header, 2, 0);
            uint16_t slen; memcpy(&slen, header, 2); slen = ntohs(slen);
            char* sbuf = new char[slen+1];
            recv(sock, sbuf, slen, 0); sbuf[slen] = '\0';
            string sender(sbuf);
            delete[] sbuf;

            // read message
            recv(sock, header, 3, 0);
            int mlen = ((unsigned char)header[0]<<16) |
                    ((unsigned char)header[1]<<8)  |
                    (unsigned char)header[2];
            char* mbuf = new char[mlen+1];
            recv(sock, mbuf, mlen, 0); mbuf[mlen] = '\0';

            string broadcastPacket = "M";
            uint16_t slen_net = htons(slen);
            broadcastPacket.append((char*)&slen_net, 2);
            broadcastPacket += sender;
            broadcastPacket += string(header, 3);
            broadcastPacket += string(mbuf, mlen);
            cout << "Protocol received: " << formatProtocol(broadcastPacket) << endl;

            cout << "[Broadcast from " << sender << "] " << mbuf << endl;
            delete[] mbuf;
        }
        else if (type=='T') {
            recv(sock, header,2,0);
            uint16_t slen; memcpy(&slen,header,2); slen=ntohs(slen);
            char* sbuf=new char[slen+1];
            recv(sock,sbuf,slen,0); sbuf[slen]='\0';
            string sender(sbuf);
            delete[] sbuf;

            recv(sock,header,3,0);
            int mlen = ((unsigned char)header[0]<<16)|
                       ((unsigned char)header[1]<<8)|
                       (unsigned char)header[2];
            char* mbuf=new char[mlen+1];
            recv(sock,mbuf,mlen,0); mbuf[mlen]='\0';
            
            string privatePacket = "T";
            uint16_t slen_net = htons(slen);
            privatePacket.append((char*)&slen_net, 2);
            privatePacket += sender;
            privatePacket += string(header, 3);
            privatePacket += string(mbuf, mlen);
            cout << "Protocol received: " << formatProtocol(privatePacket) << endl;
            
            cout << "[Private from " << sender << "] " << mbuf << endl;
            delete[] mbuf;
        }
        else if (type=='L') {
            recv(sock, header,2,0);
            uint16_t total_len; memcpy(&total_len,header,2); total_len=ntohs(total_len);
            char* buf=new char[total_len+1];
            recv(sock,buf,total_len,0); buf[total_len]='\0';
            
            string listPacket = "L";
            uint16_t total_len_net = htons(total_len);
            listPacket.append((char*)&total_len_net, 2);
            listPacket += string(buf, total_len);
            cout << "Protocol received: " << formatProtocol(listPacket) << endl;
            
            parseListResponse(buf, total_len);
            delete[] buf;
        }
        else if (type=='X') {
            cout << "Protocol received: X" << endl;
            cout << "Server closed the connection. Goodbye!" << endl;
            break;
        }
        else if (type=='F') {
            recv(sock, header, 2, 0);
            uint16_t slen; memcpy(&slen, header, 2); slen = ntohs(slen);
            char* sbuf = new char[slen+1];
            recv(sock, sbuf, slen, 0); sbuf[slen] = '\0';
            string sender(sbuf);
            delete[] sbuf;

            recv(sock, header, 3, 0);
            uint32_t flen = ((unsigned char)header[0] << 16) |
                        ((unsigned char)header[1] << 8) |
                        (unsigned char)header[2];

            char* fbuf = new char[flen+1];
            int bytes_received = 0;
            while (bytes_received < flen) {
                int r = recv(sock, fbuf + bytes_received, flen - bytes_received, 0);
                if (r <= 0) break;
                bytes_received += r;
            }
            fbuf[flen] = '\0';
            string filename(fbuf);
            delete[] fbuf;

            char size_buf[10];
            bytes_received = 0;
            while (bytes_received < 10) {
                int r = recv(sock, size_buf + bytes_received, 10 - bytes_received, 0);
                if (r <= 0) break;
                bytes_received += r;
            }
            
            uint64_t fsize = 0;
            for (int i = 0; i < 10; i++) {
                fsize = (fsize << 8) | (unsigned char)size_buf[i];
            }

            char* file_data = new char[fsize];
            bytes_received = 0;
            while (bytes_received < fsize) {
                int r = recv(sock, file_data + bytes_received, fsize - bytes_received, 0);
                if (r <= 0) break;
                bytes_received += r;
            }
            
            size_t dot_pos = filename.find_last_of(".");
            string new_filename;
            if (dot_pos != string::npos) {
                new_filename = filename.substr(0, dot_pos) + "_dest" + filename.substr(dot_pos);
            } else {
                new_filename = filename + "_dest";
            }
            
            ofstream out_file(new_filename, ios::binary);
            if (out_file.is_open()) {
                out_file.write(file_data, fsize);
                out_file.close();
                cout << "[Archivo recibido de " << sender << "] Guardado como: " << new_filename 
                    << " (" << fsize << " bytes)" << endl;
            } else {
                cout << "[Error] No se pudo guardar el archivo: " << new_filename << endl;
            }
            
            delete[] file_data;
        }
        else if (type == 'O') {
            char header[2];
            recv(sock, header, 2, 0);

            uint16_t slen;
            memcpy(&slen, header, 2);
            slen = ntohs(slen);

            char* sbuf = new char[slen + 1];
            recv(sock, sbuf, slen, 0);
            sbuf[slen] = '\0';
            string sender(sbuf);
            delete[] sbuf;

            // longitud del objeto
            char sizeBuf[4];
            recv(sock, sizeBuf, 4, 0);

            uint32_t objSize;
            memcpy(&objSize, sizeBuf, 4);
            objSize = ntohl(objSize);

            // contenido
            vector<char> objectBuf(objSize);
            recv(sock, objectBuf.data(), objSize, 0);

            Sala sala = deserializeSala(objectBuf);

            cout << "Objeto Sala recibido de: " << sender << endl;
            cout << "Silla: " << sala.silla.patas << " patas, " 
                << (sala.silla.conRespaldo ? "con respaldo" : "sin respaldo") << endl;
            cout << "Sillón: capacidad " << sala.sillon.capacidad << ", color " << sala.sillon.color << endl;
            cout << "Cocina: " << (sala.cocina->electrica ? "eléctrica" : "no eléctrica") 
                << ", " << sala.cocina->metrosCuadrados << " m²" << endl;
            cout << "n: " << sala.n << endl;
            cout << "Descripción: " << sala.descripcion << endl;

            delete sala.cocina;
        }
    }
}

int main() {
    int sock;
    struct sockaddr_in serv_addr;

    sock = socket(AF_INET, SOCK_STREAM, 0);
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(PORT);
    inet_pton(AF_INET,"127.0.0.1",&serv_addr.sin_addr);

    if (connect(sock,(struct sockaddr*)&serv_addr,sizeof(serv_addr))<0) {
        cout << "Connection failed" << endl; return 0;
    }

    string nickname;
    cout << "Enter nickname: ";
    getline(cin,nickname);
    sendNickname(sock,nickname);

    thread t(receiveMessages,sock);

    cout << "Commands:" << endl
     << "  /all msg   -> broadcast message" << endl
     << "  /to user msg -> private message" << endl
     << "  /list      -> show users" << endl
     << "  /exit      -> quit" << endl
     << "  /file dest file -> send files" << endl
     << "  /object dest -> send Sala object" << endl;

    string line;
    while (getline(cin,line)) {
        if (line == "/exit") {
            sendClose(sock);
            break;
        }
        else if (line.rfind("/all ", 0) == 0 && line.length() > 5) {
            sendBroadcast(sock, line.substr(5));
        }
        else if (line.rfind("/to ", 0) == 0) {
            size_t sp = line.find(' ', 4);
            if (sp != string::npos && sp + 1 < line.length()) {
                string dest = line.substr(4, sp - 4);
                string msg = line.substr(sp + 1);
                sendToClient(sock, dest, msg);
            } else {
                cout << "Usage: /to username message" << endl;
            }
        }
        else if (line == "/list") {
            requestList(sock);
        }
        else if (line.rfind("/file ", 0) == 0) {
            size_t sp = line.find(' ', 6);
            if (sp != string::npos && sp + 1 < line.length()) {
                string dest = line.substr(6, sp - 6);
                string filename = line.substr(sp + 1);
                sendFile(sock, dest, filename);
            } else {
                cout << "Usage: /file destinatario ruta_del_archivo" << endl;
            }
        }
        else if (line.rfind("/object ", 0) == 0) {
            size_t sp = line.find(' ', 8);
            if (sp != string::npos && sp + 1 < line.length()) {
                string dest = line.substr(8, sp - 8);
                
                // Crear un objeto Sala de ejemplo
                Sala sala;
                sala.silla.patas = 4;
                sala.silla.conRespaldo = true;
                sala.sillon.capacidad = 3;
                strcpy(sala.sillon.color, "rojo");
                sala.cocina = new Cocina();
                sala.cocina->electrica = true;
                sala.cocina->metrosCuadrados = 10.5f;
                sala.n = 42;
                strcpy(sala.descripcion, "Esta es una sala de ejemplo para prueba");
                
                sendObject(sock, dest, sala);
                
                // Limpiar memoria
                delete sala.cocina;
            } else {
                cout << "Usage: /object destinatario" << endl;
            }
        }
        else {
            cout << "Unknown command. Available: /all, /to, /list, /exit, /file" << endl;
        }
    }

    close(sock);
    if (t.joinable()) {
        t.join();
    }
    return 0;
}
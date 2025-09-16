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

using namespace std;

#define PORT 45000

map<string,int> clients;
mutex clients_mutex;

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

// Build close connection message
string buildClose() {
    return "X"; // Single byte message
}

// send a message to everyone except who is sending
void sendAll(const string data, int sender_client = -1) {
    lock_guard<mutex> lock(clients_mutex);
    for (auto client : clients) {
        if (client.second != sender_client) {
            cout << "Server sending to " << client.first << ": " << formatProtocol(data) << endl;
            send(client.second, data.c_str(), data.size(), 0);
        }
    }
}

// send a message to a specific client
void sendToClient(const string dest, const string data) {
    lock_guard<mutex> lock(clients_mutex);
    if (clients.count(dest)) {
        cout << "Server sending to " << dest << ": " << formatProtocol(data) << endl;
        send(clients[dest], data.c_str(), data.size(), 0);
    }
}

// Build the error message with the protocol
string buildError(const string& msg) {
    string packet = "E";
    uint32_t len = msg.size();
    packet.push_back((len >> 16) & 0xFF);
    packet.push_back((len >> 8) & 0xFF);
    packet.push_back(len & 0xFF);
    packet += msg;
    return packet;
}

// Build the message with the protocol
string buildBroadcast(const string& sender, const string& msg) {
    string packet = "M";
    uint16_t slen = htons(sender.size());
    packet.append((char*)&slen, 2);
    packet += sender;
    uint32_t mlen = msg.size();
    packet.push_back((mlen >> 16) & 0xFF);
    packet.push_back((mlen >> 8) & 0xFF);
    packet.push_back(mlen & 0xFF);
    packet += msg;
    return packet;
}

// Build the message to a specific client with the protocol
string buildToClient(const string& sender, const string& msg) {
    string packet = "T";
    uint16_t slen = htons(sender.size());
    packet.append((char*)&slen, 2);
    packet += sender;
    uint32_t mlen = msg.size();
    packet.push_back((mlen >> 16) & 0xFF);
    packet.push_back((mlen >> 8) & 0xFF);
    packet.push_back(mlen & 0xFF);
    packet += msg;
    return packet;
}

//Build list with the protocol
string buildList() {
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
    return packet;
}

// Función para construir mensaje de archivo
string buildFile(const string& sender, const string& filename, const char* file_data, uint64_t file_size) {
    string packet = "F"; // Tipo 'F' para archivo
    
    // Remitente
    uint16_t slen = htons(sender.size());
    packet.append((char*)&slen, 2);
    packet += sender;
    
    // Longitud del nombre del archivo (3 bytes)
    uint32_t flen = filename.size();
    packet.push_back((flen >> 16) & 0xFF);
    packet.push_back((flen >> 8) & 0xFF);
    packet.push_back(flen & 0xFF);
    
    // Nombre del archivo
    packet += filename;
    
    // Tamaño del archivo
    for (int i = 9; i >= 0; i--) {
        packet.push_back((file_size >> (i * 8)) & 0xFF);
    }
    
    // Contenido del archivo
    packet.append(file_data, file_size);
    
    return packet;
}

// Manage each client with threads
void handleClient(int client_socket) {
    char header[4];
    string nickname;

    // Read nickname (n)
    if (recv(client_socket, header, 1, 0) <= 0) { close(client_socket); return; }
    if (header[0] != 'n') { close(client_socket); return; }

    //length of nickname
    if (recv(client_socket, header, 2, 0) <= 0) { close(client_socket); return; }
    uint16_t nlen;
    memcpy(&nlen, header, 2);
    nlen = ntohs(nlen);

    //take the nickname
    char* buffer = new char[nlen+1];
    if (recv(client_socket, buffer, nlen, 0) <= 0) { delete[] buffer; close(client_socket); return; }
    buffer[nlen]='\0';
    nickname = string(buffer);
    delete[] buffer;

    // Print received nickname protocol
    string nickPacket = "n";
    uint16_t nlen_net = htons(nlen);
    nickPacket.append((char*)&nlen_net, 2);
    nickPacket += nickname;
    cout << nickname << " received: " << formatProtocol(nickPacket) << endl;

    {
        lock_guard<mutex> lock(clients_mutex);
        if (clients.count(nickname)) {
            string err = buildError("Nickname already taken");
            cout << "Server sending error to " << nickname << ": " << formatProtocol(err) << endl;
            send(client_socket, err.c_str(), err.size(), 0);
            close(client_socket);
            return;
        }
        clients[nickname] = client_socket;
    }

    cout << nickname << " joined" << endl;
    string joinMsg = buildBroadcast(nickname," joined the chat");
    cout << "Server broadcasting: " << formatProtocol(joinMsg) << endl;
    sendAll(joinMsg);

    while (true) {
        char type;
        int r = recv(client_socket, &type, 1, 0);
        if (r <= 0) break;

        if (type == 'm') {
            // broadcast
            if (recv(client_socket, header, 3, 0) <= 0) break;
            int len = ((unsigned char)header[0]<<16) |
                      ((unsigned char)header[1]<<8) |
                      (unsigned char)header[2];
            char* buf = new char[len+1];
            if (recv(client_socket, buf, len, 0) <= 0) { delete[] buf; break; }
            buf[len]='\0';
            string msg = string(buf);
            delete[] buf;
            
            string broadcastPacket = "m";
            broadcastPacket += string(header, 3);
            broadcastPacket += msg;
            cout << nickname << " received: " << formatProtocol(broadcastPacket) << endl;
            
            string packet = buildBroadcast(nickname, msg);
            sendAll(packet, client_socket);
        }
        else if (type == 't') {
            // to client
            if (recv(client_socket, header, 2, 0) <= 0) break;
            uint16_t dlen;
            memcpy(&dlen, header, 2);
            dlen = ntohs(dlen);
            char* dbuf = new char[dlen+1];
            if (recv(client_socket, dbuf, dlen, 0) <= 0) { delete[] dbuf; break; }
            dbuf[dlen]='\0';
            string dest(dbuf);
            delete[] dbuf;

            if (recv(client_socket, header, 3, 0) <= 0) break;
            int mlen = ((unsigned char)header[0]<<16) |
                       ((unsigned char)header[1]<<8) |
                       (unsigned char)header[2];
            char* mbuf = new char[mlen+1];
            if (recv(client_socket, mbuf, mlen, 0) <= 0) { delete[] mbuf; break; }
            mbuf[mlen]='\0';
            string msg(mbuf);
            delete[] mbuf;

            string privatePacket = "t";
            uint16_t dlen_net = htons(dlen);
            privatePacket.append((char*)&dlen_net, 2);
            privatePacket += dest;
            privatePacket += string(header, 3);
            privatePacket += msg;
            cout << nickname << " received: " << formatProtocol(privatePacket) << endl;

            if (clients.count(dest)) {
                string packet = buildToClient(nickname, msg);
                sendToClient(dest, packet);
            } else {
                string err = buildError("User " + dest + " not found");
                sendToClient(nickname, err);
            }
        }
        else if (type == 'l') {
            cout << nickname << " received: l" << endl;
            string listMsg = buildList();
            cout << "Server sending list to " << nickname << ": " << formatProtocol(listMsg) << endl;
            send(client_socket, listMsg.c_str(), listMsg.size(), 0);
        }
        else if (type == 'x') {
            cout << nickname << " received: x" << endl;
            string closeMsg = buildClose();
            cout << "Server sending close to " << nickname << ": " << formatProtocol(closeMsg) << endl;
            send(client_socket, closeMsg.c_str(), closeMsg.size(), 0);
            break;
        }
        else if (type=='f') {
            if (recv(client_socket, header, 2, 0) <= 0) break;
            uint16_t dlen;
            memcpy(&dlen, header, 2);
            dlen = ntohs(dlen);
            char* dbuf = new char[dlen+1];
            
            int bytes_received = 0;
            while (bytes_received < dlen) {
                int r = recv(client_socket, dbuf + bytes_received, dlen - bytes_received, 0);
                if (r <= 0) { delete[] dbuf; break; }
                bytes_received += r;
            }
            dbuf[dlen]='\0';
            string dest(dbuf);
            delete[] dbuf;

            if (recv(client_socket, header, 3, 0) <= 0) break;
            uint32_t flen = ((unsigned char)header[0] << 16) |
                        ((unsigned char)header[1] << 8) |
                        (unsigned char)header[2];

            char* fbuf = new char[flen+1];
            bytes_received = 0;
            while (bytes_received < flen) {
                int r = recv(client_socket, fbuf + bytes_received, flen - bytes_received, 0);
                if (r <= 0) { delete[] fbuf; break; }
                bytes_received += r;
            }
            fbuf[flen] = '\0';
            string filename(fbuf);
            delete[] fbuf;

            char size_buf[10];
            bytes_received = 0;
            while (bytes_received < 10) {
                int r = recv(client_socket, size_buf + bytes_received, 10 - bytes_received, 0);
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
                int r = recv(client_socket, file_data + bytes_received, fsize - bytes_received, 0);
                if (r <= 0) { 
                    delete[] file_data; 
                    break; 
                }
                bytes_received += r;
            }

            string filePacket = "f";
            uint16_t dlen_net = htons(dlen);
            filePacket.append((char*)&dlen_net, 2);
            filePacket += dest;
            filePacket += string(header, 3);
            filePacket += filename;
            filePacket += string(size_buf, 10);
            cout << nickname << " received file: " << formatProtocol(filePacket.substr(0, 50)) << "..." << endl;

            if (clients.count(dest)) {
                string packet = buildFile(nickname, filename, file_data, fsize);
                sendToClient(dest, packet);
                cout << "Archivo " << filename << " (" << fsize << " bytes) enviado a " << dest << endl;
            } else {
                string err = buildError("User " + dest + " not found");
                sendToClient(nickname, err);
            }
            
            delete[] file_data;
        }
    }

    {
        lock_guard<mutex> lock(clients_mutex);
        clients.erase(nickname);
    }
    cout << nickname << " left." << endl;
    
    string leaveMsg = buildBroadcast(nickname, " left the chat");
    cout << "Server broadcasting: " << formatProtocol(leaveMsg) << endl;
    sendAll(leaveMsg);
    
    close(client_socket);
}

int main() {
    int serverSocket;
    struct sockaddr_in address;
    int opt=1;
    int addrlen=sizeof(address);

    serverSocket = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(serverSocket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    address.sin_family=AF_INET;
    address.sin_addr.s_addr=INADDR_ANY;
    address.sin_port=htons(PORT);

    bind(serverSocket, (struct sockaddr*)&address, sizeof(address));
    listen(serverSocket, 5);

    cout << "Server running on port " << PORT << endl;

    while (true) {
        int client_socket = accept(serverSocket, (struct sockaddr*)&address, (socklen_t*)&addrlen);
        thread(handleClient, client_socket).detach();
    }

    close(serverSocket);
    return 0;
}
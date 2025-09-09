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

using namespace std;

#define PORT 45000

/*
    n: Nickname (client → server)
    m: Broadcast message (client → server)
    t: Private message (client → server)
    l: List of clients (client → server)
    x: Close connection (client → server)
    E: Error (server → client)
    M: Broadcast message (server → client)
    T: Private message (server → client)
    L: List of clients (server → client)
    X: Close connection (server → client)
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
         << "  /exit      -> quit" << endl;

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
        else {
            cout << "Unknown command. Available: /all, /to, /list, /exit" << endl;
        }
    }

    close(sock);
    if (t.joinable()) {
        t.join();
    }
    return 0;
}
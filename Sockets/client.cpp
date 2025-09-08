#include <iostream>
#include <string>
#include <thread>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <cstring>

using namespace std;

#define PORT 45000

/*
    n: Nickname (client → server)
    m: Broadcast message (client → server)
    t: Private message (client → server)
    l: List of clients (client → server)
    E: Error (server → client)
    M: Broadcast message (server → client)
    T: Private message (server → client)
    L: List of clients (server → client)
*/

void sendNickname(int sock, const string nick) {
    string packet = "n";
    uint16_t len = htons(nick.size());
    packet.append((char*)&len,2);
    packet += nick;
    send(sock, packet.c_str(), packet.size(), 0);
}

void sendBroadcast(int sock, const string msg) {
    string packet = "m";
    uint32_t len = msg.size();
    packet.push_back((len>>16)&0xFF);
    packet.push_back((len>>8)&0xFF);
    packet.push_back(len&0xFF);
    packet += msg;
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
    send(sock, packet.c_str(), packet.size(), 0);
}

void requestList(int sock) {
    string packet = "l";
    send(sock, packet.c_str(), 1, 0);
}

// Receiver thread
void receiveMessages(int sock) {
    char header[4];
    while (true) {
        int r = recv(sock, header, 1, 0);
        if (r<=0) { cout << "Disconnected.\n"; break; }
        char type = header[0];

        if (type=='E') {
            recv(sock, header, 3, 0);
            int len = ((unsigned char)header[0] << 16) |
                    ((unsigned char)header[1] << 8) |
                    (unsigned char)header[2];
            char* buf = new char[len+1];
            recv(sock, buf, len, 0);
            buf[len]='\0';
            cout << "[Error] " << buf << endl;
            delete[] buf;
            break;
        }
        else if (type=='M') {
            recv(sock, header,3,0);
            int len = ((unsigned char)header[0]<<16)|
                      ((unsigned char)header[1]<<8)|
                      (unsigned char)header[2];
            char* buf=new char[len+1];
            recv(sock,buf,len,0); buf[len]='\0';
            cout << "[Broadcast] " << buf << endl;
            delete[] buf;
        }
        else if (type=='T') {
            recv(sock, header,2,0);
            uint16_t dlen; memcpy(&dlen,header,2); dlen=ntohs(dlen);
            char* dbuf=new char[dlen+1];
            recv(sock,dbuf,dlen,0); dbuf[dlen]='\0';
            string dest(dbuf);
            delete[] dbuf;

            recv(sock,header,3,0);
            int mlen = ((unsigned char)header[0]<<16)|
                       ((unsigned char)header[1]<<8)|
                       (unsigned char)header[2];
            char* mbuf=new char[mlen+1];
            recv(sock,mbuf,mlen,0); mbuf[mlen]='\0';
            cout << "[Private to " << dest << "] " << mbuf << endl;
            delete[] mbuf;
        }
        else if (type=='L') {
            recv(sock, header,2,0);
            uint16_t len; memcpy(&len,header,2); len=ntohs(len);
            char* buf=new char[len+1];
            recv(sock,buf,len,0); buf[len]='\0';
            cout << "[Clients] " << buf << endl;
            delete[] buf;
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
        cout << "Connection failed\n"; return 0;
    }

    string nickname;
    cout << "Enter nickname: ";
    getline(cin,nickname);
    sendNickname(sock,nickname);

    thread t(receiveMessages,sock);

    cout << "Commands:\n"
         << "  /all msg   -> broadcast\n"
         << "  /to user msg -> private\n"
         << "  /list -> show users\n"
         << "  /exit -> quit\n";

    string line;
    while (getline(cin,line)) {
        if (line=="/exit") break;
        if (line.rfind("/all ",0)==0) {
            sendBroadcast(sock,line.substr(5));
        }
        else if (line.rfind("/to ",0)==0) {
            size_t sp=line.find(' ',4);
            if (sp!=string::npos) {
                string dest=line.substr(4,sp-4);
                string msg=line.substr(sp+1);
                sendToClient(sock,dest,msg);
            }
        }
        else if (line=="/list") {
            requestList(sock);
        }
        else {
            cout << "Unknown command\n";
        }
    }

    close(sock);
    t.join();
    return 0;
}

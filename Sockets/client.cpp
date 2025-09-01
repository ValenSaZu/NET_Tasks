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
#define BUFFER_SIZE 1024

enum MessageType {
    NICKNAME = 'n',
    MESSAGE = 'm'
};

struct ProtocolMessage {
    MessageType type;
    int length;
    string content;
};

void sendProtocolMessage(int socket, MessageType type, const string& content) {
    // Create header: type (1 byte) + length (2 or 3 bytes)
    char header[4];
    header[0] = static_cast<char>(type);
    
    if (type == NICKNAME) {
        // 2 bytes for nickname length
        uint16_t net_length = htons(content.length());
        memcpy(header + 1, &net_length, 2);
        send(socket, header, 3, 0);
    } else {
        // 3 bytes for message length
        uint32_t length = content.length();
        // Store in 3 bytes (24 bits)
        header[1] = (length >> 16) & 0xFF;
        header[2] = (length >> 8) & 0xFF;
        header[3] = length & 0xFF;
        send(socket, header, 4, 0);
    }
    
    send(socket, content.c_str(), content.length(), 0);
}

bool receiveProtocolMessage(int socket, ProtocolMessage& msg) {
    char header[4];
    
    // Receive message type (1 byte)
    int bytes_received = recv(socket, header, 1, 0);
    if (bytes_received <= 0) {
        return false; // Connection closed or error
    }
    
    msg.type = static_cast<MessageType>(header[0]);
    
    if (msg.type == NICKNAME) {
        // Receive nickname length (2 bytes)
        bytes_received = recv(socket, header + 1, 2, 0);
        if (bytes_received <= 0) {
            return false;
        }
        
        uint16_t net_length;
        memcpy(&net_length, header + 1, 2);
        msg.length = ntohs(net_length);
    } else {
        // Receive message length (3 bytes)
        bytes_received = recv(socket, header + 1, 3, 0);
        if (bytes_received <= 0) {
            return false;
        }
        
        // Reconstruct 24-bit length from 3 bytes
        msg.length = (static_cast<unsigned char>(header[1]) << 16) |
                     (static_cast<unsigned char>(header[2]) << 8) |
                     static_cast<unsigned char>(header[3]);
    }
    
    // Receive content
    char* buffer = new char[msg.length + 1];
    bytes_received = recv(socket, buffer, msg.length, 0);
    if (bytes_received <= 0) {
        delete[] buffer;
        return false;
    }
    
    buffer[msg.length] = '\0';
    msg.content = string(buffer);
    delete[] buffer;
    
    return true;
}

void receiveMessages(int socket) {
    ProtocolMessage msg;
    
    // First message should be server's nickname
    if (receiveProtocolMessage(socket, msg) && msg.type == NICKNAME) {
        cout << "Connected to server: " << msg.content << endl;
        cout << "Start chatting! Type 'chau' to disconnect." << endl;
    }
    
    while (true) {
        if (!receiveProtocolMessage(socket, msg)) {
            cout << "Disconnected from server." << endl;
            break;
        }
        
        if (msg.type == MESSAGE) {
            cout << msg.content << endl;
        }
    }
}

void handleClientInput(int socket) {
    string message;
    do {
        getline(cin, message);
        sendProtocolMessage(socket, MESSAGE, message);
    } while (message != "chau");
}

int main() {
    int sock;
    struct sockaddr_in serv_addr;
    
    // Get client nickname
    string nickname;
    cout << "Enter your nickname: ";
    getline(cin, nickname);
    
    // Create socket
    if ((sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) < 0) {
        cout << "Socket creation error" << endl;
        return -1;
    }
    
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(PORT);
    
    if (inet_pton(AF_INET, "127.0.0.1", &serv_addr.sin_addr) <= 0) {
        cout << "Invalid address" << endl;
        return -1;
    }
    
    // Connect to server
    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        cout << "Connection Failed" << endl;
        return -1;
    }
    
    // Send nickname to server
    sendProtocolMessage(sock, NICKNAME, nickname);
    
    // Start thread to receive messages
    thread receiver(receiveMessages, sock);
    
    // Handle client input in the main thread
    handleClientInput(sock);
    
    // Clean up
    receiver.join();
    close(sock);
    
    return 0;
}
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
    MESSAGE = 'm',
    DISCONNECT = 'd'
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

ProtocolMessage receiveProtocolMessage(int socket) {
    ProtocolMessage msg;
    char header[4];
    
    // Receive message type (1 byte)
    int bytes_received = recv(socket, header, 1, 0);
    if (bytes_received <= 0) {
        msg.type = DISCONNECT;
        return msg;
    }
    
    msg.type = static_cast<MessageType>(header[0]);
    
    if (msg.type == NICKNAME) {
        // Receive nickname length (2 bytes)
        bytes_received = recv(socket, header + 1, 2, 0);
        if (bytes_received <= 0) {
            msg.type = DISCONNECT;
            return msg;
        }
        
        uint16_t net_length;
        memcpy(&net_length, header + 1, 2);
        msg.length = ntohs(net_length);
    } else {
        // Receive message length (3 bytes)
        bytes_received = recv(socket, header + 1, 3, 0);
        if (bytes_received <= 0) {
            msg.type = DISCONNECT;
            return msg;
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
        msg.type = DISCONNECT;
        delete[] buffer;
        return msg;
    }
    
    buffer[msg.length] = '\0';
    msg.content = string(buffer);
    delete[] buffer;
    
    return msg;
}

void handleClient(int client_socket, const string& server_nickname) {
    string client_nickname;
    
    // Get nickname from client
    ProtocolMessage nick_msg = receiveProtocolMessage(client_socket);
    if (nick_msg.type != NICKNAME) {
        close(client_socket);
        return;
    }
    
    client_nickname = nick_msg.content;
    
    // Send server nickname to client
    sendProtocolMessage(client_socket, NICKNAME, server_nickname);
    
    cout << client_nickname << " joined the chat." << endl;
    
    // Handle messages from client
    while (true) {
        ProtocolMessage msg = receiveProtocolMessage(client_socket);
        
        if (msg.type == DISCONNECT) {
            break;
        }
        
        if (msg.type == MESSAGE) {
            // Format and display client's message
            cout << "(" << client_nickname << "): " << msg.content << endl;
            
            if (msg.content == "chau") {
                break;
            }
        }
    }
    
    cout << client_nickname << " left the chat." << endl;
    close(client_socket);
}

void handleServerInput(int client_socket, const string& server_nickname) {
    string message;
    do {
        getline(cin, message);
        
        // Format the message before sending
        string formatted_msg = "(" + server_nickname + "): " + message;
        sendProtocolMessage(client_socket, MESSAGE, formatted_msg);
        
    } while (message != "chau");
}

int main() {
    int server_fd, client_socket;
    struct sockaddr_in address;
    int opt = 1;
    int addrlen = sizeof(address);
    
    // Get server nickname
    string server_nickname;
    cout << "Enter server nickname: ";
    getline(cin, server_nickname);
    
    // Create socket
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }
    
    // Set socket options
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt))) {
        perror("setsockopt");
        exit(EXIT_FAILURE);
    }
    
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);
    
    // Bind socket
    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("bind failed");
        exit(EXIT_FAILURE);
    }
    
    // Listen for connections
    if (listen(server_fd, 1) < 0) {
        perror("listen");
        exit(EXIT_FAILURE);
    }
    
    cout << "Server '" << server_nickname << "' started on port " << PORT << endl;
    
    while (true) {
        cout << "Waiting for a client to connect..." << endl;
        
        // Accept incoming connection
        if ((client_socket = accept(server_fd, (struct sockaddr *)&address, (socklen_t*)&addrlen)) < 0) {
            perror("accept");
            exit(EXIT_FAILURE);
        }
        
        cout << "Client connected. Starting chat session..." << endl;
        
        // Handle client in a separate thread
        thread client_thread(handleClient, client_socket, server_nickname);
        
        // Handle server input in the main thread
        handleServerInput(client_socket, server_nickname);
        
        // Wait for client thread to finish
        client_thread.join();
        
        cout << "Chat session ended. Ready for a new client." << endl;
    }
    
    close(server_fd);
    return 0;
}
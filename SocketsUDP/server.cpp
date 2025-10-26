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
#include "sala.h"
#include "sala_serialized.h"

using namespace std;

#define PORT 45000

map<string,int> clients;
mutex clients_mutex;

int lengthPacket = 777;

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

string completePacket(string &packet, int maxLengthPacket){
    uint64_t len = packet.size();
    for(len ; len<=maxLengthPacket len++){
        packet += '#';
    }
    return packet;
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

// Function to build file message
string buildFile(const string& sender, const string& filename, const char* file_data, uint64_t file_size) {
    string packet = "F"; // Type 'F' for file
    
    // Sender
    uint16_t slen = htons(sender.size());
    packet.append((char*)&slen, 2);
    packet += sender;
    
    // Filename length (3 bytes)
    uint32_t flen = filename.size();
    packet.push_back((flen >> 16) & 0xFF);
    packet.push_back((flen >> 8) & 0xFF);
    packet.push_back(flen & 0xFF);
    
    // Filename
    packet += filename;
    
    // File size
    for (int i = 9; i >= 0; i--) {
        packet.push_back((file_size >> (i * 8)) & 0xFF);
    }
    
    // File content
    packet.append(file_data, file_size);
    
    return packet;
}

// Function to build object message
string buildObject(const string& sender, const vector<char>& objectData) {
    string packet = "O";
    
    // Sender
    uint16_t slen = htons(sender.size());
    packet.append((char*)&slen, 2);
    packet += sender;
    
    // Object size (4 bytes)
    uint32_t objSize = htonl(static_cast<uint32_t>(objectData.size()));
    packet.append(reinterpret_cast<const char*>(&objSize), sizeof(objSize));
    
    // Object content
    packet.append(objectData.data(), objectData.size());
    
    return packet;
}

struct Game {
    string player1;
    string player2;
    vector<char> board;
    string currentPlayer;
    bool gameActive;
};

map<pair<string, string>, Game> activeGames;
mutex games_mutex;

string buildGameRequest(const string& sender) {
    string packet = "J";
    uint16_t slen = htons(sender.size());
    packet.append((char*)&slen, 2);
    packet += sender;
    return packet;
}

string buildGameResponse(const string& sender, bool accepted) {
    string packet = "j";
    uint16_t slen = htons(sender.size());
    packet.append((char*)&slen, 2);
    packet += sender;
    packet.push_back(accepted ? 'y' : 'n');
    return packet;
}

// Modified buildBoard to include current player
string buildBoard(const vector<char>& board, const string& currentPlayer) {
    string packet = "B";
    uint16_t board_len = htons(board.size());
    packet.append((char*)&board_len, 2);
    packet.append(board.begin(), board.end());
    
    uint16_t player_len = htons(currentPlayer.size());
    packet.append((char*)&player_len, 2);
    packet += currentPlayer;
    
    return packet;
}

string buildGameResult(char result) {
    string packet = "W";
    packet.push_back(result);
    return packet;
}

bool checkWinner(const vector<char>& board, char player) {
    // Rows
    for (int i = 0; i < 3; i++) {
        if (board[i*3] == player && board[i*3+1] == player && board[i*3+2] == player)
            return true;
    }
    // Columns
    for (int i = 0; i < 3; i++) {
        if (board[i] == player && board[i+3] == player && board[i+6] == player)
            return true;
    }
    // Diagonals
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

    cout << nickname << " connected" << endl;

    while (true) {
        int r = recv(client_socket, header, 1, 0);
        if (r <= 0) break;
        char type = header[0];

        if (type == 'm') {
            // Read message length
            if (recv(client_socket, header, 3, 0) <= 0) break;
            int len = ((unsigned char)header[0] << 16) |
                    ((unsigned char)header[1] << 8) |
                    (unsigned char)header[2];
            
            char* buf = new char[len+1];
            if (recv(client_socket, buf, len, 0) <= 0) { delete[] buf; break; }
            buf[len] = '\0';
            
            string broadcastPacket = "m";
            broadcastPacket += string(header, 3);
            broadcastPacket += string(buf, len);
            cout << nickname << " received: " << formatProtocol(broadcastPacket) << endl;
            
            string msg = buildBroadcast(nickname, string(buf, len));
            sendAll(msg, client_socket);
            delete[] buf;
        }
        else if (type == 't') {
            // Read destination length
            if (recv(client_socket, header, 2, 0) <= 0) break;
            uint16_t dlen;
            memcpy(&dlen, header, 2);
            dlen = ntohs(dlen);
            
            // Read destination
            char* dbuf = new char[dlen+1];
            if (recv(client_socket, dbuf, dlen, 0) <= 0) { delete[] dbuf; break; }
            dbuf[dlen] = '\0';
            string dest(dbuf);
            delete[] dbuf;
            
            // Read message length
            if (recv(client_socket, header, 3, 0) <= 0) break;
            int mlen = ((unsigned char)header[0] << 16) |
                     ((unsigned char)header[1] << 8) |
                     (unsigned char)header[2];
            
            char* mbuf = new char[mlen+1];
            if (recv(client_socket, mbuf, mlen, 0) <= 0) { delete[] mbuf; break; }
            mbuf[mlen] = '\0';
            
            string privatePacket = "t";
            uint16_t dlen_net = htons(dlen);
            privatePacket.append((char*)&dlen_net, 2);
            privatePacket += dest;
            privatePacket += string(header, 3);
            privatePacket += string(mbuf, mlen);
            cout << nickname << " received: " << formatProtocol(privatePacket) << endl;
            
            string msg = buildToClient(nickname, string(mbuf, mlen));
            sendToClient(dest, msg);
            delete[] mbuf;
        }
        else if (type == 'l') {
            cout << nickname << " received: l" << endl;
            string list = buildList();
            cout << "Server sending list to " << nickname << ": " << formatProtocol(list) << endl;
            send(client_socket, list.c_str(), list.size(), 0);
        }
        else if (type == 'x') {
            cout << nickname << " received: x" << endl;
            break;
        }
        else if (type == 'f') {
            // Read destination length
            if (recv(client_socket, header, 2, 0) <= 0) break;
            uint16_t dlen;
            memcpy(&dlen, header, 2);
            dlen = ntohs(dlen);
            
            // Read destination
            char* dbuf = new char[dlen+1];
            if (recv(client_socket, dbuf, dlen, 0) <= 0) { delete[] dbuf; break; }
            dbuf[dlen] = '\0';
            string dest(dbuf);
            delete[] dbuf;
            
            // Read filename length
            if (recv(client_socket, header, 3, 0) <= 0) break;
            uint32_t flen = ((unsigned char)header[0] << 16) |
                          ((unsigned char)header[1] << 8) |
                          (unsigned char)header[2];
            
            // Read filename
            char* fbuf = new char[flen+1];
            if (recv(client_socket, fbuf, flen, 0) <= 0) { delete[] fbuf; break; }
            fbuf[flen] = '\0';
            string filename(fbuf);
            delete[] fbuf;
            
            // Read file size
            char size_buf[10];
            int bytes_received = 0;
            while (bytes_received < 10) {
                int r = recv(client_socket, size_buf + bytes_received, 10 - bytes_received, 0);
                if (r <= 0) { close(client_socket); return; }
                bytes_received += r;
            }
            
            uint64_t fsize = 0;
            for (int i = 0; i < 10; i++) {
                fsize = (fsize << 8) | (unsigned char)size_buf[i];
            }
            
            // Read file data
            char* file_data = new char[fsize];
            bytes_received = 0;
            while (bytes_received < fsize) {
                int r = recv(client_socket, file_data + bytes_received, fsize - bytes_received, 0);
                if (r <= 0) { 
                    delete[] file_data; 
                    close(client_socket); 
                    return; 
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
            filePacket += string(file_data, min(fsize, (uint64_t)10)) + "...";
            cout << nickname << " received: " << formatProtocol(filePacket) << endl;
            
            string msg = buildFile(nickname, filename, file_data, fsize);
            sendToClient(dest, msg);
            
            delete[] file_data;
        }
        else if (type == 'o') {
            // Read destination length
            if (recv(client_socket, header, 2, 0) <= 0) break;
            uint16_t dlen;
            memcpy(&dlen, header, 2);
            dlen = ntohs(dlen);
            
            // Read destination
            char* dbuf = new char[dlen + 1];
            if (recv(client_socket, dbuf, dlen, 0) <= 0) {
                delete[] dbuf;
                break;
            }
            dbuf[dlen] = '\0';
            string dest(dbuf);
            delete[] dbuf;
            
            // Read object size
            char sizeBuf[4];
            if (recv(client_socket, sizeBuf, 4, 0) <= 0) break;
            
            uint32_t objSize;
            memcpy(&objSize, sizeBuf, 4);
            objSize = ntohl(objSize);
            
            // Read object content
            vector<char> objectBuf(objSize);
            if (recv(client_socket, objectBuf.data(), objSize, 0) <= 0) break;
            
            string objectPacket = "o";
            uint16_t dlen_net = htons(dlen);
            objectPacket.append((char*)&dlen_net, 2);
            objectPacket += dest;
            objectPacket += string(sizeBuf, 4);
            objectPacket += string(objectBuf.begin(), objectBuf.begin() + min(objSize, (uint32_t)10)) + "...";
            cout << nickname << " received: " << formatProtocol(objectPacket) << endl;
            
            string msg = buildObject(nickname, objectBuf);
            sendToClient(dest, msg);
        }
        else if (type == 'J') {
            // Game request
            if (recv(client_socket, header, 2, 0) <= 0) break;
            uint16_t dlen;
            memcpy(&dlen, header, 2);
            dlen = ntohs(dlen);
            
            char* dbuf = new char[dlen + 1];
            if (recv(client_socket, dbuf, dlen, 0) <= 0) {
                delete[] dbuf;
                break;
            }
            dbuf[dlen] = '\0';
            string dest(dbuf);
            delete[] dbuf;
            
            string gameRequestPacket = "J";
            uint16_t dlen_net = htons(dlen);
            gameRequestPacket.append((char*)&dlen_net, 2);
            gameRequestPacket += dest;
            cout << nickname << " received: " << formatProtocol(gameRequestPacket) << endl;
            
            string msg = buildGameRequest(nickname);
            sendToClient(dest, msg);
        }
        else if (type == 'j') {
            // Game response
            if (recv(client_socket, header, 2, 0) <= 0) break;
            uint16_t slen;
            memcpy(&slen, header, 2);
            slen = ntohs(slen);
            
            char* sbuf = new char[slen + 1];
            if (recv(client_socket, sbuf, slen, 0) <= 0) {
                delete[] sbuf;
                break;
            }
            sbuf[slen] = '\0';
            string sender(sbuf);
            delete[] sbuf;
            
            char response;
            if (recv(client_socket, &response, 1, 0) <= 0) break;
            
            string gameResponsePacket = "j";
            uint16_t slen_net = htons(slen);
            gameResponsePacket.append((char*)&slen_net, 2);
            gameResponsePacket += sender;
            gameResponsePacket += response;
            cout << nickname << " received: " << formatProtocol(gameResponsePacket) << endl;
            
            string msg = buildGameResponse(nickname, response == 'y');
            sendToClient(sender, msg);
            
            if (response == 'y') {
                // Start the game
                lock_guard<mutex> lock(games_mutex);
                pair<string, string> gameKey = make_pair(min(nickname, sender), max(nickname, sender));
                initializeGame(activeGames[gameKey], nickname, sender);
                
                string boardMsg = buildBoard(activeGames[gameKey].board, activeGames[gameKey].currentPlayer);
                sendToClient(nickname, boardMsg);
                sendToClient(sender, boardMsg);
                cout << "Game started between " << nickname << " and " << sender << ". First turn: " << activeGames[gameKey].currentPlayer << endl;
            }
        }
        else if (type == 'P') {
            // Position move
            char posBuf[4];
            if (recv(client_socket, posBuf, 4, 0) <= 0) break;
            
            uint32_t position = ((unsigned char)posBuf[0] << 24) |
                              ((unsigned char)posBuf[1] << 16) |
                              ((unsigned char)posBuf[2] << 8) |
                              (unsigned char)posBuf[3];
            
            string positionPacket = "P";
            positionPacket += string(posBuf, 4);
            cout << nickname << " received: " << formatProtocol(positionPacket) << endl;
            
            // Find the game
            lock_guard<mutex> lock(games_mutex);
            Game* currentGame = nullptr;
            string opponent;
            pair<string, string> gameKey;
            
            for (auto& game : activeGames) {
                if ((game.second.player1 == nickname || game.second.player2 == nickname) && 
                    game.second.gameActive) {
                    currentGame = &game.second;
                    opponent = (game.second.player1 == nickname) ? game.second.player2 : game.second.player1;
                    gameKey = game.first;
                    break;
                }
            }
            
            if (!currentGame) {
                string err = buildError("No active game found");
                sendToClient(nickname, err);
                continue;
            }
            
            if (currentGame->currentPlayer != nickname) {
                string err = buildError("Not your turn");
                sendToClient(nickname, err);
                continue;
            }
            
            if (position >= 9) {
                string err = buildError("Invalid position");
                sendToClient(nickname, err);
                continue;
            }
            
            // Validate move
            if (currentGame->board[position] == ' ') {
                currentGame->board[position] = (nickname == currentGame->player1) ? 'X' : 'O';
                
                // Check for winner
                char currentSymbol = (nickname == currentGame->player1) ? 'X' : 'O';
                if (checkWinner(currentGame->board, currentSymbol)) {
                    // Current player wins
                    string result1 = buildGameResult('1');
                    string result2 = buildGameResult('0');
                    sendToClient(nickname, result1);
                    sendToClient(opponent, result2);
                    currentGame->gameActive = false;
                    activeGames.erase(gameKey);
                    cout << "Game finished. Winner: " << nickname << endl;
                } else if (checkIsPositionUsed(currentGame->board)) {
                    // Draw
                    string result = buildGameResult('2');
                    sendToClient(nickname, result);
                    sendToClient(opponent, result);
                    currentGame->gameActive = false;
                    activeGames.erase(gameKey);
                    cout << "Game finished in draw between " << nickname << " and " << opponent << endl;
                } else {
                    // Continue game - switch turns
                    currentGame->currentPlayer = opponent;
                    
                    // Send updated board to both players with turn information
                    string boardMsg = buildBoard(currentGame->board, currentGame->currentPlayer);
                    sendToClient(nickname, boardMsg);
                    sendToClient(opponent, boardMsg);
                    cout << "Turn switched to: " << currentGame->currentPlayer << endl;
                }
            } else {
                string err = buildError("Position already occupied");
                sendToClient(nickname, err);
            }
        }
    }

    {
        lock_guard<mutex> lock(clients_mutex);
        clients.erase(nickname);
    }
    
    // Remove any active games involving this player
    {
        lock_guard<mutex> lock(games_mutex);
        vector<pair<string, string>> toRemove;
        for (auto& game : activeGames) {
            if (game.second.player1 == nickname || game.second.player2 == nickname) {
                toRemove.push_back(game.first);
            }
        }
        for (auto& key : toRemove) {
            // Notify the other player that the game ended
            string otherPlayer = (activeGames[key].player1 == nickname) ? 
                               activeGames[key].player2 : activeGames[key].player1;
            if (clients.count(otherPlayer)) {
                string result = buildGameResult('3');
                sendToClient(otherPlayer, result);
            }
            activeGames.erase(key);
        }
    }

    cout << nickname << " disconnected" << endl;
    close(client_socket);
}

int main() {
    int server_fd;
    struct sockaddr_in address;
    int opt = 1;

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    bind(server_fd, (struct sockaddr*)&address, sizeof(address));
    listen(server_fd, 3);

    cout << "Server listening on port " << PORT << endl;

    while (true) {
        int client_socket = accept(server_fd, nullptr, nullptr);
        thread t(handleClient, client_socket);
        t.detach();
    }

    return 0;
}
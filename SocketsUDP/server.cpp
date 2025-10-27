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

string completePacket(string &packet, int maxLengthPacket) {
    size_t len = packet.size();
    if (len < (size_t)maxLengthPacket) {
        packet.append(maxLengthPacket - len, '#');
    }
    return packet;
}

vector<string> fragmentPackets(const string &header, const string &data, int maxLengthPacket, int sizeFieldBytes) {
    vector<string> packets;

    size_t overhead = 1 + sizeFieldBytes;
    size_t maxDataPerPacket = maxLengthPacket - overhead;
    size_t offset = 0;
    int numPacket = 1;

    while (offset < data.size()) {
        size_t remaining = data.size() - offset;
        size_t chunkSize = min(remaining, maxDataPerPacket);
        bool isLast = (offset + chunkSize >= data.size());

        string fragment;
        fragment.push_back(isLast ? 0 : (char)numPacket);

        if (numPacket == 1)
            fragment += header;

        uint32_t len = (uint32_t)chunkSize;
        for (int i = sizeFieldBytes - 1; i >= 0; --i)
            fragment.push_back((len >> (8 * i)) & 0xFF);

        fragment += data.substr(offset, chunkSize);

        if (fragment.size() < (size_t)maxLengthPacket)
            fragment.append(maxLengthPacket - fragment.size(), '#');

        packets.push_back(fragment);
        offset += chunkSize;
        numPacket++;
    }

    return packets;
}

struct Fragment{
    string header;
    string data;
};

map<pair<int,char>,Fragment> clientsBuffer;
mutex fragmentMutex;

bool reconstructPacket(int client, string fragment, string& fullMsg){
    lock_guard<mutex> lock(fragmentMutex);
    if (fragment.empty()) return false;

    unsigned char index = fragment[0];
    char type = fragment[1];
    pair<int,char> identifier = make_pair(client, type);

    if (!clientsBuffer.count(identifier)) {
        clientsBuffer[identifier] = Fragment{};
    }

    clientsBuffer[identifier].data += fragment.substr(1);

    if (index == 0) {
        fullMsg = clientsBuffer[identifier].data;
        clientsBuffer.erase(identifier);
        return true;
    }

    return false;
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
vector<string> buildError(const string& msg) {
    string packet = "E";
    uint32_t len = msg.size();
    vector<string> packets = fragmentPackets(packet, msg, lengthPacket, 3);
    return packets;
}

// Build the message with the protocol
vector<string> buildBroadcast(const string& sender, const string& msg) {
    string packet = "M";
    uint16_t slen = htons(sender.size());
    packet.append((char*)&slen, 2);
    packet += sender;
    vector<string> packets = fragmentPackets(packet, msg, lengthPacket, 3);
    return packets;
}

// Build the message to a specific client with the protocol
vector<string> buildToClient(const string& sender, const string& msg) {
    string packet = "T";
    uint16_t slen = htons(sender.size());
    packet.append((char*)&slen, 2);
    packet += sender;
    vector<string> packets = fragmentPackets(packet, msg, lengthPacket, 3);
    return packets;
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
    packet = completePacket(packet, lengthPacket);
    return packet;
}

// Function to build file message
vector<string> buildFile(const string& sender, const string& filename, const char* file_data, uint64_t file_size, int lengthPacket) {
    vector<string> packets;
    string header = "F";
    
    uint16_t slen = htons(sender.size());
    header.append((char*)&slen, 2);
    header += sender;
    
    uint32_t flen = filename.size();
    header.push_back((flen >> 16) & 0xFF);
    header.push_back((flen >> 8) & 0xFF);
    header.push_back(flen & 0xFF);
    
    header += filename;

    string fileDataStr(file_data, file_data + file_size);
    packets = fragmentPackets(header, fileDataStr, lengthPacket, 10);
    
    return packets;
}

// Function to build object message
vector<string> buildObject(const string& sender, const vector<char>& objectData) {
    string header = "O";
    
    // Sender
    uint16_t slen = htons(sender.size());
    header.append((char*)&slen, 2);
    header += sender;
    
    string ObjectData(objectData.begin(), objectData.end());
    vector<string> packets = fragmentPackets(header, ObjectData, lengthPacket, 4);
    
    return packets;
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
    packet = completePacket(packet, lengthPacket);
    return packet;
}

string buildGameResponse(const string& sender, bool accepted) {
    string packet = "j";
    uint16_t slen = htons(sender.size());
    packet.append((char*)&slen, 2);
    packet += sender;
    packet.push_back(accepted ? 'y' : 'n');
    packet = completePacket(packet, lengthPacket);
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
    packet = completePacket(packet, lengthPacket);
    return packet;
}

string buildGameResult(char result) {
    string packet = "W";
    packet.push_back(result);
    packet = completePacket(packet, lengthPacket);
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
            vector<string> errors = buildError("Nickname already taken");
            for(auto err:errors){
                cout << "Server sending error to " << nickname << ": " << formatProtocol(err) << endl;
                send(client_socket, err.c_str(), err.size(), 0);
            }
            close(client_socket);
            return;
        }
        clients[nickname] = client_socket;
    }

    cout << nickname << " connected" << endl;

    while (true) {

        char buffer[777];

        int r = recv(client_socket, buffer, 777, 0);
        if (r <= 0) break;

        string fragment(buffer,r);
        string completePacket;

        if (reconstructPacket(client_socket, fragment, completePacket)) {
            char type = completePacket[0];

            if (type == 'm') {
                int len = ((unsigned char)completePacket[1] << 16) |
                        ((unsigned char)completePacket[2] << 8) |
                        (unsigned char)completePacket[3];

                string message = completePacket.substr(4, len);
                cout << nickname << " received: " << formatProtocol(completePacket) << endl;

                vector<string> msgs = buildBroadcast(nickname, message);
                for (auto &msg : msgs) sendAll(msg, client_socket);
            }

            else if (type == 't') {
                uint16_t dlen;
                memcpy(&dlen, &completePacket[1], 2);
                dlen = ntohs(dlen);
                string dest = completePacket.substr(3, dlen);

                int mlen = ((unsigned char)completePacket[3 + dlen] << 16) |
                        ((unsigned char)completePacket[3 + dlen + 1] << 8) |
                        (unsigned char)completePacket[3 + dlen + 2];
                string message = completePacket.substr(3 + dlen + 3, mlen);

                cout << nickname << " received: " << formatProtocol(completePacket) << endl;

                vector<string> msgs = buildToClient(nickname, message);
                for (auto &msg : msgs) sendToClient(dest, msg);
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
                // [f][2 bytes dlen][dest][3 bytes flen][filename][10 bytes fsize][file data]
                uint16_t dlen;
                memcpy(&dlen, &completePacket[1], 2);
                dlen = ntohs(dlen);

                string dest = completePacket.substr(3, dlen);

                int dataOffset = 3 + dlen;
                int flen = ((unsigned char)completePacket[dataOffset] << 16) |
                        ((unsigned char)completePacket[dataOffset + 1] << 8) |
                        (unsigned char)completePacket[dataOffset + 2];

                string filename = completePacket.substr(dataOffset + 3, flen);

                uint64_t fsize = 0;
                int fsizeOffset = dataOffset + 3 + flen;
                for (int i = 0; i < 10; i++) {
                    fsize = (fsize << 8) | (unsigned char)completePacket[fsizeOffset + i];
                }

                string fileData = completePacket.substr(3 + dlen + 3 + flen + 10);

                cout << nickname << " received: " << formatProtocol(completePacket)<< endl;

                vector<string> packets = buildFile(nickname, filename, fileData.c_str(), fsize, lengthPacket);
                for (auto &packet : packets) sendToClient(dest, packet);
            }
            else if (type == 'o') {
                // [o][2 bytes dlen][dest][4 bytes objSize][object data]
                uint16_t dlen;
                memcpy(&dlen, &completePacket[1], 2);
                dlen = ntohs(dlen);

                string dest = completePacket.substr(3, dlen);

                uint32_t objSize;
                memcpy(&objSize, &completePacket[3 + dlen], 4);
                objSize = ntohl(objSize);

                vector<char> objectBuf(objSize);
                memcpy(objectBuf.data(), &completePacket[3 + dlen + 4], objSize);

                cout << nickname << " received: " << formatProtocol(completePacket) << endl;

                vector<string> msgs = buildObject(nickname, objectBuf);
                for (auto &msg : msgs) sendToClient(dest, msg);
            }
            else if (type == 'J') {
                uint16_t dlen;
                memcpy(&dlen, &completePacket[1], 2);
                dlen = ntohs(dlen);
                string dest = completePacket.substr(3, dlen);

                cout << nickname << " received: " << formatProtocol(completePacket) << endl;

                lock_guard<mutex> lock(clients_mutex);
                if (clients.count(dest)) {
                    string msg = buildGameRequest(nickname);
                    sendToClient(dest, msg);
                } else {
                    vector<string> errors = buildError("User not found");
                    for(auto err:errors){
                        send(client_socket, err.c_str(), err.size(), 0);
                    }
                }
            }
            else if (type == 'j') {
                uint16_t slen;
                memcpy(&slen, &completePacket[1], 2);
                slen = ntohs(slen);

                string sender = completePacket.substr(3, slen);
                char response = completePacket[3 + slen];

                cout << nickname << " received: " << formatProtocol(completePacket) << endl;

                string msg = buildGameResponse(nickname, response == 'y');
                sendToClient(sender, msg);

                if (response == 'y') {
                    lock_guard<mutex> lock(games_mutex);
                    auto key = make_pair(min(nickname, sender), max(nickname, sender));
                    initializeGame(activeGames[key], nickname, sender);

                    string boardPacket = buildBoard(activeGames[key].board, activeGames[key].currentPlayer);
                    sendToClient(nickname, boardPacket);
                    sendToClient(sender, boardPacket);
                }
            }
            else if (type == 'P') {
                uint32_t pos = ((unsigned char)completePacket[1] << 24) |
                            ((unsigned char)completePacket[2] << 16) |
                            ((unsigned char)completePacket[3] << 8) |
                            (unsigned char)completePacket[4];

                lock_guard<mutex> lock(games_mutex);
                Game* g = nullptr;
                string opponent;
                pair<string,string> key;

                cout << nickname << " received: " << formatProtocol(completePacket) << endl;

                for (auto& entry : activeGames) {
                    if ((entry.second.player1 == nickname || entry.second.player2 == nickname) && entry.second.gameActive) {
                        g = &entry.second;
                        opponent = (entry.second.player1 == nickname) ? entry.second.player2 : entry.second.player1;
                        key = entry.first;
                        break;
                    }
                }

                if (!g) return;
                if (g->currentPlayer != nickname) return;
                if (pos >= 9) return;

                if (g->board[pos] == ' ') {
                    g->board[pos] = (nickname == g->player1) ? 'X' : 'O';
                    char symbol = (nickname == g->player1) ? 'X' : 'O';

                    if (checkWinner(g->board, symbol)) {
                        string win = buildGameResult('1');
                        string lose = buildGameResult('0');
                        sendToClient(nickname, win);
                        sendToClient(opponent, lose);
                        activeGames.erase(key);
                    } else if (checkIsPositionUsed(g->board)) {
                        string draw = buildGameResult('2');
                        sendToClient(nickname, draw);
                        sendToClient(opponent, draw);
                        activeGames.erase(key);
                    } else {
                        g->currentPlayer = opponent;
                        string boardPacket = buildBoard(g->board, g->currentPlayer);
                        sendToClient(nickname, boardPacket);
                        sendToClient(opponent, boardPacket);
                    }
                }
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
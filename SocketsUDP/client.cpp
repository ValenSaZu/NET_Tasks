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
#include "sala.h"
#include "sala_serialized.h"
#include <map>

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

int lengthPacket = 777;

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
    packet = completePacket(packet, lengthPacket);
    cout << "Protocol sending: " << formatProtocol(packet) << endl;
    send(sock, packet.c_str(), packet.size(), 0);
}

void sendBroadcast(int sock, const string msg) {
    string header = "m";
    vector<string> packets = fragmentPackets(header, msg, lengthPacket, 3);
    for(auto packet:packets){
        cout << "Protocol sending: " << formatProtocol(packet) << endl;
        send(sock, packet.c_str(), packet.size(), 0);
    }
}

void sendToClient(int sock, const string dest, const string msg) {
    string header = "t";
    uint16_t dlen = htons(dest.size());
    header.append((char*)&dlen,2);
    header += dest;
    vector<string> packets = fragmentPackets(header, msg, lengthPacket, 3);
    for(auto packet: packets){
        cout << "Protocol sending: " << formatProtocol(packet) << endl;
        send(sock, packet.c_str(), packet.size(), 0);
    }
}

void requestList(int sock) {
    string packet = "l";
    packet = completePacket(packet, lengthPacket);
    cout << "Protocol sending: " << formatProtocol(packet) << endl;
    send(sock, packet.c_str(), packet.size(), 0);
}

void sendClose(int sock) {
    string packet = "x";
    packet = completePacket(packet, lengthPacket);
    cout << "Protocol sending: " << formatProtocol(packet) << endl;
    send(sock, packet.c_str(), packet.size(), 0);
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
    
    string header = "f";
    
    // nickname
    uint16_t dlen = htons(dest.size());
    header.append((char*)&dlen, 2);
    header += dest;
    
    // filename
    uint32_t flen = filename.size();
    header.push_back((flen >> 16) & 0xFF);
    header.push_back((flen >> 8) & 0xFF);
    header.push_back(flen & 0xFF);
    
    header += filename;

    string fileData(file_data.begin(), file_data.end());
    vector<string> packets = fragmentPackets(header, fileData, lengthPacket, 10);
    
    for(auto packet:packets){
        cout << "Protocol sending: " << formatProtocol(packet.substr(0, 777)) << endl;
        send(sock, packet.c_str(), packet.size(), 0);
    }
}

void sendObject(int sock, const string &dest, const Sala &sala) {
    string header;

    header.push_back('o');

    uint16_t dlen = htons(static_cast<uint16_t>(dest.size()));
    header.append(reinterpret_cast<char*>(&dlen), sizeof(dlen));
    header += dest;

    vector<char> objectContent = serializarSala(sala);

    string object_content(objectContent.begin(), objectContent.end());

    vector<string> packets = fragmentPackets(header, object_content, lengthPacket, 4);
    for(auto packet: packets){
        send(sock, packet.data(), packet.size(), 0);
    }
}

void sendGameRequest(int sock, const string& dest) {
    string packet = "J";
    uint16_t dlen = htons(dest.size());
    packet.append((char*)&dlen, 2);
    packet += dest;
    packet = completePacket(packet, lengthPacket);
    cout << "Protocol sending: " << formatProtocol(packet) << endl;
    send(sock, packet.c_str(), packet.size(), 0);
}

void sendGameResponse(int sock, const string& sender, bool accept) {
    string packet = "j";
    uint16_t slen = htons(sender.size());
    packet.append((char*)&slen, 2);
    packet += sender;
    packet.push_back(accept ? 'y' : 'n');
    packet = completePacket(packet, lengthPacket);
    cout << "Protocol sending: " << formatProtocol(packet) << endl;
    send(sock, packet.c_str(), packet.size(), 0);
}

void sendBoardPosition(int sock, int position) {
    string packet = "P";
    uint32_t pos = position;
    packet.push_back((pos >> 24) & 0xFF);
    packet.push_back((pos >> 16) & 0xFF);
    packet.push_back((pos >> 8) & 0xFF);
    packet.push_back(pos & 0xFF);
    packet = completePacket(packet, lengthPacket);
    cout << "Protocol sending: " << formatProtocol(packet) << endl;
    send(sock, packet.c_str(), packet.size(), 0);
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

// Receiver thread
void receiveMessages(int sock, const string& nickname) {
    char header[4];
    while (true) {
        char buffer[777];

        int r = recv(sock, buffer, 777, 0);
        if (r<=0) { cout << "Disconnected." << endl; break; }
        string fragment(buffer,r);
        string completePacket;

        if(reconstructPacket(sock, fragment, completePacket)){
            char type = completePacket[0];

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
                uint16_t slen;
                memcpy(&slen, &completePacket[1], 2);
                slen = ntohs(slen);

                string sender = completePacket.substr(3, slen);

                int mlen = ((unsigned char)completePacket[3 + slen] << 16) |
                        ((unsigned char)completePacket[3 + slen + 1] << 8) |
                        (unsigned char)completePacket[3 + slen + 2];

                string msg = completePacket.substr(3 + slen + 3, mlen);

                cout << "Protocol received: " << formatProtocol(completePacket) << endl;
                cout << "[Broadcast from " << sender << "] " << msg << endl;
            }
            else if (type=='T') {
                uint16_t slen;
                memcpy(&slen, &completePacket[1], 2);
                slen = ntohs(slen);

                string sender = completePacket.substr(3, slen);

                int mlen = ((unsigned char)completePacket[3 + slen] << 16) |
                        ((unsigned char)completePacket[3 + slen + 1] << 8) |
                        (unsigned char)completePacket[3 + slen + 2];

                string msg = completePacket.substr(3 + slen + 3, mlen);

                cout << "Protocol received: " << formatProtocol(completePacket) << endl;
                cout << "[Private from " << sender << "] " << msg << endl;
            }
            else if (type=='L') {
                uint16_t total_len;
                memcpy(&total_len, &completePacket[1], 2);
                total_len = ntohs(total_len);

                string listData = completePacket.substr(3, total_len);
                cout << "Protocol received: " << formatProtocol(completePacket) << endl;

                parseListResponse((char*)listData.data(), total_len);
            }
            else if (type=='X') {
                cout << "Protocol received: X" << endl;
                cout << "Server closed the connection. Goodbye!" << endl;
                break;
            }
            else if (type=='F') {
                uint16_t slen;
                memcpy(&slen, &completePacket[1], 2);
                slen = ntohs(slen);

                string sender = completePacket.substr(3, slen);

                int flen = ((unsigned char)completePacket[3 + slen] << 16) |
                        ((unsigned char)completePacket[3 + slen + 1] << 8) |
                        (unsigned char)completePacket[3 + slen + 2];

                string filename = completePacket.substr(3 + slen + 3, flen);

                uint64_t fsize = 0;
                for (int i = 0; i < 10; i++) {
                    fsize = (fsize << 8) | (unsigned char)completePacket[3 + slen + 3 + flen + i];
                }

                string fileData = completePacket.substr(3 + slen + 3 + flen + 10);
                cout << "Protocol received: " << formatProtocol(completePacket)<< endl;

                size_t dot_pos = filename.find_last_of(".");
                string new_filename = (dot_pos != string::npos)
                    ? filename.substr(0, dot_pos) + "_dest" + filename.substr(dot_pos)
                    : filename + "_dest";

                ofstream out_file(new_filename, ios::binary);
                if (out_file.is_open()) {
                    out_file.write(fileData.c_str(), fsize);
                    out_file.close();
                    cout << "[File received from " << sender << "] Saved as: "
                        << new_filename << " (" << fsize << " bytes)" << endl;
                } else {
                    cout << "[Error] Could not save file: " << new_filename << endl;
                }
            }
            else if (type == 'O') {
                uint16_t slen;
                memcpy(&slen, &completePacket[1], 2);
                slen = ntohs(slen);

                string sender = completePacket.substr(3, slen);

                uint32_t objSize;
                memcpy(&objSize, &completePacket[3 + slen], 4);
                objSize = ntohl(objSize);

                vector<char> objectBuf(objSize);
                memcpy(objectBuf.data(), &completePacket[3 + slen + 4], objSize);

                Sala sala = deserializeSala(objectBuf);

                cout << "Protocol received: " << formatProtocol(completePacket) << endl;

                cout << "Sala object received from: " << sender << endl;
                cout << "Chair: " << sala.silla.patas << " legs, "
                    << (sala.silla.conRespaldo ? "with backrest" : "without backrest") << endl;
                cout << "Sofa: capacity " << sala.sillon.capacidad << ", color " << sala.sillon.color << endl;
                cout << "Kitchen: " << (sala.cocina->electrica ? "electric" : "non-electric")
                    << ", " << sala.cocina->metrosCuadrados << " m²" << endl;
                cout << "n: " << sala.n << endl;
                cout << "Description: " << sala.descripcion << endl;

                delete sala.cocina;
            }
            else if (type == 'J') {
                uint16_t slen;
                memcpy(&slen, &completePacket[1], 2);
                slen = ntohs(slen);

                string sender = completePacket.substr(3, slen);
                cout << "Protocol received: " << formatProtocol(completePacket) << endl;
                cout << "[Game request from " << sender << "]\n";

                string response = getGameInput(sender + " is inviting you to play Tic Tac Toe\nDo you accept? (y/n): ");
                bool accept = (response == "y" || response == "Y" || response == "s" || response == "S");
                sendGameResponse(sock, sender, accept);

                if (accept) {
                    cout << "Starting game with " << sender << "..." << endl;
                } else {
                    cout << "Invitation declined." << endl;
                }
            }
            else if (type == 'j') {
                uint16_t slen;
                memcpy(&slen, &completePacket[1], 2);
                slen = ntohs(slen);

                string sender = completePacket.substr(3, slen);
                char response = completePacket[3 + slen];

                cout << "Protocol received: " << formatProtocol(completePacket) << endl;

                if (response == 'y')
                    cout << "[Game accepted by " << sender << "]\n";
                else
                    cout << "[Game rejected by " << sender << "]\n";
            }
            else if (type == 'B') {
                uint16_t board_len;
                memcpy(&board_len, &completePacket[1], 2);
                board_len = ntohs(board_len);

                vector<char> board(board_len);
                memcpy(board.data(), &completePacket[3], board_len);

                uint16_t player_len;
                memcpy(&player_len, &completePacket[3 + board_len], 2);
                player_len = ntohs(player_len);

                string currentPlayer = completePacket.substr(3 + board_len + 2, player_len);

                cout << "Protocol received: " << formatProtocol(completePacket) << endl;

                if (currentPlayer == nickname) {
                    string move = getBoardInput("Select a position (0-8): ");
                    
                    try {
                        int position = stoi(move);
                        if (position >= 0 && position <= 8) {
                            sendBoardPosition(sock, position);
                        } else {
                            cout << "Invalid position. Must be between 0 and 8." << endl;
                        }
                    } catch (...) {
                        cout << "Invalid input." << endl;
                    }
                } else {
                    cout << "Please wait for " << currentPlayer << " to make a move..." << endl;
                }
            }
            else if (type == 'W') {
                char result = completePacket[1];
                cout << "Protocol received: " << formatProtocol(completePacket) << endl;
                if (result == '1') {
                    cout << "You win!" << endl;
                } else if (result == '0') {
                    cout << "You lose!" << endl;
                } else if (result == '2') {
                    cout << "It's a tie!" << endl;
                } else if (result == '3') {
                    cout << "Game ended: opponent disconnected" << endl;
                }
            }
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

    thread t(receiveMessages,sock,nickname);

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
                
                sendObject(sock, dest, sala);
                
                // Clean up memory
                delete sala.cocina;
            } else {
                cout << "Usage: /object destination" << endl;
            }
        }
        else if (line.rfind("/play ", 0) == 0) {
            if (line.length() > 6) {
                string dest = line.substr(6);
                sendGameRequest(sock, dest);
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
#include <iostream>
#include <sstream>
#include <cstring>
using namespace std;

string hexToText(const string& hex) {
    string text;
    stringstream ss(hex);
    string byte;
    while (ss >> byte) {
        int value = stoi(byte, nullptr, 16);
        text += (char)value;
    }
    return text;
}

int main() {
    string hexInput;
    cout << "Ingresa hex (ej: 48 6F 6C 61): ";
    getline(cin, hexInput);
    cout << "Texto: " << hexToText(hexInput) << endl;
    return 0;
}
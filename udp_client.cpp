#include <iostream>
#include <string>
#include <cstring>
#include <thread>
#include <atomic>
#include <chrono>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
    typedef int socklen_t;
#else
    #include <sys/socket.h>
    #include <arpa/inet.h>
    #include <netinet/in.h>
    #include <unistd.h>
    #define SOCKET int
    #define INVALID_SOCKET -1
    #define SOCKET_ERROR -1
    #define closesocket close
#endif

class UDPClient {
private:
    SOCKET clientSocket;
    struct sockaddr_in serverAddr;
    std::atomic<bool> running;
    std::thread receiveThread;
    std::string clientName;
    
    void receiveLoop() {
        const int bufferSize = 1024;
        char buffer[bufferSize];
        sockaddr_in senderAddr;
        socklen_t senderAddrLen = sizeof(senderAddr);
        
        while (running) {
            // Receive data
            memset(buffer, 0, bufferSize);
            int bytesReceived = recvfrom(clientSocket, buffer, bufferSize - 1, 0, 
                                        (struct sockaddr*)&senderAddr, &senderAddrLen);
            
            if (bytesReceived > 0) {
                buffer[bytesReceived] = '\0';
                std::string message(buffer);
                std::cout << "[" << clientName << "] Received: " << message << std::endl;
            }
        }
    }
    
public:
    UDPClient(const std::string& serverIP, int serverPort, const std::string& name) 
        : running(false), clientName(name) {
#ifdef _WIN32
        WSADATA wsaData;
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
            throw std::runtime_error("Failed to initialize Winsock");
        }
#endif

        // Create socket
        clientSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (clientSocket == INVALID_SOCKET) {
            throw std::runtime_error("Failed to create socket");
        }
        
        // Configure server address
        memset(&serverAddr, 0, sizeof(serverAddr));
        serverAddr.sin_family = AF_INET;
        serverAddr.sin_port = htons(serverPort);
        
        // Convert IP address from text to binary
        if (inet_pton(AF_INET, serverIP.c_str(), &serverAddr.sin_addr) <= 0) {
            closesocket(clientSocket);
            throw std::runtime_error("Invalid address / Address not supported");
        }
        
        std::cout << "UDP Client " << clientName << " initialized. Connected to " 
                  << serverIP << ":" << serverPort << std::endl;
    }
    
    ~UDPClient() {
        stop();
        closesocket(clientSocket);
#ifdef _WIN32
        WSACleanup();
#endif
    }
    
    void start() {
        if (!running) {
            running = true;
            receiveThread = std::thread(&UDPClient::receiveLoop, this);
            std::cout << "Client " << clientName << " started" << std::endl;
        }
    }
    
    void stop() {
        if (running) {
            running = false;
            if (receiveThread.joinable()) {
                receiveThread.join();
            }
            std::cout << "Client " << clientName << " stopped" << std::endl;
        }
    }
    
    void sendMessage(const std::string& message) {
        std::string fullMessage = clientName + ": " + message;
        sendto(clientSocket, fullMessage.c_str(), fullMessage.length(), 0,
               (struct sockaddr*)&serverAddr, sizeof(serverAddr));
    }
};

int main(int argc, char* argv[]) {
    std::string serverIP = "127.0.0.1";
    int serverPort = 8888;
    std::string clientName = "Client1";
    
    if (argc > 1) {
        serverIP = argv[1];
    }
    
    if (argc > 2) {
        serverPort = std::stoi(argv[2]);
    }
    
    if (argc > 3) {
        clientName = argv[3];
    }
    
    try {
        UDPClient client(serverIP, serverPort, clientName);
        client.start();
        
        std::cout << "Type messages to send (or 'exit' to quit):" << std::endl;
        std::string message;
        while (true) {
            std::getline(std::cin, message);
            if (message == "exit") {
                break;
            }
            
            client.sendMessage(message);
        }
        
        client.stop();
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}
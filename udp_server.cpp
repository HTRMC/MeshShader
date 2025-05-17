#include <iostream>
#include <string>
#include <cstring>
#include <vector>
#include <unordered_map>
#include <thread>
#include <mutex>
#include <atomic>

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

class UDPServer {
private:
    SOCKET serverSocket;
    struct sockaddr_in serverAddr;
    std::atomic<bool> running;
    std::thread receiveThread;
    mutable std::mutex clientsMutex;
    
    // Store client information (IP:Port as key)
    std::unordered_map<std::string, sockaddr_in> clients;
    
    std::string getClientKey(const sockaddr_in& addr) {
        char ipStr[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &(addr.sin_addr), ipStr, INET_ADDRSTRLEN);
        return std::string(ipStr) + ":" + std::to_string(ntohs(addr.sin_port));
    }
    
    void receiveLoop() {
        const int bufferSize = 1024;
        char buffer[bufferSize];
        sockaddr_in clientAddr;
        socklen_t clientAddrLen = sizeof(clientAddr);
        
        while (running) {
            // Receive data
            memset(buffer, 0, bufferSize);
            int bytesReceived = recvfrom(serverSocket, buffer, bufferSize - 1, 0, 
                                        (struct sockaddr*)&clientAddr, &clientAddrLen);
            
            if (bytesReceived > 0) {
                buffer[bytesReceived] = '\0';
                
                // Register client if new
                std::string clientKey = getClientKey(clientAddr);
                {
                    std::lock_guard<std::mutex> lock(clientsMutex);
                    if (clients.find(clientKey) == clients.end()) {
                        clients[clientKey] = clientAddr;
                        std::cout << "New client connected: " << clientKey << std::endl;
                    }
                }
                
                // Process the message
                std::string message(buffer);
                std::cout << "Received from " << clientKey << ": " << message << std::endl;
                
                // Echo back to the sender
                std::string response = "Server received: " + message;
                sendto(serverSocket, response.c_str(), response.length(), 0,
                       (struct sockaddr*)&clientAddr, clientAddrLen);
                
                // Broadcast message to all other clients
                broadcastMessage(clientKey, message);
            }
        }
    }
    
public:
    UDPServer(int port) : running(false) {
#ifdef _WIN32
        WSADATA wsaData;
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
            throw std::runtime_error("Failed to initialize Winsock");
        }
#endif

        // Create socket
        serverSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (serverSocket == INVALID_SOCKET) {
            throw std::runtime_error("Failed to create socket");
        }
        
        // Configure server address
        memset(&serverAddr, 0, sizeof(serverAddr));
        serverAddr.sin_family = AF_INET;
        serverAddr.sin_addr.s_addr = INADDR_ANY;
        serverAddr.sin_port = htons(port);
        
        // Bind the socket
        if (bind(serverSocket, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
            closesocket(serverSocket);
            throw std::runtime_error("Failed to bind socket");
        }
        
        std::cout << "UDP Server initialized on port " << port << std::endl;
    }
    
    ~UDPServer() {
        stop();
        closesocket(serverSocket);
#ifdef _WIN32
        WSACleanup();
#endif
    }
    
    void start() {
        if (!running) {
            running = true;
            receiveThread = std::thread(&UDPServer::receiveLoop, this);
            std::cout << "Server started" << std::endl;
        }
    }
    
    void stop() {
        if (running) {
            running = false;
            if (receiveThread.joinable()) {
                receiveThread.join();
            }
            std::cout << "Server stopped" << std::endl;
        }
    }
    
    void broadcastMessage(const std::string& senderKey, const std::string& message) {
        std::string broadcastMsg = "Broadcast from " + senderKey + ": " + message;
        
        std::lock_guard<std::mutex> lock(clientsMutex);
        for (const auto& client : clients) {
            // Don't send back to the original sender
            if (client.first != senderKey) {
                sendto(serverSocket, broadcastMsg.c_str(), broadcastMsg.length(), 0,
                      (struct sockaddr*)&client.second, sizeof(client.second));
            }
        }
    }
    
    size_t getClientCount() const {
        std::lock_guard<std::mutex> lock(clientsMutex);
        return clients.size();
    }
};

int main(int argc, char* argv[]) {
    int port = 42069;
    if (argc > 1) {
        port = std::stoi(argv[1]);
    }
    
    try {
        UDPServer server(port);
        server.start();
        
        std::cout << "Server running. Press Enter to stop..." << std::endl;
        std::cin.get();
        
        server.stop();
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}
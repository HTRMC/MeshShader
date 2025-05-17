#include <iostream>
#include <string>
#include <cstring>
#include <unordered_map>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <random>
#include <glm/glm.hpp>

#include "game_protocol.h"

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

// Default port changed to 42069
const int DEFAULT_PORT = 42069;

class GameServer {
private:
    SOCKET serverSocket;
    struct sockaddr_in serverAddr;
    std::atomic<bool> running;
    std::thread receiveThread;
    std::thread updateThread;
    mutable std::mutex playersMutex;
    
    // Game state
    PlayerState players[MAX_PLAYERS];
    std::unordered_map<std::string, uint32_t> clientToPlayerMap; // Maps client address to player ID
    
    // Player ID counter
    uint32_t nextPlayerId = 1;
    
    // Random number generator for color assignment
    std::mt19937 rng;
    std::uniform_real_distribution<float> colorDist;
    
    std::string getClientKey(const sockaddr_in& addr) {
        char ipStr[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &(addr.sin_addr), ipStr, INET_ADDRSTRLEN);
        return std::string(ipStr) + ":" + std::to_string(ntohs(addr.sin_port));
    }
    
    glm::vec3 generateRandomColor() {
        return glm::vec3(
            colorDist(rng),
            colorDist(rng),
            colorDist(rng)
        );
    }
    
    int findAvailablePlayerSlot() {
        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (!players[i].active) {
                return i;
            }
        }
        return -1; // No slots available
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
                // Get client identifier
                std::string clientKey = getClientKey(clientAddr);
                
                // Deserialize the message
                NetworkMessage msg = deserializeMessage(buffer, bytesReceived);
                
                // Process based on message type
                switch (msg.type) {
                    case MessageType::CONNECT:
                        handleConnect(clientKey, clientAddr);
                        break;
                        
                    case MessageType::DISCONNECT:
                        handleDisconnect(clientKey);
                        break;
                        
                    case MessageType::POSITION_UPDATE:
                        handlePositionUpdate(clientKey, msg);
                        break;
                        
                    default:
                        // Ignore unknown message types
                        break;
                }
            }
        }
    }
    
    void handleConnect(const std::string& clientKey, const sockaddr_in& clientAddr) {
        std::lock_guard<std::mutex> lock(playersMutex);
        
        // Check if client is already connected
        if (clientToPlayerMap.find(clientKey) != clientToPlayerMap.end()) {
            return; // Already connected
        }
        
        // Find available player slot
        int playerSlot = findAvailablePlayerSlot();
        if (playerSlot == -1) {
            std::cout << "Server full, rejecting client: " << clientKey << std::endl;
            return;
        }
        
        // Assign player ID
        uint32_t playerId = nextPlayerId++;
        clientToPlayerMap[clientKey] = playerId;
        
        // Initialize player state
        players[playerSlot].active = true;
        players[playerSlot].position = glm::vec3(0.0f, 0.0f, (float)(playerSlot * 2)); // Space them out
        players[playerSlot].color = generateRandomColor();
        
        std::cout << "New player connected: " << clientKey << " (Player " << playerId << ")" << std::endl;
        
        // Send acknowledgment to client
        std::string response = serializeConnectMessage(playerId);
        sendto(serverSocket, response.c_str(), response.length(), 0,
               (struct sockaddr*)&clientAddr, sizeof(clientAddr));
        
        // Broadcast full state to all clients
        broadcastGameState();
    }
    
    void handleDisconnect(const std::string& clientKey) {
        std::lock_guard<std::mutex> lock(playersMutex);
        
        auto it = clientToPlayerMap.find(clientKey);
        if (it == clientToPlayerMap.end()) {
            return; // Client not found
        }
        
        uint32_t playerId = it->second;
        
        // Find player slot
        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (players[i].active && i == playerId - 1) {
                players[i].active = false;
                break;
            }
        }
        
        clientToPlayerMap.erase(it);
        std::cout << "Player disconnected: " << clientKey << " (Player " << playerId << ")" << std::endl;
        
        // Broadcast updated state
        broadcastGameState();
    }
    
    void handlePositionUpdate(const std::string& clientKey, const NetworkMessage& msg) {
        std::lock_guard<std::mutex> lock(playersMutex);
        
        auto it = clientToPlayerMap.find(clientKey);
        if (it == clientToPlayerMap.end()) {
            return; // Client not found
        }
        
        uint32_t playerId = it->second;
        
        // Update player position
        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (players[i].active && i == playerId - 1) {
                players[i].position.x = msg.payload.position.x;
                players[i].position.y = msg.payload.position.y;
                players[i].position.z = msg.payload.position.z;
                break;
            }
        }
        
        // We don't need to broadcast immediately after each position update
        // The update loop will handle broadcasting at regular intervals
    }
    
    void updateLoop() {
        using namespace std::chrono;
        
        // Update rate: 20 times per second
        const milliseconds updateInterval(50);
        
        auto nextUpdate = steady_clock::now();
        
        while (running) {
            // Sleep until next update time
            std::this_thread::sleep_until(nextUpdate);
            nextUpdate += updateInterval;
            
            // Broadcast game state
            broadcastGameState();
        }
    }
    
    void broadcastGameState() {
        std::lock_guard<std::mutex> lock(playersMutex);
        
        // Serialize full game state
        std::string stateMsg = serializeFullState(players);
        
        // Send to all connected clients
        for (const auto& client : clientToPlayerMap) {
            // Extract IP and port from client key
            size_t separatorPos = client.first.find(':');
            if (separatorPos == std::string::npos) continue;
            
            std::string ip = client.first.substr(0, separatorPos);
            int port = std::stoi(client.first.substr(separatorPos + 1));
            
            // Create address structure
            sockaddr_in clientAddr;
            memset(&clientAddr, 0, sizeof(clientAddr));
            clientAddr.sin_family = AF_INET;
            clientAddr.sin_port = htons(port);
            inet_pton(AF_INET, ip.c_str(), &clientAddr.sin_addr);
            
            // Send game state
            sendto(serverSocket, stateMsg.c_str(), stateMsg.length(), 0,
                  (struct sockaddr*)&clientAddr, sizeof(clientAddr));
        }
    }
    
public:
    GameServer(int port = DEFAULT_PORT) : running(false), colorDist(0.2f, 1.0f) {
        // Initialize random number generator
        std::random_device rd;
        rng = std::mt19937(rd());
        
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
        
        // Initialize player states
        for (int i = 0; i < MAX_PLAYERS; i++) {
            players[i].active = false;
        }
        
        std::cout << "Game Server initialized on port " << port << std::endl;
    }
    
    ~GameServer() {
        stop();
        closesocket(serverSocket);
#ifdef _WIN32
        WSACleanup();
#endif
    }
    
    void start() {
        if (!running) {
            running = true;
            receiveThread = std::thread(&GameServer::receiveLoop, this);
            updateThread = std::thread(&GameServer::updateLoop, this);
            std::cout << "Game Server started" << std::endl;
        }
    }
    
    void stop() {
        if (running) {
            running = false;
            if (receiveThread.joinable()) {
                receiveThread.join();
            }
            if (updateThread.joinable()) {
                updateThread.join();
            }
            std::cout << "Game Server stopped" << std::endl;
        }
    }
    
    size_t getPlayerCount() const {
        std::lock_guard<std::mutex> lock(playersMutex);
        return clientToPlayerMap.size();
    }
};

int main(int argc, char* argv[]) {
    int port = DEFAULT_PORT;
    if (argc > 1) {
        port = std::stoi(argv[1]);
    }
    
    try {
        GameServer server(port);
        server.start();
        
        std::cout << "Game Server running on port " << port << ". Press Enter to stop..." << std::endl;
        std::cin.get();
        
        server.stop();
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}
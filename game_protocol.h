#pragma once

#include <cstdint>
#include <string>
#include <glm/glm.hpp>

// Define message types for our game protocol
enum class MessageType : uint8_t {
    CONNECT = 1,        // Client connects to server
    DISCONNECT = 2,     // Client disconnects from server
    POSITION_UPDATE = 3, // Client sends position update
    FULL_STATE = 4      // Server sends full game state
};

// Maximum number of players supported
constexpr int MAX_PLAYERS = 16;

// Basic player state
struct PlayerState {
    bool active = false;
    glm::vec3 position = glm::vec3(0.0f);
    glm::vec3 color = glm::vec3(1.0f);
};

// Message structure for serialization
struct NetworkMessage {
    MessageType type;
    uint32_t clientId;
    
    union PayloadData {
        struct PositionData {
            float x, y, z;
        } position;
        
        struct FullStateData {
            uint8_t playerCount;
            bool playerActive[MAX_PLAYERS];
            float positions[MAX_PLAYERS * 3]; // x,y,z for each player
            float colors[MAX_PLAYERS * 3];    // r,g,b for each player
        } fullState;
    } payload;
};

// Helper functions for serialization
std::string serializeConnectMessage(uint32_t clientId);
std::string serializeDisconnectMessage(uint32_t clientId);
std::string serializePositionUpdate(uint32_t clientId, const glm::vec3& position);
std::string serializeFullState(const PlayerState players[MAX_PLAYERS]);

// Helper functions for deserialization
NetworkMessage deserializeMessage(const char* data, size_t length);

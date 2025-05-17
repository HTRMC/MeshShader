#include "game_protocol.h"
#include <cstring>

// Serialize a connect message
std::string serializeConnectMessage(uint32_t clientId) {
    NetworkMessage msg;
    msg.type = MessageType::CONNECT;
    msg.clientId = clientId;
    
    return std::string(reinterpret_cast<char*>(&msg), sizeof(MessageType) + sizeof(uint32_t));
}

// Serialize a disconnect message
std::string serializeDisconnectMessage(uint32_t clientId) {
    NetworkMessage msg;
    msg.type = MessageType::DISCONNECT;
    msg.clientId = clientId;
    
    return std::string(reinterpret_cast<char*>(&msg), sizeof(MessageType) + sizeof(uint32_t));
}

// Serialize position update
std::string serializePositionUpdate(uint32_t clientId, const glm::vec3& position) {
    NetworkMessage msg;
    msg.type = MessageType::POSITION_UPDATE;
    msg.clientId = clientId;
    msg.payload.position.x = position.x;
    msg.payload.position.y = position.y;
    msg.payload.position.z = position.z;
    
    return std::string(reinterpret_cast<char*>(&msg), 
                       sizeof(MessageType) + sizeof(uint32_t) + sizeof(NetworkMessage::PayloadData::PositionData));
}

// Serialize full game state
std::string serializeFullState(const PlayerState players[MAX_PLAYERS]) {
    NetworkMessage msg;
    msg.type = MessageType::FULL_STATE;
    msg.clientId = 0; // Server message
    
    // Count active players
    uint8_t playerCount = 0;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (players[i].active) {
            playerCount++;
        }
    }
    
    msg.payload.fullState.playerCount = playerCount;
    
    // Fill in player data
    for (int i = 0; i < MAX_PLAYERS; i++) {
        msg.payload.fullState.playerActive[i] = players[i].active;
        
        // Position
        msg.payload.fullState.positions[i*3] = players[i].position.x;
        msg.payload.fullState.positions[i*3+1] = players[i].position.y;
        msg.payload.fullState.positions[i*3+2] = players[i].position.z;
        
        // Color
        msg.payload.fullState.colors[i*3] = players[i].color.r;
        msg.payload.fullState.colors[i*3+1] = players[i].color.g;
        msg.payload.fullState.colors[i*3+2] = players[i].color.b;
    }
    
    return std::string(reinterpret_cast<char*>(&msg), 
                       sizeof(MessageType) + sizeof(uint32_t) + sizeof(NetworkMessage::PayloadData::FullStateData));
}

// Deserialize message
NetworkMessage deserializeMessage(const char* data, size_t length) {
    NetworkMessage msg;
    
    if (length < sizeof(MessageType) + sizeof(uint32_t)) {
        // Invalid message, return default
        return msg;
    }
    
    // Copy the message type and client ID
    std::memcpy(&msg.type, data, sizeof(MessageType));
    std::memcpy(&msg.clientId, data + sizeof(MessageType), sizeof(uint32_t));
    
    // Extract payload based on message type
    switch (msg.type) {
        case MessageType::POSITION_UPDATE:
            if (length >= sizeof(MessageType) + sizeof(uint32_t) + sizeof(NetworkMessage::PayloadData::PositionData)) {
                std::memcpy(&msg.payload.position, 
                           data + sizeof(MessageType) + sizeof(uint32_t),
                           sizeof(NetworkMessage::PayloadData::PositionData));
            }
            break;
            
        case MessageType::FULL_STATE:
            if (length >= sizeof(MessageType) + sizeof(uint32_t) + sizeof(NetworkMessage::PayloadData::FullStateData)) {
                std::memcpy(&msg.payload.fullState,
                           data + sizeof(MessageType) + sizeof(uint32_t),
                           sizeof(NetworkMessage::PayloadData::FullStateData));
            }
            break;
            
        default:
            // No additional payload needed for CONNECT and DISCONNECT
            break;
    }
    
    return msg;
}

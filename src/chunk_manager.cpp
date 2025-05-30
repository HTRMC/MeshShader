#include "chunk_manager.h"
#include "logger.h"
#include <algorithm>

namespace Zerith {

ChunkManager::ChunkManager() {
    m_meshGenerator = std::make_unique<ChunkMeshGenerator>();
    m_terrainGenerator = std::make_unique<TerrainGenerator>();
    
    // Create worker threads for chunk loading
    unsigned int numThreads = std::max(1u, std::thread::hardware_concurrency() / 2);
    for (unsigned int i = 0; i < numThreads; ++i) {
        m_workerThreads.emplace_back(&ChunkManager::chunkWorkerThread, this);
    }
    
    LOG_INFO("ChunkManager initialized with render distance %d and %d worker threads", m_renderDistance, numThreads);
}

ChunkManager::~ChunkManager() {
    // Signal shutdown
    m_shutdown = true;
    m_queueCondition.notify_all();
    
    // Wait for all worker threads to finish
    for (auto& thread : m_workerThreads) {
        if (thread.joinable()) {
            thread.join();
        }
    }
}

void ChunkManager::updateLoadedChunks(const glm::vec3& playerPosition) {
    // Process any completed chunks first
    processCompletedChunks();
    
    // Get the chunk position the player is in
    glm::ivec3 playerChunkPos = worldToChunkPos(playerPosition);
    
    // Only update if player moved to a different chunk
    if (playerChunkPos == m_lastPlayerChunkPos) {
        return;
    }
    
    m_lastPlayerChunkPos = playerChunkPos;
    bool chunksChanged = false;
    
    // Find chunks to unload (outside render distance)
    std::vector<glm::ivec3> chunksToUnload;
    {
        std::lock_guard<std::mutex> lock(m_chunksMutex);
        for (const auto& [chunkPos, chunk] : m_chunks) {
            if (!isChunkInRange(chunkPos, playerChunkPos)) {
                chunksToUnload.push_back(chunkPos);
            }
        }
    }
    
    // Unload distant chunks
    for (const auto& chunkPos : chunksToUnload) {
        unloadChunk(chunkPos);
        chunksChanged = true;
    }
    
    // Load chunks within render distance asynchronously
    for (int dx = -m_renderDistance; dx <= m_renderDistance; ++dx) {
        for (int dy = -2; dy <= 2; ++dy) { // Limit vertical range
            for (int dz = -m_renderDistance; dz <= m_renderDistance; ++dz) {
                glm::ivec3 chunkPos = playerChunkPos + glm::ivec3(dx, dy, dz);
                
                // Skip if outside render distance
                if (!isChunkInRange(chunkPos, playerChunkPos)) {
                    continue;
                }
                
                // Check if already loaded or loading
                {
                    std::lock_guard<std::mutex> lock(m_chunksMutex);
                    if (m_chunks.find(chunkPos) != m_chunks.end() || 
                        m_loadingChunks.find(chunkPos) != m_loadingChunks.end()) {
                        continue;
                    }
                }
                
                // Calculate priority based on distance to player
                glm::ivec3 diff = chunkPos - playerChunkPos;
                int distance = diff.x * diff.x + diff.y * diff.y + diff.z * diff.z;
                int priority = 1000 - distance; // Closer chunks get higher priority
                
                loadChunkAsync(chunkPos, priority);
            }
        }
    }
    
    // Mark for rebuild if chunks changed
    if (chunksChanged) {
        m_needsRebuild = true;
        rebuildAllFaceInstances();
        // Only print chunk updates occasionally for performance
        static int updateCount = 0;
        if (++updateCount % 10 == 0) {
            std::lock_guard<std::mutex> lock(m_chunksMutex);
            LOG_DEBUG("Chunks loaded: %zu, Total faces: %zu", m_chunks.size(), m_allFaceInstances.size());
        }
    }
}

void ChunkManager::rebuildAllFaceInstances() {
    // Build new vector instead of clearing and rebuilding
    std::vector<BlockbenchInstanceGenerator::FaceInstance> newFaceInstances;
    
    // Lock once for the entire operation - more efficient
    {
        std::lock_guard<std::mutex> lock(m_chunksMutex);
        
        // Calculate total size while we have the lock
        size_t totalFaces = 0;
        for (const auto& [chunkPos, faces] : m_chunkMeshes) {
            totalFaces += faces.size();
        }
        
        // Reserve space for better performance
        newFaceInstances.reserve(totalFaces);
        
        // Collect all face instances from all chunks
        for (const auto& [chunkPos, faces] : m_chunkMeshes) {
            newFaceInstances.insert(newFaceInstances.end(), faces.begin(), faces.end());
        }
    }
    
    // Move the new vector to replace the old one (avoids unnecessary deallocations)
    m_allFaceInstances = std::move(newFaceInstances);
    
    // Rebuild indirect commands for proper GPU indirect drawing
    rebuildIndirectCommands();
}

void ChunkManager::rebuildIndirectCommands() {
    m_indirectDrawManager.clear();
    
    // Build chunk data for all chunks
    uint32_t currentFaceIndex = 0;
    uint32_t chunkCount = 0;
    
    for (const auto& [chunkPos, faces] : m_chunkMeshes) {
        if (faces.empty()) continue;
        
        // Calculate chunk bounds in world space
        glm::vec3 chunkWorldPos = glm::vec3(chunkPos) * float(Chunk::CHUNK_SIZE);
        float minBounds[3] = {chunkWorldPos.x, chunkWorldPos.y, chunkWorldPos.z};
        float maxBounds[3] = {
            chunkWorldPos.x + Chunk::CHUNK_SIZE,
            chunkWorldPos.y + Chunk::CHUNK_SIZE,
            chunkWorldPos.z + Chunk::CHUNK_SIZE
        };
        
        // Add chunk data (but not individual draw commands)
        m_indirectDrawManager.addChunkData(
            static_cast<uint32_t>(faces.size()),
            minBounds,
            maxBounds,
            currentFaceIndex
        );
        
        currentFaceIndex += static_cast<uint32_t>(faces.size());
        chunkCount++;
    }
    
    // Create a single indirect command that launches one task workgroup per chunk
    if (chunkCount > 0) {
        m_indirectDrawManager.setSingleDrawCommand(chunkCount, 1, 1);
    }
}

Chunk* ChunkManager::getChunk(const glm::ivec3& chunkPos) {
    std::lock_guard<std::mutex> lock(m_chunksMutex);
    auto it = m_chunks.find(chunkPos);
    return (it != m_chunks.end()) ? it->second.get() : nullptr;
}

BlockType ChunkManager::getBlock(const glm::vec3& worldPos) const {
    glm::ivec3 chunkPos = worldToChunkPos(worldPos);
    
    std::lock_guard<std::mutex> lock(m_chunksMutex);
    auto it = m_chunks.find(chunkPos);
    if (it == m_chunks.end()) {
        return BlockType::AIR;
    }
    
    return it->second->getBlockWorld(worldPos);
}

void ChunkManager::setBlock(const glm::vec3& worldPos, BlockType type) {
    glm::ivec3 chunkPos = worldToChunkPos(worldPos);
    
    glm::ivec3 localPos;
    {
        std::lock_guard<std::mutex> lock(m_chunksMutex);
        auto it = m_chunks.find(chunkPos);
        if (it == m_chunks.end()) {
            return;
        }
        
        localPos = it->second->worldToLocal(worldPos);
        it->second->setBlock(localPos.x, localPos.y, localPos.z, type);
    }
    
    // Regenerate mesh for this chunk
    regenerateChunkMesh(chunkPos);
    
    // Also regenerate neighboring chunks if block is on edge
    if (localPos.x == 0) regenerateChunkMesh(chunkPos + glm::ivec3(-1, 0, 0));
    if (localPos.x == Chunk::CHUNK_SIZE - 1) regenerateChunkMesh(chunkPos + glm::ivec3(1, 0, 0));
    if (localPos.y == 0) regenerateChunkMesh(chunkPos + glm::ivec3(0, -1, 0));
    if (localPos.y == Chunk::CHUNK_SIZE - 1) regenerateChunkMesh(chunkPos + glm::ivec3(0, 1, 0));
    if (localPos.z == 0) regenerateChunkMesh(chunkPos + glm::ivec3(0, 0, -1));
    if (localPos.z == Chunk::CHUNK_SIZE - 1) regenerateChunkMesh(chunkPos + glm::ivec3(0, 0, 1));
    
    // Mark for rebuild and rebuild the combined face instances
    m_needsRebuild = true;
    rebuildAllFaceInstances();
}

size_t ChunkManager::getTotalFaceCount() const {
    std::lock_guard<std::mutex> lock(m_chunksMutex);
    size_t total = 0;
    for (const auto& [chunkPos, faces] : m_chunkMeshes) {
        total += faces.size();
    }
    return total;
}

glm::ivec3 ChunkManager::worldToChunkPos(const glm::vec3& worldPos) const {
    return glm::ivec3(
        static_cast<int>(std::floor(worldPos.x / Chunk::CHUNK_SIZE)),
        static_cast<int>(std::floor(worldPos.y / Chunk::CHUNK_SIZE)),
        static_cast<int>(std::floor(worldPos.z / Chunk::CHUNK_SIZE))
    );
}

void ChunkManager::loadChunk(const glm::ivec3& chunkPos) {
    LOG_TRACE("Loading chunk at (%d, %d, %d)", chunkPos.x, chunkPos.y, chunkPos.z);
    // Create new chunk
    auto chunk = std::make_unique<Chunk>(chunkPos);
    
    // Generate terrain
    generateTerrain(*chunk);
    
    // Get neighboring chunks for proper face culling
    const Chunk* neighborXMinus = nullptr;
    const Chunk* neighborXPlus = nullptr;
    const Chunk* neighborYMinus = nullptr;
    const Chunk* neighborYPlus = nullptr;
    const Chunk* neighborZMinus = nullptr;
    const Chunk* neighborZPlus = nullptr;
    
    auto neighborIt = m_chunks.find(chunkPos + glm::ivec3(-1, 0, 0));
    if (neighborIt != m_chunks.end()) neighborXMinus = neighborIt->second.get();
    
    neighborIt = m_chunks.find(chunkPos + glm::ivec3(1, 0, 0));
    if (neighborIt != m_chunks.end()) neighborXPlus = neighborIt->second.get();
    
    neighborIt = m_chunks.find(chunkPos + glm::ivec3(0, -1, 0));
    if (neighborIt != m_chunks.end()) neighborYMinus = neighborIt->second.get();
    
    neighborIt = m_chunks.find(chunkPos + glm::ivec3(0, 1, 0));
    if (neighborIt != m_chunks.end()) neighborYPlus = neighborIt->second.get();
    
    neighborIt = m_chunks.find(chunkPos + glm::ivec3(0, 0, -1));
    if (neighborIt != m_chunks.end()) neighborZMinus = neighborIt->second.get();
    
    neighborIt = m_chunks.find(chunkPos + glm::ivec3(0, 0, 1));
    if (neighborIt != m_chunks.end()) neighborZPlus = neighborIt->second.get();
    
    // Generate mesh with neighbor awareness
    auto faces = m_meshGenerator->generateChunkMeshWithNeighbors(*chunk,
                                                                  neighborXMinus, neighborXPlus,
                                                                  neighborYMinus, neighborYPlus,
                                                                  neighborZMinus, neighborZPlus);
    m_chunkMeshes[chunkPos] = std::move(faces);
    
    // Store chunk
    m_chunks[chunkPos] = std::move(chunk);
}

void ChunkManager::unloadChunk(const glm::ivec3& chunkPos) {
    LOG_TRACE("Unloading chunk at (%d, %d, %d)", chunkPos.x, chunkPos.y, chunkPos.z);
    std::lock_guard<std::mutex> lock(m_chunksMutex);
    m_chunks.erase(chunkPos);
    m_chunkMeshes.erase(chunkPos);
}

void ChunkManager::generateTerrain(Chunk& chunk) {
    m_terrainGenerator->generateTerrain(chunk);
}

bool ChunkManager::isChunkInRange(const glm::ivec3& chunkPos, const glm::ivec3& centerChunkPos) const {
    glm::ivec3 diff = chunkPos - centerChunkPos;
    
    // Check horizontal distance (circular)
    int horizontalDist = diff.x * diff.x + diff.z * diff.z;
    if (horizontalDist > m_renderDistance * m_renderDistance) {
        return false;
    }
    
    // Check vertical distance (limited range)
    if (std::abs(diff.y) > 2) {
        return false;
    }
    
    return true;
}


void ChunkManager::loadChunkAsync(const glm::ivec3& chunkPos, int priority) {
    // Add to loading queue
    std::lock_guard<std::mutex> lock(m_queueMutex);
    m_loadQueue.push({chunkPos, priority});
    m_queueCondition.notify_one();
}

void ChunkManager::chunkWorkerThread() {
    while (!m_shutdown) {
        ChunkLoadRequest request;
        
        // Wait for work
        {
            std::unique_lock<std::mutex> lock(m_queueMutex);
            m_queueCondition.wait(lock, [this] { return !m_loadQueue.empty() || m_shutdown; });
            
            if (m_shutdown) {
                break;
            }
            
            request = m_loadQueue.top();
            m_loadQueue.pop();
        }
        
        // Check if chunk is still needed
        {
            std::lock_guard<std::mutex> lock(m_chunksMutex);
            if (m_chunks.find(request.chunkPos) != m_chunks.end()) {
                continue; // Already loaded
            }
        }
        
        // Load chunk in background
        auto chunkData = loadChunkBackground(request.chunkPos);
        
        // Add to completed queue
        {
            std::lock_guard<std::mutex> lock(m_completedMutex);
            m_completedChunks.push({request.chunkPos, std::move(chunkData)});
        }
    }
}

std::unique_ptr<ChunkData> ChunkManager::loadChunkBackground(const glm::ivec3& chunkPos) {
    LOG_TRACE("Loading chunk at (%d, %d, %d) in background", chunkPos.x, chunkPos.y, chunkPos.z);
    
    auto chunkData = std::make_unique<ChunkData>();
    
    // Create new chunk
    chunkData->chunk = std::make_unique<Chunk>(chunkPos);
    
    // Generate terrain
    generateTerrain(*chunkData->chunk);
    
    // Get neighboring chunks for proper face culling
    const Chunk* neighborXMinus = nullptr;
    const Chunk* neighborXPlus = nullptr;
    const Chunk* neighborYMinus = nullptr;
    const Chunk* neighborYPlus = nullptr;
    const Chunk* neighborZMinus = nullptr;
    const Chunk* neighborZPlus = nullptr;
    
    // Generate mesh with neighbor awareness
    // We need to keep the mutex locked during mesh generation to prevent neighbor chunks from being unloaded
    std::vector<BlockbenchInstanceGenerator::FaceInstance> faces;
    {
        std::lock_guard<std::mutex> lock(m_chunksMutex);
        
        auto neighborIt = m_chunks.find(chunkPos + glm::ivec3(-1, 0, 0));
        if (neighborIt != m_chunks.end()) neighborXMinus = neighborIt->second.get();
        
        neighborIt = m_chunks.find(chunkPos + glm::ivec3(1, 0, 0));
        if (neighborIt != m_chunks.end()) neighborXPlus = neighborIt->second.get();
        
        neighborIt = m_chunks.find(chunkPos + glm::ivec3(0, -1, 0));
        if (neighborIt != m_chunks.end()) neighborYMinus = neighborIt->second.get();
        
        neighborIt = m_chunks.find(chunkPos + glm::ivec3(0, 1, 0));
        if (neighborIt != m_chunks.end()) neighborYPlus = neighborIt->second.get();
        
        neighborIt = m_chunks.find(chunkPos + glm::ivec3(0, 0, -1));
        if (neighborIt != m_chunks.end()) neighborZMinus = neighborIt->second.get();
        
        neighborIt = m_chunks.find(chunkPos + glm::ivec3(0, 0, 1));
        if (neighborIt != m_chunks.end()) neighborZPlus = neighborIt->second.get();
        
        // Generate mesh while holding the lock to ensure neighbor chunks don't get unloaded
        faces = m_meshGenerator->generateChunkMeshWithNeighbors(*chunkData->chunk,
                                                               neighborXMinus, neighborXPlus,
                                                               neighborYMinus, neighborYPlus,
                                                               neighborZMinus, neighborZPlus);
    }
    
    chunkData->faces = std::move(faces);
    
    chunkData->ready = true;
    return chunkData;
}

void ChunkManager::processCompletedChunks() {
    std::lock_guard<std::mutex> completedLock(m_completedMutex);
    
    std::vector<glm::ivec3> chunksToRegenerate;
    
    while (!m_completedChunks.empty()) {
        auto [chunkPos, chunkData] = std::move(m_completedChunks.front());
        m_completedChunks.pop();
        
        // Move to main chunk storage
        {
            std::lock_guard<std::mutex> chunksLock(m_chunksMutex);
            m_chunks[chunkPos] = std::move(chunkData->chunk);
            m_chunkMeshes[chunkPos] = std::move(chunkData->faces);
            
            // Check which neighboring chunks exist and need regeneration
            glm::ivec3 neighbors[6] = {
                chunkPos + glm::ivec3(-1, 0, 0),
                chunkPos + glm::ivec3(1, 0, 0),
                chunkPos + glm::ivec3(0, -1, 0),
                chunkPos + glm::ivec3(0, 1, 0),
                chunkPos + glm::ivec3(0, 0, -1),
                chunkPos + glm::ivec3(0, 0, 1)
            };
            
            for (const auto& neighborPos : neighbors) {
                if (m_chunks.find(neighborPos) != m_chunks.end()) {
                    chunksToRegenerate.push_back(neighborPos);
                }
            }
        }
        
        // Mark for rebuild
        m_needsRebuild = true;
    }
    
    // Regenerate neighboring chunks' meshes to account for new neighbors
    for (const auto& chunkPos : chunksToRegenerate) {
        regenerateChunkMesh(chunkPos);
    }
    
    // Rebuild if needed
    if (m_needsRebuild) {
        rebuildAllFaceInstances();
        // Don't reset m_needsRebuild here - let it be reset when face instances are consumed
    }
}

void ChunkManager::regenerateChunkMesh(const glm::ivec3& chunkPos) {
    std::lock_guard<std::mutex> lock(m_chunksMutex);
    
    auto chunkIt = m_chunks.find(chunkPos);
    if (chunkIt == m_chunks.end()) {
        return;
    }
    
    // Get neighboring chunks for proper face culling
    const Chunk* neighborXMinus = nullptr;
    const Chunk* neighborXPlus = nullptr;
    const Chunk* neighborYMinus = nullptr;
    const Chunk* neighborYPlus = nullptr;
    const Chunk* neighborZMinus = nullptr;
    const Chunk* neighborZPlus = nullptr;
    
    auto neighborIt = m_chunks.find(chunkPos + glm::ivec3(-1, 0, 0));
    if (neighborIt != m_chunks.end()) neighborXMinus = neighborIt->second.get();
    
    neighborIt = m_chunks.find(chunkPos + glm::ivec3(1, 0, 0));
    if (neighborIt != m_chunks.end()) neighborXPlus = neighborIt->second.get();
    
    neighborIt = m_chunks.find(chunkPos + glm::ivec3(0, -1, 0));
    if (neighborIt != m_chunks.end()) neighborYMinus = neighborIt->second.get();
    
    neighborIt = m_chunks.find(chunkPos + glm::ivec3(0, 1, 0));
    if (neighborIt != m_chunks.end()) neighborYPlus = neighborIt->second.get();
    
    neighborIt = m_chunks.find(chunkPos + glm::ivec3(0, 0, -1));
    if (neighborIt != m_chunks.end()) neighborZMinus = neighborIt->second.get();
    
    neighborIt = m_chunks.find(chunkPos + glm::ivec3(0, 0, 1));
    if (neighborIt != m_chunks.end()) neighborZPlus = neighborIt->second.get();
    
    // Generate mesh for this chunk with neighbor awareness
    auto faces = m_meshGenerator->generateChunkMeshWithNeighbors(*chunkIt->second,
                                                                 neighborXMinus, neighborXPlus,
                                                                 neighborYMinus, neighborYPlus,
                                                                 neighborZMinus, neighborZPlus);
    
    // Update the mesh storage
    m_chunkMeshes[chunkPos] = std::move(faces);
    
    // Mark that face instances need rebuilding
    m_needsRebuild = true;
}

} // namespace Zerith
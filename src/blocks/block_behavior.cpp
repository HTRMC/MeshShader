#include "blocks/block_behavior.h"
#include "blocks/fluid_block.h"
#include <unordered_map>
#include <memory>

#include "blocks.h"

namespace Zerith {

std::unordered_map<BlockType, std::unique_ptr<BlockBehavior>> BlockBehaviorRegistry::s_behaviors;
BlockBehavior BlockBehaviorRegistry::s_defaultBehavior;

void BlockBehaviorRegistry::registerBehavior(BlockType blockType, std::unique_ptr<BlockBehavior> behavior) {
    s_behaviors[blockType] = std::move(behavior);
}

const BlockBehavior* BlockBehaviorRegistry::getBehavior(BlockType blockType) {
    auto it = s_behaviors.find(blockType);
    if (it != s_behaviors.end()) {
        return it->second.get();
    }
    return &s_defaultBehavior;
}

void BlockBehaviorRegistry::initialize() {
    // Register water block behavior using FluidBlock with appropriate settings
    auto waterSettings = BlockSettings::create()
        .liquid()
        .noCollision() 
        .transparent()
        .walkThrough();
    
    registerBehavior(Blocks::WATER, std::make_unique<FluidBlock>(waterSettings));
}

} // namespace Zerith
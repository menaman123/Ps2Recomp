#pragma once
#include <cstdint>
#include <cstddef>
#include <unordered_map>


struct SDL_Texture;


struct TextureCacheKey {
   uint32_t tbp, tbw, psm, tw, th, cbp, cpsm;


   bool operator==(const TextureCacheKey& o) const;
};


struct TextureCacheKeyHash {
   size_t operator()(const TextureCacheKey& k) const;
};


class TextureCache {
public:
   // Returns cached SDL_Texture* or nullptr on miss.
   // Updates last_used frame counter on hit.
   SDL_Texture* Lookup(const TextureCacheKey& key);


   // Insert decoded texture into cache. Takes ownership of SDL_Texture.
   // Triggers LRU eviction if cache exceeds MAX_ENTRIES.
   void Insert(const TextureCacheKey& key, SDL_Texture* tex);


   // Invalidate all entries (TEXFLUSH). Destroys all SDL_Texture objects.
   void InvalidateAll();


   // Invalidate entries whose TBP falls within [base_addr, base_addr + size_bytes).
   // Destroys the associated SDL_Texture objects.
   void InvalidateRegion(uint32_t base_addr, uint32_t size_bytes);


   // Destroy all SDL_Texture objects and clear the cache (shutdown).
   void Clear();


private:
   struct CacheEntry {
       SDL_Texture* texture = nullptr;
       uint64_t last_used = 0;
   };


   std::unordered_map<TextureCacheKey, CacheEntry, TextureCacheKeyHash> entries;
   uint64_t frame_counter = 0;
   static constexpr size_t MAX_ENTRIES = 128;


   void EvictLRU();
};




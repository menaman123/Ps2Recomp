#include "texture_cache.h"
#include <SDL.h>
#include <algorithm>
#include <limits>


// --- TextureCacheKey ---


bool TextureCacheKey::operator==(const TextureCacheKey& o) const {
   return tbp == o.tbp && tbw == o.tbw && psm == o.psm &&
          tw == o.tw && th == o.th && cbp == o.cbp && cpsm == o.cpsm;
}


size_t TextureCacheKeyHash::operator()(const TextureCacheKey& k) const {
   // Simple hash combining all key fields
   size_t h = 0;
   auto combine = [&](uint32_t v) {
       h ^= std::hash<uint32_t>{}(v) + 0x9e3779b9 + (h << 6) + (h >> 2);
   };
   combine(k.tbp);
   combine(k.tbw);
   combine(k.psm);
   combine(k.tw);
   combine(k.th);
   combine(k.cbp);
   combine(k.cpsm);
   return h;
}


// --- TextureCache ---


SDL_Texture* TextureCache::Lookup(const TextureCacheKey& key) {
   auto it = entries.find(key);
   if (it == entries.end()) {
       return nullptr;
   }
   it->second.last_used = ++frame_counter;
   return it->second.texture;
}


void TextureCache::Insert(const TextureCacheKey& key, SDL_Texture* tex) {
   if (!tex) return;


   // Evict if at capacity
   while (entries.size() >= MAX_ENTRIES) {
       EvictLRU();
   }


   auto it = entries.find(key);
   if (it != entries.end()) {
       // Replace existing entry — destroy old texture
       if (it->second.texture) {
           SDL_DestroyTexture(it->second.texture);
       }
       it->second.texture = tex;
       it->second.last_used = ++frame_counter;
   } else {
       CacheEntry entry;
       entry.texture = tex;
       entry.last_used = ++frame_counter;
       entries[key] = entry;
   }
}


void TextureCache::InvalidateAll() {
   for (auto& [key, entry] : entries) {
       if (entry.texture) {
           SDL_DestroyTexture(entry.texture);
       }
   }
   entries.clear();
}


void TextureCache::InvalidateRegion(uint32_t base_addr, uint32_t size_bytes) {
   for (auto it = entries.begin(); it != entries.end(); ) {
       uint32_t tbp = it->first.tbp;
       if (tbp >= base_addr && tbp < base_addr + size_bytes) {
           if (it->second.texture) {
               SDL_DestroyTexture(it->second.texture);
           }
           it = entries.erase(it);
       } else {
           ++it;
       }
   }
}


void TextureCache::Clear() {
   for (auto& [key, entry] : entries) {
       if (entry.texture) {
           SDL_DestroyTexture(entry.texture);
       }
   }
   entries.clear();
}


void TextureCache::EvictLRU() {
   if (entries.empty()) return;


   auto oldest = entries.begin();
   uint64_t min_used = std::numeric_limits<uint64_t>::max();


   for (auto it = entries.begin(); it != entries.end(); ++it) {
       if (it->second.last_used < min_used) {
           min_used = it->second.last_used;
           oldest = it;
       }
   }


   if (oldest->second.texture) {
       SDL_DestroyTexture(oldest->second.texture);
   }
   entries.erase(oldest);
}




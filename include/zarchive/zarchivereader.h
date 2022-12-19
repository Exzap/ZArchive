#pragma once

#include <cstdint>
#include <vector>
#include <string_view>
#include <unordered_map>
#include <mutex>

#include <filesystem>
#include <fstream>

#include "zarchivecommon.h"

using ZArchiveNodeHandle = uint32_t;

static inline ZArchiveNodeHandle ZARCHIVE_INVALID_NODE = 0xFFFFFFFF;

class ZArchiveReader
{
public:
	struct ZAR_PUB DirEntry
	{
		std::string_view name;
		bool isFile;
		bool isDirectory;
		uint64_t size; // only valid for directories
	};

	ZAR_PUB static ZArchiveReader* OpenFromFile(const std::filesystem::path& path);

	ZAR_PUB ~ZArchiveReader();

	ZAR_PUB ZArchiveNodeHandle LookUp(std::string_view path, bool allowFile = true, bool allowDirectory = true);
	ZAR_PUB bool IsDirectory(ZArchiveNodeHandle nodeHandle) const;
	ZAR_PUB bool IsFile(ZArchiveNodeHandle nodeHandle) const;

	// directory operations
	ZAR_PUB uint32_t GetDirEntryCount(ZArchiveNodeHandle nodeHandle) const;
	ZAR_PUB bool GetDirEntry(ZArchiveNodeHandle nodeHandle, uint32_t index, DirEntry& dirEntry) const;

	// file operations
	ZAR_PUB uint64_t GetFileSize(ZArchiveNodeHandle nodeHandle);
	ZAR_PUB uint64_t ReadFromFile(ZArchiveNodeHandle nodeHandle, uint64_t offset, uint64_t length, void* buffer);

private:
	struct CacheBlock
	{
		uint8_t* data;
		uint64_t blockIndex;
		// linked-list for LRU
		CacheBlock* prev;
		CacheBlock* next;
	};

	std::mutex m_accessMutex;

	std::vector<uint8_t> m_cacheDataBuffer;
	std::vector<CacheBlock> m_cacheBlocks;
	CacheBlock* m_lruChainFirst;
	CacheBlock* m_lruChainLast;
	std::unordered_map<uint64_t, CacheBlock*> m_blockLookup;

	ZArchiveReader(std::ifstream&& file, std::vector<_ZARCHIVE::CompressionOffsetRecord>&& offsetRecords, std::vector<uint8_t>&& nameTable, std::vector<_ZARCHIVE::FileDirectoryEntry>&& fileTree, uint64_t compressedDataOffset, uint64_t compressedDataSize);

	CacheBlock* GetCachedBlock(uint64_t blockIndex);
	CacheBlock* RecycleLRUBlock(uint64_t newBlockIndex);
	void MarkBlockAsMRU(CacheBlock* block);

	void RegisterBlock(CacheBlock* block, uint64_t blockIndex);
	void UnregisterBlock(CacheBlock* block);
	bool LoadBlock(CacheBlock* block);

	static std::string_view GetName(const std::vector<uint8_t>& nameTable, uint32_t nameOffset);

	std::ifstream m_file;
	std::vector<_ZARCHIVE::CompressionOffsetRecord> m_offsetRecords;
	std::vector<uint8_t> m_nameTable;
	std::vector<_ZARCHIVE::FileDirectoryEntry> m_fileTree;
	uint64_t m_compressedDataOffset;
	uint64_t m_compressedDataSize;
	uint64_t m_blockCount;

	std::vector<uint8_t> m_blockDecompressionBuffer;
};
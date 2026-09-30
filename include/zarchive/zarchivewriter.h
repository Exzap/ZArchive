#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <string_view>
#include <unordered_map>

#include "zarchivecommon.h"

class ZArchiveWriter
{
	struct PathNode
	{
		PathNode() : isFile(false), nameIndex(0xFFFFFFFF) {};
		PathNode(bool isFile, uint32_t nameIndex) : isFile(isFile), nameIndex(nameIndex) {};
		PathNode(const PathNode&) = delete;
		PathNode& operator=(const PathNode&) = delete;
		~PathNode()
		{
			for (PathNode* subnode : subnodes)
				delete subnode;
		}

		bool isFile;
		uint32_t nameIndex; // index in m_nodeNames

		std::vector<PathNode*> subnodes; // owned
		// Lookup acceleration for large directories: hash of the (ASCII case-folded) name -> subnode. Only built once a directory has more than a few entries
		std::unique_ptr<std::unordered_multimap<uint64_t, PathNode*>> subnodeLookup;

		// file properties
		uint64_t fileOffset{};
		uint64_t fileSize{};
		// directory properties
		uint32_t nodeStartIndex{};
	};

public:
	typedef void(*CB_NewOutputFile)(const int32_t partIndex, void* ctx);
	typedef void(*CB_WriteOutputData)(const void* data, size_t length, void* ctx);

	ZArchiveWriter(CB_NewOutputFile cbNewOutputFile, CB_WriteOutputData cbWriteOutputData, void* ctx);
	~ZArchiveWriter();

	// Number of threads used to compress data. 0 = automatic (number of hardware threads), 1 = compress on the calling thread only.
	// The output is identical regardless of the thread count. Output callbacks are always invoked from the thread that calls AppendData()/Finalize(),
	// but with more than one thread the data is delivered in batches (all remaining data is delivered by Finalize())
	void SetThreadCount(uint32_t threadCount);

	// zstd compression level, default is 6. Higher levels produce smaller archives but are slower to create. The level has no influence on
	// read speed or on compatibility, archives created with any level can be read by every ZArchive reader. Values outside the range supported by zstd are clamped
	void SetCompressionLevel(int compressionLevel);

	bool StartNewFile(const char* path); // creates a new virtual file and makes it active
	void AppendData(const void* data, size_t size); // appends data to currently active file
	bool MakeDir(const char* path, bool recursive = false);
	void Finalize();

private:
	PathNode* GetNodeByPath(PathNode* root, std::string_view path);
	PathNode* FindSubnodeByName(PathNode* parent, std::string_view nodeName);
	void AddSubnode(PathNode* parent, PathNode* node);

	uint32_t CreateNameEntry(std::string_view name);

	void OutputData(const void* data, size_t length);
	uint64_t GetCurrentOutputOffset() const;

	void StoreBlock(const uint8_t* uncompressedData);
	void EmitBlock(const uint8_t* uncompressedData, const uint8_t* compressedData, size_t compressedSize);
	void FlushBlockBatch();
	uint32_t GetEffectiveThreadCount();

	void WriteOffsetRecords();
	void WriteNameTable();
	void WriteFileTree();
	void WriteMetaData();
	void WriteFooter();

private:
	// callbacks
	CB_NewOutputFile m_cbNewOutputFile;
	CB_WriteOutputData m_cbWriteOutputData;
	void* m_cbCtx;
	// file tree
	PathNode m_rootNode;
	PathNode* m_currentFileNode{ nullptr };
	std::vector<std::string> m_nodeNames;
	std::vector<uint32_t> m_nodeNameOffsets;
	std::unordered_map<std::string, uint32_t> m_nodeNameLookup;
	// footer
	_ZARCHIVE::Footer m_footer;
	// writes and compression
	std::vector<uint8_t> m_currentWriteBuffer;
	std::vector<uint8_t> m_compressionBuffer;
	// multi-threaded compression: blocks are collected into a batch, compressed in parallel and then emitted in order
	int m_compressionLevel{ 6 };
	uint32_t m_threadCount{ 0 }; // 0 = automatic
	uint32_t m_effectiveThreadCount{ 0 }; // resolved from m_threadCount, 0 = not resolved yet
	size_t m_batchCapacity{ 0 }; // in blocks
	size_t m_batchCount{ 0 };
	size_t m_compressBound{ 0 };
	std::vector<uint8_t> m_batchInput;
	std::vector<uint8_t> m_batchOutput;
	std::vector<size_t> m_batchOutputSizes;
	uint64_t m_currentCompressedWriteIndex{ 0 }; // output file write index
	uint64_t m_currentInputOffset{ 0 }; // current offset within uncompressed file data
	// uncompressed-to-compressed offset records
	uint64_t m_numWrittenOffsetRecords{ 0 };
	std::vector<_ZARCHIVE::CompressionOffsetRecord> m_compressionOffsetRecord;
	// hashing
	struct Sha_256* m_mainShaCtx{};
	uint8_t m_integritySha[32];
};
#include "zarchive/zarchivewriter.h"
#include "zarchive/zarchivecommon.h"

#include <string>
#include <string_view>
#include <queue>

#include <zstd.h>

#include "sha_256.h"

#include <cassert>
#include <algorithm>
#include <atomic>
#include <thread>

ZArchiveWriter::ZArchiveWriter(CB_NewOutputFile cbNewOutputFile, CB_WriteOutputData cbWriteOutputData, void* ctx) : m_cbNewOutputFile(cbNewOutputFile), m_cbWriteOutputData(cbWriteOutputData), m_cbCtx(ctx)
{
	cbNewOutputFile(-1, ctx);
	m_mainShaCtx = (struct Sha_256*)malloc(sizeof(struct Sha_256));
	sha_256_init(m_mainShaCtx, m_integritySha);
};

ZArchiveWriter::~ZArchiveWriter()
{
	free(m_mainShaCtx);
}

ZArchiveWriter::PathNode* ZArchiveWriter::GetNodeByPath(ZArchiveWriter::PathNode* root, std::string_view path)
{
	PathNode* currentNode = &m_rootNode;

	std::string_view pathParser = path;
	while (true)
	{
		std::string_view nodeName;
		if (!_ZARCHIVE::GetNextPathNode(pathParser, nodeName))
			break;
		PathNode* nextSubnode = FindSubnodeByName(currentNode, nodeName);
		if (!nextSubnode || (nextSubnode && nextSubnode->isFile))
			return nullptr;
		currentNode = nextSubnode;
	}
	return currentNode;
}

// Directories with at least this many entries get a hash index. Small directories are faster to scan linearly
static constexpr size_t SUBNODE_LOOKUP_THRESHOLD = 16;

// Hash of a node name. Case-insensitive in the same way as _ZARCHIVE::CompareNodeName (ASCII letters only)
static uint64_t HashNodeName(std::string_view name)
{
	uint64_t hash = 0xcbf29ce484222325ULL; // FNV-1a
	for (char ch : name)
	{
		if (ch >= 'A' && ch <= 'Z')
			ch = (char)(ch + ('a' - 'A'));
		hash ^= (uint64_t)(uint8_t)ch;
		hash *= 0x100000001b3ULL;
	}
	return hash;
}

ZArchiveWriter::PathNode* ZArchiveWriter::FindSubnodeByName(ZArchiveWriter::PathNode* parent, std::string_view nodeName)
{
	if (parent->subnodeLookup)
	{
		auto range = parent->subnodeLookup->equal_range(HashNodeName(nodeName));
		for (auto it = range.first; it != range.second; ++it)
		{
			if (_ZARCHIVE::CompareNodeNameBool(m_nodeNames[it->second->nameIndex], nodeName))
				return it->second;
		}
		return nullptr;
	}
	for (auto& it : parent->subnodes)
	{
		std::string_view itName = m_nodeNames[it->nameIndex];
		if (_ZARCHIVE::CompareNodeNameBool(itName, nodeName))
			return it;
	}
	return nullptr;
}

// takes ownership of node
void ZArchiveWriter::AddSubnode(PathNode* parent, PathNode* node)
{
	parent->subnodes.emplace_back(node);
	if (parent->subnodeLookup)
	{
		parent->subnodeLookup->emplace(HashNodeName(m_nodeNames[node->nameIndex]), node);
	}
	else if (parent->subnodes.size() >= SUBNODE_LOOKUP_THRESHOLD)
	{
		// directory grew large enough, build the index
		parent->subnodeLookup = std::make_unique<std::unordered_multimap<uint64_t, PathNode*>>();
		parent->subnodeLookup->reserve(parent->subnodes.size() * 2);
		for (PathNode* subnode : parent->subnodes)
			parent->subnodeLookup->emplace(HashNodeName(m_nodeNames[subnode->nameIndex]), subnode);
	}
}

bool ZArchiveWriter::StartNewFile(const char* path)
{
	m_currentFileNode = nullptr;
	std::string_view pathParser = path;
	std::string_view filename;
	_ZARCHIVE::SplitFilenameFromPath(pathParser, filename);
	PathNode* dir = GetNodeByPath(&m_rootNode, pathParser);
	if (!dir)
		return false;
	if (FindSubnodeByName(dir, filename))
		return false;
	// add new entry and make it the currently active file for append operations
	PathNode* node = new PathNode(true, CreateNameEntry(filename));
	AddSubnode(dir, node);
	m_currentFileNode = node;
	node->fileOffset = m_currentInputOffset;
	return true;
}

bool ZArchiveWriter::MakeDir(const char* path, bool recursive)
{
	std::string_view pathParser = path;
	while (!pathParser.empty() && (pathParser.back() == '/' || pathParser.back() == '\\'))
		pathParser.remove_suffix(1);
	if (!recursive)
	{
		std::string_view dirName;
		_ZARCHIVE::SplitFilenameFromPath(pathParser, dirName);
		PathNode* dir = GetNodeByPath(&m_rootNode, pathParser);
		if (!dir)
			return false;
		if (FindSubnodeByName(dir, dirName))
			return false;
		AddSubnode(dir, new PathNode(false, CreateNameEntry(dirName)));
	}
	else
	{
		PathNode* currentNode = &m_rootNode;
		while (true)
		{
			std::string_view nodeName;
			if (!_ZARCHIVE::GetNextPathNode(pathParser, nodeName))
				break;
			PathNode* nextSubnode = FindSubnodeByName(currentNode, nodeName);
			if (nextSubnode && nextSubnode->isFile)
				return false;
			if (!nextSubnode)
			{
				nextSubnode = new PathNode(false, CreateNameEntry(nodeName));
				AddSubnode(currentNode, nextSubnode);
			}
			currentNode = nextSubnode;
		}
	}
	return true;
}

uint32_t ZArchiveWriter::CreateNameEntry(std::string_view name)
{
	auto it = m_nodeNameLookup.find(std::string(name));
	if (it != m_nodeNameLookup.end())
		return it->second;
	uint32_t nameIndex = (uint32_t)m_nodeNames.size();
	m_nodeNames.emplace_back(name);
	m_nodeNameLookup.emplace(name, nameIndex);
	return nameIndex;
}

void ZArchiveWriter::OutputData(const void* data, size_t length)
{
	m_cbWriteOutputData(data, length, m_cbCtx);
	m_currentCompressedWriteIndex += length;
	// hash the data
	if (m_mainShaCtx)
		sha_256_write(m_mainShaCtx, data, length);
}

uint64_t ZArchiveWriter::GetCurrentOutputOffset() const
{
	return m_currentCompressedWriteIndex;
}

static constexpr uint32_t MAX_AUTO_THREADS = 32;
static constexpr size_t BATCH_BLOCKS_PER_THREAD = 8;

void ZArchiveWriter::SetThreadCount(uint32_t threadCount)
{
	FlushBlockBatch(); // deliver everything that was collected with the previous settings
	m_threadCount = threadCount;
	m_effectiveThreadCount = 0;
	// release the batch buffers, they are sized for the previous thread count and reallocated on demand
	m_batchCapacity = 0;
	std::vector<uint8_t>().swap(m_batchInput);
	std::vector<uint8_t>().swap(m_batchOutput);
	std::vector<size_t>().swap(m_batchOutputSizes);
}

void ZArchiveWriter::SetCompressionLevel(int compressionLevel)
{
	FlushBlockBatch(); // blocks which were collected so far are compressed with the previous level
	m_compressionLevel = std::clamp(compressionLevel, ZSTD_minCLevel(), ZSTD_maxCLevel());
}

uint32_t ZArchiveWriter::GetEffectiveThreadCount()
{
	if (m_effectiveThreadCount == 0)
	{
		uint32_t count = m_threadCount;
		if (count == 0)
			count = std::min<uint32_t>(std::thread::hardware_concurrency(), MAX_AUTO_THREADS);
		m_effectiveThreadCount = std::max<uint32_t>(count, 1);
	}
	return m_effectiveThreadCount;
}

// writes a block to the output. compressedSize is the result of ZSTD_compress (which includes error codes)
void ZArchiveWriter::EmitBlock(const uint8_t* uncompressedData, const uint8_t* compressedData, size_t outputSize)
{
	uint64_t compressedWriteOffset = GetCurrentOutputOffset();
	if (outputSize >= _ZARCHIVE::COMPRESSED_BLOCK_SIZE)
	{
		// store block uncompressed if it is equal or larger than the input after compression
		outputSize = _ZARCHIVE::COMPRESSED_BLOCK_SIZE;
		OutputData(uncompressedData, _ZARCHIVE::COMPRESSED_BLOCK_SIZE);
	}
	else
	{
		OutputData(compressedData, outputSize);
	}
	// add offset translation record
	if ((m_numWrittenOffsetRecords % _ZARCHIVE::ENTRIES_PER_OFFSETRECORD) == 0)
		m_compressionOffsetRecord.emplace_back().baseOffset = compressedWriteOffset;
	m_compressionOffsetRecord.back().size[m_numWrittenOffsetRecords % _ZARCHIVE::ENTRIES_PER_OFFSETRECORD] = (uint16_t)outputSize - 1;
	m_numWrittenOffsetRecords++;
}

void ZArchiveWriter::StoreBlock(const uint8_t* uncompressedData)
{
	if (GetEffectiveThreadCount() <= 1)
	{
		// compress and store right away
		m_compressionBuffer.resize(ZSTD_compressBound(_ZARCHIVE::COMPRESSED_BLOCK_SIZE));
		size_t outputSize = ZSTD_compress(m_compressionBuffer.data(), m_compressionBuffer.size(), uncompressedData, _ZARCHIVE::COMPRESSED_BLOCK_SIZE, m_compressionLevel);
		EmitBlock(uncompressedData, m_compressionBuffer.data(), outputSize);
		return;
	}
	// multi-threaded: collect the block into the current batch. The batch is compressed in parallel once it is full (or on Finalize)
	if (m_batchCapacity == 0)
	{
		m_compressBound = ZSTD_compressBound(_ZARCHIVE::COMPRESSED_BLOCK_SIZE);
		m_batchCapacity = (size_t)GetEffectiveThreadCount() * BATCH_BLOCKS_PER_THREAD;
		m_batchInput.resize(m_batchCapacity * _ZARCHIVE::COMPRESSED_BLOCK_SIZE);
		m_batchOutput.resize(m_batchCapacity * m_compressBound);
		m_batchOutputSizes.resize(m_batchCapacity);
	}
	std::memcpy(m_batchInput.data() + m_batchCount * _ZARCHIVE::COMPRESSED_BLOCK_SIZE, uncompressedData, _ZARCHIVE::COMPRESSED_BLOCK_SIZE);
	m_batchCount++;
	if (m_batchCount == m_batchCapacity)
		FlushBlockBatch();
}

// compresses all collected blocks in parallel and then outputs them in their original order
void ZArchiveWriter::FlushBlockBatch()
{
	if (m_batchCount == 0)
		return;
	const size_t blockCount = m_batchCount;
	std::atomic<size_t> nextBlock{ 0 };
	auto worker = [&]()
	{
		while (true)
		{
			size_t i = nextBlock.fetch_add(1, std::memory_order_relaxed);
			if (i >= blockCount)
				break;
			// every block is compressed independently, therefore the result is the same as when compressing one after the other
			m_batchOutputSizes[i] = ZSTD_compress(m_batchOutput.data() + i * m_compressBound, m_compressBound, m_batchInput.data() + i * _ZARCHIVE::COMPRESSED_BLOCK_SIZE, _ZARCHIVE::COMPRESSED_BLOCK_SIZE, m_compressionLevel);
		}
	};
	std::vector<std::thread> helpers;
	size_t helperCount = std::min<size_t>(GetEffectiveThreadCount(), blockCount) - 1; // the calling thread participates as well
	try
	{
		for (size_t i = 0; i < helperCount; i++)
			helpers.emplace_back(worker);
	}
	catch (...)
	{
		// unable to start more threads, the threads we have (at least the calling thread) will process the remaining blocks
	}
	worker();
	for (auto& helper : helpers)
		helper.join();
	// output in order
	for (size_t i = 0; i < blockCount; i++)
		EmitBlock(m_batchInput.data() + i * _ZARCHIVE::COMPRESSED_BLOCK_SIZE, m_batchOutput.data() + i * m_compressBound, m_batchOutputSizes[i]);
	m_batchCount = 0;
}

void ZArchiveWriter::AppendData(const void* data, size_t size)
{
	size_t dataSize = size;
	const uint8_t* input = (const uint8_t*)data;
	while (size > 0)
	{
		size_t bytesToCopy = _ZARCHIVE::COMPRESSED_BLOCK_SIZE - m_currentWriteBuffer.size();
		if (bytesToCopy > size)
			bytesToCopy = size;
		if (bytesToCopy == _ZARCHIVE::COMPRESSED_BLOCK_SIZE)
		{
			// if incoming data is block-aligned we can store it directly without memcpy to temporary buffer
			StoreBlock(input);
			input += bytesToCopy;
			size -= bytesToCopy;
			continue;
		}
		m_currentWriteBuffer.insert(m_currentWriteBuffer.end(), input, input + bytesToCopy);
		input += bytesToCopy;
		size -= bytesToCopy;
		if (m_currentWriteBuffer.size() == _ZARCHIVE::COMPRESSED_BLOCK_SIZE)
		{
			StoreBlock(m_currentWriteBuffer.data());
			m_currentWriteBuffer.clear();
		}
	}
	if (m_currentFileNode)
		m_currentFileNode->fileSize += dataSize;
	m_currentInputOffset += dataSize;
}

void ZArchiveWriter::Finalize()
{
	m_currentFileNode = nullptr; // make sure the padding added below doesn't modify the active file
	// flush write buffer by padding it to the length of a full block
	if (!m_currentWriteBuffer.empty())
	{
		std::vector<uint8_t> padBuffer;
		padBuffer.resize(_ZARCHIVE::COMPRESSED_BLOCK_SIZE - m_currentWriteBuffer.size());
		AppendData(padBuffer.data(), padBuffer.size());
	}
	FlushBlockBatch(); // make sure all blocks have been compressed and written
	m_footer.sectionCompressedData.offset = 0;
	m_footer.sectionCompressedData.size = GetCurrentOutputOffset();
	// pad to 8 byte
	while ((GetCurrentOutputOffset() % 8) != 0)
	{
		uint8_t b = 0;
		OutputData(&b, sizeof(uint8_t));
	}
	WriteOffsetRecords();
	WriteNameTable();
	WriteFileTree();
	WriteMetaData();
	WriteFooter();
}

void ZArchiveWriter::WriteOffsetRecords()
{
	m_footer.sectionOffsetRecords.offset = GetCurrentOutputOffset();
	_ZARCHIVE::CompressionOffsetRecord::Serialize(m_compressionOffsetRecord.data(), m_compressionOffsetRecord.size(), m_compressionOffsetRecord.data()); // in-place
	OutputData(m_compressionOffsetRecord.data(), m_compressionOffsetRecord.size() * sizeof(_ZARCHIVE::CompressionOffsetRecord));
	m_footer.sectionOffsetRecords.size = GetCurrentOutputOffset() - m_footer.sectionOffsetRecords.offset;
}

void ZArchiveWriter::WriteNameTable()
{
	m_footer.sectionNames.offset = GetCurrentOutputOffset();
	uint32_t currentNameTableOffset = 0;
	m_nodeNameOffsets.resize(m_nodeNames.size());
	for (size_t i = 0; i < m_nodeNames.size(); i++)
	{
		m_nodeNameOffsets[i] = currentNameTableOffset;
		// Each node name is stored with a length prefix byte. The prefix byte's MSB is used to indicate if an extended 2-byte header is used. The lower 7 bits are used to store the lower bits of the name length
		// If MSB is set, add an extra byte which extends the 7 bit name length field to 15 bit
		std::string_view name = m_nodeNames[i];
		if (name.size() > 0x7FFF)
			name = name.substr(0, 0x7FFF); // cut-off after 2^15-1 characters
		if (name.size() >= 0x80)
		{
			uint8_t header[2];
			header[0] = (uint8_t)(name.size() & 0x7F) | 0x80;
			header[1] = (uint8_t)(name.size() >> 7);
			OutputData(header, 2);
			currentNameTableOffset += 2;
		}
		else
		{
			uint8_t header[1];
			header[0] = (uint8_t)name.size() & 0x7F;
			OutputData(header, 1);
			currentNameTableOffset += 1;
		}
		OutputData(name.data(), name.size());
		currentNameTableOffset += (uint32_t)name.size();
	}
	m_footer.sectionNames.size = GetCurrentOutputOffset() - m_footer.sectionNames.offset;
}

void ZArchiveWriter::WriteFileTree()
{
	std::queue<PathNode*> nodeQueue;
	// first pass - assign a node range to all directories
	nodeQueue.push(&m_rootNode);
	uint32_t currentIndex = 1; // root node is at index 0
	while (!nodeQueue.empty())
	{
		PathNode* node = nodeQueue.front();
		nodeQueue.pop();
		if (node->isFile)
		{
			node->nodeStartIndex = (uint32_t)0xFFFFFFFF;
			continue;
		}
		// order entries lexicographically so we can use binary search in the reader
		std::sort(node->subnodes.begin(), node->subnodes.end(),
			[&](ZArchiveWriter::PathNode*& a, ZArchiveWriter::PathNode*& b) -> int
			{
				return _ZARCHIVE::CompareNodeName(m_nodeNames[a->nameIndex], m_nodeNames[b->nameIndex]) > 0;
			});

		node->nodeStartIndex = currentIndex;
		currentIndex += (uint32_t)node->subnodes.size();
		for (auto& it : node->subnodes)
			nodeQueue.push(it);
	}
	// second pass - serialize to file
	m_footer.sectionFileTree.offset = GetCurrentOutputOffset();
	nodeQueue.push(&m_rootNode);
	while (!nodeQueue.empty())
	{
		PathNode* node = nodeQueue.front();
		nodeQueue.pop();

		_ZARCHIVE::FileDirectoryEntry tmp;
		if(node == &m_rootNode)
			tmp.SetTypeAndNameOffset(node->isFile, 0x7FFFFFFF);
		else
			tmp.SetTypeAndNameOffset(node->isFile, m_nodeNameOffsets[node->nameIndex]);
		if (node->isFile)
		{
			tmp.SetFileOffset(node->fileOffset);
			tmp.SetFileSize(node->fileSize);
		}
		else
		{
			tmp.directoryRecord.count = (uint32_t)node->subnodes.size();
			tmp.directoryRecord.nodeStartIndex = node->nodeStartIndex;
			tmp.directoryRecord._reserved = 0;
		}
		_ZARCHIVE::FileDirectoryEntry::Serialize(&tmp, 1, &tmp);
		OutputData(&tmp, sizeof(_ZARCHIVE::FileDirectoryEntry));
		for (auto& it : node->subnodes)
			nodeQueue.push(it);
	}
	m_footer.sectionFileTree.size = GetCurrentOutputOffset() - m_footer.sectionFileTree.offset;
}

void ZArchiveWriter::WriteMetaData()
{
	// todo
	m_footer.sectionMetaDirectory.offset = GetCurrentOutputOffset();
	m_footer.sectionMetaDirectory.size = 0;
	m_footer.sectionMetaData.offset = GetCurrentOutputOffset();
	m_footer.sectionMetaData.size = 0;
}

void ZArchiveWriter::WriteFooter()
{
	m_footer.magic = _ZARCHIVE::Footer::kMagic;
	m_footer.version = _ZARCHIVE::Footer::kVersion1;
	m_footer.totalSize = GetCurrentOutputOffset() + sizeof(_ZARCHIVE::Footer);

	_ZARCHIVE::Footer tmp;

	// serialize and hash the footer with all hash bytes set to zero
	memset(m_footer.integrityHash, 0, 32);
	_ZARCHIVE::Footer::Serialize(&m_footer, &tmp);
	sha_256_write(m_mainShaCtx, &tmp, sizeof(_ZARCHIVE::Footer));
	sha_256_close(m_mainShaCtx);
	free(m_mainShaCtx);
	m_mainShaCtx = nullptr;

	// set hash and write footer
	memcpy(m_footer.integrityHash, m_integritySha, 32);
	_ZARCHIVE::Footer::Serialize(&m_footer, &tmp);
	OutputData(&tmp, sizeof(_ZARCHIVE::Footer));
}

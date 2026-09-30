#include "zarchive/zarchivewriter.h"
#include "zarchive/zarchivereader.h"

#include <vector>
#include <fstream>
#include <filesystem>
#include <cassert>
#include <optional>
#include <cstdlib>
#include <cstdint>
#include <string>
#include <algorithm>
#include <chrono>

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
static bool StdoutIsTerminal() { return _isatty(_fileno(stdout)) != 0; }
#else
#include <unistd.h>
static bool StdoutIsTerminal() { return isatty(fileno(stdout)) != 0; }
#endif

namespace fs = std::filesystem;

// Single line progress bar. Only used when stdout is an interactive terminal, otherwise the tool prints one line per file like before.
// Not thread-safe, must only be used from the main thread
class ProgressBar
{
public:
	void Enable(bool enabled) { m_enabled = enabled; }
	bool IsEnabled() const { return m_enabled; }

	void Start(uint64_t totalBytes)
	{
		m_total = totalBytes;
		m_done = 0;
		m_lineOpen = false;
		m_startTime = Clock::now();
		m_lastDraw = m_startTime;
		if (m_enabled)
			Draw();
	}

	void Add(uint64_t bytes)
	{
		m_done += bytes;
		if (!m_enabled)
			return;
		const auto now = Clock::now();
		if (now - m_lastDraw < std::chrono::milliseconds(100))
			return; // redraw at most 10 times per second
		m_lastDraw = now;
		Draw();
	}

	// ends the current line so that messages printed afterwards do not end up in the middle of the bar
	void Break()
	{
		if (m_enabled && m_lineOpen)
		{
			fputc('\n', stdout);
			fflush(stdout);
			m_lineOpen = false;
		}
	}

	void Finish()
	{
		if (!m_enabled)
			return;
		m_done = m_total; // the files could have changed since the initial scan
		Draw();
		Break();
	}

private:
	using Clock = std::chrono::steady_clock;

	static int PickUnit(double value)
	{
		int unit = 0;
		while (unit < 4 && value >= 1024.0)
		{
			value /= 1024.0;
			unit++;
		}
		return unit;
	}

	void Draw()
	{
		static const char* const unitNames[] = { "B", "KiB", "MiB", "GiB", "TiB" };
		const double total = (double)m_total;
		const double done = (double)std::min(m_done, m_total);
		const double fraction = m_total > 0 ? done / total : 1.0;
		constexpr int barWidth = 20;
		char bar[barWidth + 1];
		const int filled = (int)(fraction * barWidth);
		for (int i = 0; i < barWidth; i++)
			bar[i] = i < filled ? '#' : '-';
		bar[barWidth] = '\0';
		const double elapsed = std::chrono::duration<double>(Clock::now() - m_startTime).count();
		const double speed = elapsed > 0.25 ? done / elapsed : 0.0; // bytes per second, average since start
		// use the same unit for the processed and the total amount
		const int unit = PickUnit(total);
		const double unitScale = (double)(1ull << (10 * unit));
		const int speedUnit = PickUnit(speed);
		const double speedScale = (double)(1ull << (10 * speedUnit));
		char eta[32] = "--:--";
		if (m_done >= m_total)
		{
			snprintf(eta, sizeof(eta), "00:00");
		}
		else if (speed > 0.0)
		{
			const unsigned long long secs = (unsigned long long)((total - done) / speed);
			if (secs >= 3600)
				snprintf(eta, sizeof(eta), "%llu:%02llu:%02llu", secs / 3600, (secs / 60) % 60, secs % 60);
			else
				snprintf(eta, sizeof(eta), "%02llu:%02llu", secs / 60, secs % 60);
		}
		char line[160];
		int len = snprintf(line, sizeof(line), "[%s] %5.1f%%  %.1f/%.1f %s  %.1f %s/s  ETA %s", bar, fraction * 100.0, done / unitScale, total / unitScale, unitNames[unit], speed / speedScale, unitNames[speedUnit], eta);
		if (len < 0)
			return;
		// pad to a fixed width so leftovers of a previous, longer line are overwritten. Stay below 80 columns to avoid wrapping in narrow consoles
		constexpr int lineWidth = 78;
		fprintf(stdout, "\r%s%*s", line, std::max(0, lineWidth - len), "");
		fflush(stdout);
		m_lineOpen = true;
	}

	bool m_enabled{ false };
	bool m_lineOpen{ false };
	uint64_t m_total{ 0 };
	uint64_t m_done{ 0 };
	Clock::time_point m_startTime;
	Clock::time_point m_lastDraw;
};

static ProgressBar g_progress;

void PrintHelp()
{
	puts("Usage:\n");
	puts("zarchive.exe input_path [output_path] [options]");
	puts("If input_path is a directory, then output_path will be the ZArchive output file path");
	puts("If input_path is a ZArchive file path, then output_path will be the output directory");
	puts("output_path is optional");
	puts("");
	puts("Options:");
	puts("-j N            Number of threads to use when creating an archive (default: number of hardware threads)");
	puts("-l N            zstd compression level used when creating an archive (default: 6). Higher levels create smaller archives but are slower.");
	puts("                Archives can be read the same way regardless of the level");
	puts("--no-progress   Print one line per file instead of a progress bar (this is always the case if the output is not a terminal)");
}

bool ExtractFile(ZArchiveReader* reader, std::string_view srcPath, const fs::path& path, std::vector<uint8_t>& buffer)
{
	ZArchiveNodeHandle fileHandle = reader->LookUp(srcPath, true, false);
	if (fileHandle == ZARCHIVE_INVALID_NODE)
	{
		g_progress.Break();
		puts("Unable to extract file:");
		puts(std::string(srcPath).c_str());
		return false;
	}

	std::ofstream fileOut(path, std::ios_base::binary | std::ios_base::out | std::ios_base::trunc);
	if (!fileOut.is_open())
	{
		g_progress.Break();
		puts("Unable to write file:");
		puts(path.generic_string().c_str());
		return false;
	}
	uint64_t readOffset = 0;
	while (true)
	{
		uint64_t bytesRead = reader->ReadFromFile(fileHandle, readOffset, buffer.size(), buffer.data());
		if (bytesRead == 0)
			break;
		fileOut.write((const char*)buffer.data(), bytesRead);
		readOffset += bytesRead;
		g_progress.Add(bytesRead);
	}
	fileOut.close();
	if (fileOut.fail())
	{
		g_progress.Break();
		puts("Failed to write file:");
		puts(path.generic_string().c_str());
		return false;
	}
	if (readOffset != reader->GetFileSize(fileHandle))
		return false;

	return true;
}

// total size of all files below srcPath, used for the progress bar
static uint64_t CountArchiveBytes(ZArchiveReader* reader, const std::string& srcPath)
{
	ZArchiveNodeHandle dirHandle = reader->LookUp(srcPath, false, true);
	if (dirHandle == ZARCHIVE_INVALID_NODE)
		return 0;
	uint64_t total = 0;
	uint32_t numEntries = reader->GetDirEntryCount(dirHandle);
	for (uint32_t i = 0; i < numEntries; i++)
	{
		ZArchiveReader::DirEntry dirEntry;
		if (!reader->GetDirEntry(dirHandle, i, dirEntry))
			continue; // invalid nodes are reported when extracting
		if (dirEntry.isDirectory)
			total += CountArchiveBytes(reader, std::string(srcPath).append("/").append(dirEntry.name));
		else
			total += dirEntry.size;
	}
	return total;
}

bool ExtractRecursive(ZArchiveReader* reader, std::string srcPath, fs::path outputDirectory, std::vector<uint8_t>& buffer)
{
	ZArchiveNodeHandle dirHandle = reader->LookUp(srcPath, false, true);
	if (dirHandle == ZARCHIVE_INVALID_NODE)
		return false;
	std::error_code ec;
	fs::create_directories(outputDirectory);
	uint32_t numEntries = reader->GetDirEntryCount(dirHandle);
	for (uint32_t i = 0; i < numEntries; i++)
	{
		ZArchiveReader::DirEntry dirEntry;
		if (!reader->GetDirEntry(dirHandle, i, dirEntry))
		{
			g_progress.Break();
			puts("Directory contains invalid node");
			return false;
		}
		if (!g_progress.IsEnabled())
			puts(std::string(srcPath).append("/").append(dirEntry.name).c_str());
		if (dirEntry.isDirectory)
		{
			if (!ExtractRecursive(reader, std::string(srcPath).append("/").append(dirEntry.name), outputDirectory / dirEntry.name, buffer))
				return false;
		}
		else
		{
			// extract file
			if (!ExtractFile(reader, std::string(srcPath).append("/").append(dirEntry.name), outputDirectory / dirEntry.name, buffer))
				return false;
		}
	}
	return true;
}

int Extract(fs::path inputFile, fs::path outputDirectory)
{
	std::error_code ec;
	if (!fs::exists(inputFile, ec))
	{
		puts("Unable to find archive file");
		return -10;
	}

	ZArchiveReader* reader = ZArchiveReader::OpenFromFile(inputFile);
	if (!reader)
	{
		puts("Failed to open ZArchive");
		return -11;
	}
	std::vector<uint8_t> buffer;
	buffer.resize(256 * 1024); // shared by all files
	if (g_progress.IsEnabled())
		g_progress.Start(CountArchiveBytes(reader, ""));
	bool r = ExtractRecursive(reader, "", outputDirectory, buffer);
	if (!r)
	{
		g_progress.Break();
		puts("Extraction failed");
		delete reader;
		return -12;
	}
	g_progress.Finish();
	delete reader;
	return 0;
}

struct PackContext
{
	fs::path outputFilePath;
	std::ofstream currentOutputFile;
	bool hasError{false};
};

void _pack_NewOutputFile(const int32_t partIndex, void* ctx)
{
	PackContext* packContext = (PackContext*)ctx;
	packContext->currentOutputFile = std::ofstream(packContext->outputFilePath, std::ios::binary);
	if (!packContext->currentOutputFile.is_open())
	{
		printf("Failed to create output file: %s\n", packContext->outputFilePath.string().c_str());
		packContext->hasError = true;
	}
}

void _pack_WriteOutputData(const void* data, size_t length, void* ctx)
{
	PackContext* packContext = (PackContext*)ctx;
	packContext->currentOutputFile.write((const char*)data, length);
	if (!packContext->currentOutputFile.good() && !packContext->hasError)
	{
		// most likely the disk is full. The error is handled by Pack() which stops as soon as possible
		g_progress.Break();
		puts("Failed to write to the output file");
		packContext->hasError = true;
	}
}

// total size of all files that Pack() is going to add, used for the progress bar
static uint64_t CountInputBytes(const fs::path& inputDirectory, const fs::path& outputFile)
{
	uint64_t total = 0;
	std::error_code ec;
	for (fs::recursive_directory_iterator it(inputDirectory, ec), end; !ec && it != end; it.increment(ec))
	{
		std::error_code ec2;
		if (!it->is_regular_file(ec2) || ec2 || it->path() == outputFile)
			continue;
		uintmax_t size = it->file_size(ec2);
		if (!ec2)
			total += size;
	}
	return total;
}

int Pack(fs::path inputDirectory, fs::path outputFile, uint32_t threadCount, std::optional<int> compressionLevel)
{
	std::vector<uint8_t> buffer;
	buffer.resize(64 * 1024);

	std::error_code ec;
	PackContext packContext;
	packContext.outputFilePath = outputFile;
	ZArchiveWriter zWriter(_pack_NewOutputFile, _pack_WriteOutputData, &packContext);
	if (packContext.hasError)
		return -16;
	zWriter.SetThreadCount(threadCount);
	if (compressionLevel)
		zWriter.SetCompressionLevel(*compressionLevel);
	if (g_progress.IsEnabled())
		g_progress.Start(CountInputBytes(inputDirectory, outputFile));
	// fs::relative() has to canonicalize both paths for every single entry which involves file system accesses
	// the entries we get from the iterator all start with inputDirectory so a purely lexical operation is enough
	fs::path inputDirectoryBase = inputDirectory;
	if (!inputDirectoryBase.has_filename())
		inputDirectoryBase = inputDirectoryBase.parent_path(); // strip trailing separator
	for (auto const& dirEntry : fs::recursive_directory_iterator(inputDirectory))
	{
		fs::path pathEntry = dirEntry.path().lexically_relative(inputDirectoryBase);
		if (pathEntry.empty() || *pathEntry.begin() == fs::path(".."))
			pathEntry = fs::relative(dirEntry.path(), inputDirectory, ec);
		if (dirEntry.is_directory())
		{
			if (!zWriter.MakeDir(pathEntry.generic_string().c_str(), false))
			{
				g_progress.Break();
				printf("Failed to create directory %s\n", pathEntry.string().c_str());
				return -13;
			}
		}
		else if (dirEntry.is_regular_file())
		{
			if (dirEntry == outputFile) {
				continue;
			}
			if (!g_progress.IsEnabled())
				printf("Adding %s\n", pathEntry.string().c_str());
			if (!zWriter.StartNewFile(pathEntry.generic_string().c_str()))
			{
				g_progress.Break();
				printf("Failed to create archive file %s\n", pathEntry.string().c_str());
				return -14;
			}
			std::ifstream inputFile(inputDirectory / pathEntry, std::ios::binary);
			if (!inputFile.is_open())
			{
				g_progress.Break();
				printf("Failed to open input file %s\n", pathEntry.string().c_str());
				return -15;
			}
			while( true )
			{
				inputFile.read((char*)buffer.data(), buffer.size());
				int32_t readBytes = (int32_t)inputFile.gcount();
				if (readBytes <= 0)
					break;
				zWriter.AppendData(buffer.data(), readBytes);
				g_progress.Add((uint64_t)readBytes);
				if (packContext.hasError)
					break; // no point in continuing with this file
			}
		}
		if (packContext.hasError)
		{
			g_progress.Break();
			return -16;
		}
	}
	zWriter.Finalize();
	packContext.currentOutputFile.close();
	if (packContext.hasError || packContext.currentOutputFile.fail())
	{
		g_progress.Break();
		puts("Failed to write the output file");
		return -16;
	}
	g_progress.Finish();
	return 0;
}

int main(int argc, char* argv[])
{
	if (argc <= 1)
	{
		PrintHelp();
		return 0;
	}
	std::optional<std::string> strInput;
	std::optional<std::string> strOutput;
	uint32_t threadCount = 0; // 0 = automatic
	std::optional<int> compressionLevel;
	bool noProgress = false;
	for (int i = 1; i < argc; i++)
	{
		if (strcmp(argv[i], "--no-progress") == 0)
		{
			noProgress = true;
			continue;
		}
		if (strcmp(argv[i], "-l") == 0)
		{
			char* end = nullptr;
			long v = (i + 1 < argc) ? strtol(argv[i + 1], &end, 10) : 0;
			if (i + 1 >= argc || end == argv[i + 1] || *end != '\0' || v < -131072 || v > 22)
			{
				puts("Invalid or missing value for -l");
				return -1;
			}
			compressionLevel = (int)v;
			i++;
			continue;
		}
		if (strcmp(argv[i], "-j") == 0)
		{
			char* end = nullptr;
			unsigned long v = (i + 1 < argc) ? strtoul(argv[i + 1], &end, 10) : 0;
			if (i + 1 >= argc || end == argv[i + 1] || *end != '\0' || v == 0 || v > 1024)
			{
				puts("Invalid or missing value for -j");
				return -1;
			}
			threadCount = (uint32_t)v;
			i++;
			continue;
		}
		if (strInput)
		{
			if (strOutput)
			{
				puts("Too many paths specified");
				return -1;
			}
			else
			{
				strOutput = argv[i];
			}
		}
		else
		{
			strInput = argv[i];
		}
	}

	g_progress.Enable(!noProgress && StdoutIsTerminal());

	if (strInput)
	{
		std::error_code ec;
		fs::path p(*strInput);
		if (fs::is_regular_file(p, ec))
		{
			// extract
			fs::path outputDirectory;
			if (!strOutput)
			{
				fs::path defaultOutputPath = p.parent_path() / (p.stem().filename().string().append("_extracted"));
				outputDirectory = defaultOutputPath;
				printf("Extracting to: %s\n", outputDirectory.generic_string().c_str());
			}
			else
				outputDirectory = *strOutput;
			if (fs::exists(outputDirectory, ec) && !fs::is_directory(outputDirectory, ec))
			{
				puts("The specified output path is not a valid directory");
				return -3;
			}
			fs::create_directories(outputDirectory, ec);
			if (!fs::exists(outputDirectory, ec))
			{
				puts("Failed to create output directory");
				return -4;
			}
			return Extract(p, outputDirectory);
		}
		else if(fs::is_directory(p, ec))
		{
			// pack
			fs::path outputFile;
			if (!strOutput)
			{
				fs::path defaultOutputPath = p.parent_path() / (p.stem().filename().string().append(".zar"));
				outputFile = defaultOutputPath;
				printf("Outputting to: %s\n", outputFile.generic_string().c_str());
			}
			else
				outputFile = *strOutput;
			if ((fs::exists(outputFile, ec) && !fs::is_regular_file(outputFile, ec)))
			{
				puts("The specified output path is not a valid file");
				return -10;
			}
			if ((fs::exists(outputFile, ec) && fs::is_regular_file(outputFile, ec)))
			{
				puts("The output file already exists");
				return -11;
			}
			int r = Pack(p, outputFile, threadCount, compressionLevel);
			if (r != 0)
			{
				// delete incomplete output file
				fs::remove(outputFile, ec);
			}
			return r;
		}
		else
		{
			puts("Input path is not a valid file or directory");
			return -1;
		}
	}
	return 0;
}

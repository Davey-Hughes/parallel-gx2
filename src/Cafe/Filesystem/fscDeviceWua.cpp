#include "Cafe/Filesystem/fsc.h"
#include "Cafe/Filesystem/ZArchiveVFS.h"

class FSCDeviceWuaFileCtx : public FSCVirtualFile
{
	friend class fscDeviceWUAC;

protected:
	FSCDeviceWuaFileCtx(const ZArchive* archive, const ZArchive::Node& node, std::string_view path, uint32 fscType)
	{
		this->m_archive = archive;
		this->m_fscType = fscType;
		this->m_node = node;
		this->m_seek = 0;
		if (fscType == FSC_TYPE_FILE)
			m_file = std::make_unique<ZArchiveFile>(*archive, node);
		else
			archive->ListDir(path, m_entries); // from the index in memory
	};

public:
	sint32 fscGetType() override
	{
		return m_fscType;
	}

	uint32 fscDeviceWuaFile_getFileSize()
	{
		return (uint32)m_node.size;
	}

	uint64 fscQueryValueU64(uint32 id) override
	{
		if (m_fscType == FSC_TYPE_FILE)
		{
			if (id == FSC_QUERY_SIZE)
				return fscDeviceWuaFile_getFileSize();
			else if (id == FSC_QUERY_WRITEABLE)
				return 0; // WUD images are read-only
			else
				cemu_assert_error();
		}
		else
		{
			cemu_assert_unimplemented();
		}
		return 0;
	}

	uint32 fscWriteData(void* buffer, uint32 size) override
	{
		cemu_assert_error();
		return 0;
	}

	uint32 fscReadData(void* buffer, uint32 size) override
	{
		if (m_fscType != FSC_TYPE_FILE)
			return 0;
		cemu_assert(size < (2ULL * 1024 * 1024 * 1024)); // single read operation larger than 2GiB not supported
		uint32 bytesLeft = fscDeviceWuaFile_getFileSize() - m_seek;
		uint32 bytesToRead = (std::min)(bytesLeft, (uint32)size);
		uint32 bytesSuccessfullyRead = (uint32)m_file->Read(m_seek, buffer, bytesToRead);
		m_seek += bytesSuccessfullyRead;
		return bytesSuccessfullyRead;
	}

	void fscSetSeek(uint64 seek) override
	{
		if (m_fscType != FSC_TYPE_FILE)
			return;
		cemu_assert_debug(seek <= 0xFFFFFFFFULL);
		this->m_seek = (uint32)seek;
	}

	uint64 fscGetSeek() override
	{
		if (m_fscType != FSC_TYPE_FILE)
			return 0;
		return m_seek;
	}

	bool fscDirNext(FSCDirEntry* dirEntry) override
	{
		if (m_fscType != FSC_TYPE_DIRECTORY)
			return false;

		if (m_iteratorIndex >= m_entries.size())
			return false;
		const ZArchive::DirEntry& zarDirEntry = m_entries[m_iteratorIndex];
		m_iteratorIndex++;

		if (zarDirEntry.node.dir)
		{
			dirEntry->isDirectory = true;
			dirEntry->isFile = false;
			dirEntry->fileSize = 0;
		}
		else
		{
			dirEntry->isDirectory = false;
			dirEntry->isFile = true;
			dirEntry->fileSize = (uint32)zarDirEntry.node.size;
		}
		std::memset(dirEntry->path, 0, sizeof(dirEntry->path));
		std::strncpy(dirEntry->path, zarDirEntry.name.data(), std::min(sizeof(dirEntry->path) - 1, zarDirEntry.name.size()));
		return true;
	}

private:
	const ZArchive* m_archive{nullptr};
	sint32 m_fscType;
	ZArchive::Node m_node;
	// file: its own reader, as each open file reads on its own
	std::unique_ptr<ZArchiveFile> m_file;
	uint32 m_seek{0};
	// directory
	std::vector<ZArchive::DirEntry> m_entries;
	uint32 m_iteratorIndex{0};
};

class fscDeviceWUAC : public fscDeviceC
{
	FSCVirtualFile* fscDeviceOpenByPath(std::string_view path, FSC_ACCESS_FLAG accessFlags, void* ctx, sint32* fscStatus) override
	{
		const ZArchive* archive = (const ZArchive*)ctx;
		cemu_assert_debug(!HAS_FLAG(accessFlags, FSC_ACCESS_FLAG::WRITE_PERMISSION)); // writing to WUA is not supported

		const std::optional<ZArchive::Node> node = archive->LookUp(path);
		if (!node || !HAS_FLAG(accessFlags, node->dir ? FSC_ACCESS_FLAG::OPEN_DIR : FSC_ACCESS_FLAG::OPEN_FILE))
		{
			*fscStatus = FSC_STATUS_FILE_NOT_FOUND;
			return nullptr;
		}
		*fscStatus = FSC_STATUS_OK;
		return new FSCDeviceWuaFileCtx(archive, *node, path, node->dir ? FSC_TYPE_DIRECTORY : FSC_TYPE_FILE);
	}

	// singleton
public:
	static fscDeviceWUAC& instance()
	{
		static fscDeviceWUAC _instance;
		return _instance;
	}
};

bool FSCDeviceWUA_Mount(std::string_view mountPath, std::string_view destinationBaseDir, ZArchive* archive, sint32 priority)
{
	return fsc_mount(mountPath, destinationBaseDir, &fscDeviceWUAC::instance(), archive, priority) == FSC_STATUS_OK;
}
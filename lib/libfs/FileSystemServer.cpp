/*
 * Copyright (C) 2015 Niek Linnenbank
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <FreeNOS/User.h>
#include <Assert.h>
#include <Vector.h>
#include <HashTable.h>
#include <HashIterator.h>
#include <DatastoreClient.h>
#include "FileSystemClient.h"
#include "FileSystemMount.h"
#include "FileSystemServer.h"

FileSystemServer::FileSystemServer(Directory *root, const char *path)
    : ChannelServer<FileSystemServer, FileSystemMessage>(this)
    , m_pid(ProcessCtl(SELF, GetPID))
    , m_root(ZERO)
    , m_mountPath(path)
    , m_mounts(ZERO)
    , m_requests(new List<FileSystemRequest *>())
{
    setRoot(root);

    // Register message handlers
    addIPCHandler(FileSystem::CreateFile, &FileSystemServer::pathHandler, false);
    addIPCHandler(FileSystem::StatFile,   &FileSystemServer::pathHandler, false);
    addIPCHandler(FileSystem::DeleteFile, &FileSystemServer::pathHandler, false);
    addIPCHandler(FileSystem::ReadFile,   &FileSystemServer::pathHandler, false);
    addIPCHandler(FileSystem::WriteFile,  &FileSystemServer::pathHandler, false);
    addIPCHandler(FileSystem::MountFileSystem, &FileSystemServer::mountHandler);
    addIPCHandler(FileSystem::WaitFileSystem,  &FileSystemServer::pathHandler, false);
    addIPCHandler(FileSystem::GetFileSystems,  &FileSystemServer::getFileSystemsHandler);
}

FileSystemServer::~FileSystemServer()
{
    if (m_requests)
    {
        delete m_requests;
    }

    clearFileCache(m_root);
}

const char * FileSystemServer::getMountPath() const
{
    return m_mountPath;
}

FileSystem::Result FileSystemServer::mount()
{
    // The rootfs server manages the mounts table. Retrieve it from the datastore.
    if (m_pid == ROOTFS_PID)
    {
        const DatastoreClient datastore;
        const Datastore::Result result =
            datastore.registerBuffer("mounts", &m_mounts, sizeof(FileSystemMount) * MaximumFileSystemMounts);

        if (result != Datastore::Success)
            return FileSystem::IOError;

        assert(m_mounts != NULL);

        // Fill the mounts table
        MemoryBlock::set(m_mounts, 0, sizeof(FileSystemMount) * MaximumFileSystemMounts);
        return FileSystem::Success;
    }
    // Other file systems send a request to root file system to mount.
    else
    {
        const FileSystemClient rootfs(ROOTFS_PID);
        const FileSystem::Result result = rootfs.mountFileSystem(m_mountPath);

        assert(result == FileSystem::Success);
        return result;
    }
}

File * FileSystemServer::createFile(const FileSystem::FileType type,
                                    const DeviceID deviceID)
{
    return (File *) ZERO;
}

FileSystem::Result FileSystemServer::registerFile(File *file, const char *path)
{
    // Add to the filesystem cache
    FileCache *cache = insertFileCache(file, path);
    if (cache == ZERO)
    {
        return FileSystem::IOError;
    }

    // Also add to the parent directory
    const FileSystemPath p(path);
    Directory *parent = ZERO;

    if (p.parent().length() > 0)
    {
        FileCache *cache = findFileCache(*p.parent());
        if (cache != ZERO)
        {
            parent = static_cast<Directory *>(cache->file);
        }
    }
    else
    {
        parent = static_cast<Directory *>(m_root->file);
    }

    if (parent != ZERO)
    {
        parent->insert(file->getType(), *p.base());
        return FileSystem::Success;
    }
    else
    {
        return FileSystem::NotFound;
    }
}

void FileSystemServer::pathHandler(FileSystemMessage *msg)
{
    // Prepare request
    FileSystemRequest req(msg);

    // Process the request.
    if (processRequest(req) == FileSystem::RetryAgain)
    {
        FileSystemRequest *reqCopy = new FileSystemRequest(msg);
        assert(reqCopy != NULL);
        m_requests->append(reqCopy);
    }
}

bool FileSystemServer::redirectRequest(const char *path, FileSystemMessage *msg)
{
    Size savedMountLength = 0;
    FileSystemMount *mnt = ZERO;

    // Search for the longest matching mount
    for (Size i = 0; i < MaximumFileSystemMounts; i++)
    {
        if (m_mounts[i].path[0] != ZERO)
        {
            const String mountStr(m_mounts[i].path, false);
            const Size mountStrLen = mountStr.length();

            if (mountStrLen > savedMountLength && mountStr.compareTo(path, true, mountStrLen) == 0)
            {
                savedMountLength = mountStrLen;
                mnt = &m_mounts[i];
            }
        }
    }

    // If no match was found, no redirect is needed
    if (!mnt)
        return false;

    msg->type = ChannelMessage::Response;

    if (msg->action == FileSystem::WaitFileSystem)
        msg->result = FileSystem::Success;
    else
        msg->result = FileSystem::RedirectRequest;
    msg->pid = mnt->procID;
    msg->pathMountLength = savedMountLength;

    sendResponse(msg);
    return true;
}

FileSystem::Result FileSystemServer::processRequest(FileSystemRequest &req)
{
    char buf[FileSystemPath::MaximumLength];
    FileCache *cache = ZERO;
    File *file = ZERO;
    Directory *parent;
    FileSystemMessage *msg = req.getMessage();
    Error ret;

    // Copy the file path
    if ((ret = VMCopy(msg->from, API::Read, (Address) buf,
                    (Address) msg->path, FileSystemPath::MaximumLength)) <= 0)
    {
        ERROR("VMCopy failed: result = " << (int)ret << " from = " << msg->from <<
              " addr = " << (void *) msg->path << " action = " << (int) msg->action);
        msg->result = FileSystem::IOError;
        sendResponse(msg);
        return msg->result;
    }
    DEBUG(m_self << ": path = " << buf << " action = " << msg->action);

    // Handle mounted file system paths (will result in redirect messages)
    if (m_pid == ROOTFS_PID && redirectRequest(buf, msg))
    {
        DEBUG(m_self << ": redirect " << buf << " to " << msg->pid);
        return msg->result;
    }

    const FileSystemPath path(buf + String::length(m_mountPath));

    // Do we have this file cached?
    if ((cache = findFileCache(path)) ||
        (cache = lookupFile(path)))
    {
        file = cache->file;
    }
    // File not found
    else if (msg->action != FileSystem::CreateFile && msg->action != FileSystem::WaitFileSystem)
    {
        DEBUG(m_self << ": not found");
        msg->result = FileSystem::NotFound;
        sendResponse(msg);
        return msg->result;
    }

    // Perform I/O on the file
    switch (msg->action)
    {
        case FileSystem::CreateFile:
            if (cache)
                msg->result = FileSystem::AlreadyExists;
            else
            {
                /* Attempt to create the new file. */
                if ((file = createFile(msg->filetype, msg->deviceID)))
                {
                    insertFileCache(file, *path.full());

                    /* Add directory entry to our parent. */
                    if (path.parent().length() > 0)
                    {
                        parent = (Directory *) findFileCache(*path.parent())->file;
                    }
                    else
                        parent = (Directory *) m_root->file;

                    parent->insert(file->getType(), *path.full());
                    msg->result = FileSystem::Success;
                }
                else
                    msg->result = FileSystem::IOError;
            }
            DEBUG(m_self << ": create = " << (int)msg->result);
            break;

        case FileSystem::DeleteFile:
            if (cache->entries.count() == 0)
            {
                clearFileCache(cache);
                msg->result = FileSystem::Success;
            }
            else
                msg->result = FileSystem::PermissionDenied;
            DEBUG(m_self << ": delete = " << (int)msg->result);
            break;

        case FileSystem::StatFile:
            if (file->status(msg) == FileSystem::Success)
            {
                msg->result = FileSystem::Success;
            }
            else
            {
                msg->result = FileSystem::IOError;
            }
            DEBUG(m_self << ": stat = " << (int)msg->result);
            break;

        case FileSystem::ReadFile: {
            if ((ret = file->read(req.getBuffer(), msg->size, msg->offset)) >= 0)
            {
                msg->size = ret;
                msg->result = FileSystem::Success;

                if (req.getBuffer().getCount())
                    req.getBuffer().flush();
            }
            else if (ret == FileSystem::RetryAgain)
            {
                msg->result = FileSystem::RetryAgain;
            }
            else
            {
                msg->result = FileSystem::IOError;
            }
            DEBUG(m_self << ": read = " << (int)msg->result);
            break;
        }

        case FileSystem::WriteFile: {
            if (!req.getBuffer().getCount())
                req.getBuffer().bufferedRead();

            if ((ret = file->write(req.getBuffer(), msg->size, msg->offset)) >= 0)
            {
                msg->size = ret;
                msg->result = FileSystem::Success;
            }
            else if (ret == FileSystem::RetryAgain)
            {
                msg->result = FileSystem::RetryAgain;
            }
            else
            {
                msg->result = FileSystem::IOError;
            }
            DEBUG(m_self << ": write = " << (int)msg->result);
            break;
        }

        case FileSystem::WaitFileSystem: {
            // Do nothing here. Once the targeted file system is mounted
            // this function will send a redirect message when called again
            DEBUG(m_self << ": wait for " << buf);
            msg->result = FileSystem::RetryAgain;
            break;
        }

        default: {
            ERROR("unhandled file I/O operation: " << (int)msg->action);
            msg->result = FileSystem::NotSupported;
            break;
        }
    }

    // Only send reply if completed (not RetryAgain)
    if (msg->result != FileSystem::RetryAgain)
    {
        sendResponse(msg);
    }

    return msg->result;
}

void FileSystemServer::sendResponse(FileSystemMessage *msg) const
{
    msg->type = ChannelMessage::Response;

    Channel *channel = m_registry.getProducer(msg->from);
    if (channel == ZERO)
    {
        ERROR("failed to retrieve channel for PID " << msg->from);
        return;
    }

    const Channel::Result result = channel->write(msg);
    if (result != Channel::Success)
    {
        ERROR("failed to write channel for PID " << msg->from);
        return;
    }

    ProcessCtl(msg->from, Wakeup, 0);
}

void FileSystemServer::mountHandler(FileSystemMessage *msg)
{
    char buf[FileSystemPath::MaximumLength + 1];

    // Copy the file path
    const API::Result result = VMCopy(msg->from, API::Read, (Address) buf,
                                     (Address) msg->path, FileSystemPath::MaximumLength);
    if (result <= 0)
    {
        ERROR("failed to copy mount path: result = " << (int) result);
        msg->result = FileSystem::IOError;
        return;
    }

    // Null-terminate
    buf[FileSystemPath::MaximumLength] = 0;
    const String path(buf);

    // Check for already existing entry (re-mount)
    for (Size i = 0; i < MaximumFileSystemMounts; i++)
    {
        const String entry(m_mounts[i].path);

        if (path.equals(entry))
        {
            m_mounts[i].procID  = msg->from;
            m_mounts[i].options = ZERO;
            NOTICE("remounted " << m_mounts[i].path);
            msg->result = FileSystem::Success;
            return;
        }
    }

    // Append to our filesystem mounts table
    for (Size i = 0; i < MaximumFileSystemMounts; i++)
    {
        if (!m_mounts[i].path[0])
        {
            MemoryBlock::copy(m_mounts[i].path, buf, sizeof(m_mounts[i].path));
            m_mounts[i].procID = msg->from;
            m_mounts[i].options = ZERO;
            NOTICE("mounted " << m_mounts[i].path);
            msg->result = FileSystem::Success;
            return;
        }
    }

    // Not space left
    msg->result = FileSystem::IOError;
}

void FileSystemServer::getFileSystemsHandler(FileSystemMessage *msg)
{
    // Copy mounts table to the requesting process
    const Size mountsSize = sizeof(FileSystemMount) * MaximumFileSystemMounts;
    const Size numBytes = msg->size < mountsSize ? msg->size : mountsSize;
    const API::Result result = VMCopy(msg->from, API::Write, (Address) m_mounts,
                                     (Address) msg->buffer, numBytes);
    if (result <= 0)
    {
        ERROR("failed to copy mount table: result = " << (int) result);
        msg->result = FileSystem::IOError;
        return;
    }

    msg->result = FileSystem::Success;
}

bool FileSystemServer::retryRequests()
{
    bool restartNeeded = false;

    DEBUG("");

    for (ListIterator<FileSystemRequest *> i(m_requests); i.hasCurrent(); i++)
    {
        FileSystem::Result result = processRequest(*i.current());
        if (result != FileSystem::RetryAgain)
        {
            delete i.current();
            i.remove();
            restartNeeded = true;
        }
    }

    return restartNeeded;
}

void FileSystemServer::setRoot(Directory *newRoot)
{
    if (newRoot != ZERO)
    {
        m_root = new FileCache(newRoot, "/", ZERO);
        insertFileCache(newRoot, ".");
        insertFileCache(newRoot, "..");
    }
}

FileCache * FileSystemServer::lookupFile(const FileSystemPath &path)
{
    const List<String> &entries = path.split();
    FileCache *c = m_root;
    File *file = ZERO;
    Directory *dir;

    // Loop the entire path
    for (ListIterator<String> i(entries); i.hasCurrent(); i++)
    {
        // Do we have this entry cached already?
        if (!c->entries.contains(i.current()))
        {
            // If this isn't a directory, we cannot perform a lookup
            if (c->file->getType() != FileSystem::DirectoryFile)
            {
                return ZERO;
            }
            dir = (Directory *) c->file;

            // Fetch the file, if possible
            if (!(file = dir->lookup(*i.current())))
            {
                return ZERO;
            }
            // Insert into the FileCache
            c = new FileCache(file, *i.current(), c);
            assert(c != NULL);
        }
        // Move to the next entry
        else if (c != ZERO)
        {
            c = (FileCache *) c->entries.value(i.current());
        }
        else
        {
            break;
        }
    }

    // All done
    return c;
}

FileCache * FileSystemServer::insertFileCache(File *file, const char *pathStr)
{
    const FileSystemPath path(pathStr);
    FileCache *parent = ZERO;

    // Lookup our parent
    if (path.parent().length() == 0)
    {
        parent = m_root;
    }
    else if (!(parent = findFileCache(path.parent())))
    {
        return ZERO;
    }

    // Create new cache
    FileCache *c = new FileCache(file, *path.base(), parent);
    assert(c != NULL);
    return c;
}

FileCache * FileSystemServer::findFileCache(const char *path) const
{
    const FileSystemPath p(path);
    return findFileCache(p);
}

FileCache * FileSystemServer::findFileCache(const String &path) const
{
    return findFileCache(*path);
}

FileCache * FileSystemServer::findFileCache(const FileSystemPath &path) const
{
    const List<String> &entries = path.split();
    FileCache *c = m_root;

    // Root is treated special
    if (path.parent().length() == 0 && path.length() == 0)
    {
        return m_root;
    }

    // Loop the entire path
    for (ListIterator<String> i(entries); i.hasCurrent(); i++)
    {
        if (!c->entries.contains(i.current()))
        {
            return ZERO;
        }
        c = (FileCache *) c->entries.value(i.current());
    }

    // Return what we got
    return c;
}

void FileSystemServer::removeFileFromCache(FileCache *cache, File *file)
{
    assert(cache != ZERO);
    assert(file != ZERO);

    // Walk all our childs
    for (HashIterator<String, FileCache *> i(cache->entries); i.hasCurrent(); i++)
    {
        FileCache *child = i.current();

        if (child->file == cache->file)
        {
            static_cast<Directory *>(cache->file)->remove(*child->name);
            removeFileFromCache(child, file);
            child->file = ZERO;
        }
    }
}

void FileSystemServer::clearFileCache(FileCache *cache)
{
    // Start from root?
    if (!cache)
    {
        cache = m_root;
    }

    // Make sure the current file is removed from all childs and below
    if (cache->file != ZERO)
    {
        removeFileFromCache(cache, cache->file);
    }

    // Walk all our childs
    for (HashIterator<String, FileCache *> i(cache->entries); i.hasCurrent();)
    {
        FileCache *child = i.current();

        static_cast<Directory *>(cache->file)->remove(*child->name);
        clearFileCache(child);
        i.remove();
    }

    // Cleanup this cache object
    assert(cache->entries.count() == 0);

    if (cache->file != ZERO)
    {
        // Remove entry from parent */
        if (cache->parent)
        {
            if (cache->parent->file)
            {
                static_cast<Directory *>(cache->parent->file)->remove(*cache->name);
            }
            cache->parent->entries.remove(cache->name);
        }

        delete cache->file;
    }
    delete cache;
}

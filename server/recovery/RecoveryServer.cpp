/*
 * Copyright (C) 2020 Niek Linnenbank
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

#include <FreeNOS/System.h>
#include <Log.h>
#include <FileSystemClient.h>
#include <ExecutableFormat.h>
#include "RecoveryServer.h"

RecoveryServer::RecoveryServer()
    : ChannelServer<RecoveryServer, RecoveryMessage>(this)
{
    addIPCHandler(Recovery::RestartProcess, &RecoveryServer::restartProcess);
}

void RecoveryServer::restartProcess(RecoveryMessage *msg)
{
    DEBUG("pid = " << msg->pid);

    // The identifier must be in range of allowed values
    if (msg->pid >= MAX_PROCS)
    {
        ERROR("invalid PID " << msg->pid);
        msg->result = Recovery::InvalidArgument;
        return;
    }

    // Retrieve state information of the given process
    ProcessInfo pinfo;
    API::Result result = ProcessCtl(msg->pid, InfoPID, (Address) &pinfo);
    if (result != API::Success)
    {
        ERROR("failed to retrieve ProcessInfo for PID " << msg->pid <<
              ": result = " << (int)result);
        msg->result = Recovery::NotFound;
        return;
    }

    // Halt the process. Brings it out of the scheduler
    // and also prevents other processes from waking it up while
    // it is being restarted by us.
    result = ProcessCtl(msg->pid, Stop);
    if (result != API::Success)
    {
        ERROR("failed to stop PID " << msg->pid <<
              ": result = " << (int) result);
        msg->result = Recovery::IOError;
        return;
    }

    // Retrieve full command string
    char cmd[128];
    const Arch::MemoryMap map;
    const Memory::Range argRange = map.range(MemoryMap::UserArgs);

    if (VMCopy(msg->pid, API::Read, (Address) cmd, argRange.virt, sizeof(cmd)) != sizeof(cmd))
    {
        ERROR("failed to read command string from PID " << msg->pid);
        msg->result = Recovery::IOError;
        return;
    }
    cmd[sizeof(cmd) - 1] = 0;

    // Fetch program path from command string
    List<String> progpath = String(cmd).split(' ');
    if (progpath.count() == 0)
    {
        ERROR("failed to find program path for PID " << msg->pid);
        msg->result = Recovery::IOError;
        return;
    }

    // Write fresh copy of the program inside
    if (!reloadProgram(msg->pid, *progpath[0]))
    {
        ERROR("failed to reload PID " << msg->pid <<
              " from " << *progpath[0] <<
              ": result = " << (int) result);
        msg->result = Recovery::IOError;
        return;
    }

    // Continue program
    result = ProcessCtl(msg->pid, Resume);
    if (result != API::Success)
    {
        ERROR("failed to resume PID " << msg->pid <<
              ": result = " << (int) result);
        msg->result = Recovery::IOError;
        return;
    }

    // Success
    msg->result = Recovery::Success;
}

bool RecoveryServer::reloadProgram(const ProcessID pid,
                                   const char *path) const
{
    const FileSystemClient fs;
    FileSystem::FileStat st;
    bool result = true;

    DEBUG("pid = " << pid << " path = " << path);

    // Retrieve file information
    const FileSystem::Result statResult = fs.statFile(path, &st);
    if (statResult != FileSystem::Success)
    {
        ERROR("failed to statFile() for " << path << ": result = " << (int) statResult);
        return false;
    }

    // Map memory buffer for the program image
    Memory::Range range;
    range.virt   = ZERO;
    range.phys   = ZERO;
    range.size   = st.size;
    range.access = Memory::User|Memory::Readable|Memory::Writable;

    // Create memory mapping
    const API::Result mapResult = VMCtl(SELF, MapContiguous, &range);
    if (mapResult != API::Success)
    {
        ERROR("failed to map memory: result = " << (int) mapResult);
        return false;
    }

    // Read the program image
    Size num = st.size;
    const FileSystem::Result readResult = fs.readFile(path, (void *) range.virt, &num, 0);
    if (readResult != FileSystem::Success || num != st.size)
    {
        ERROR("failed to readFile() for " << path <<
              ": result = " << (int) readResult << ", num = " << num);
        result = false;
    }

    if (result)
    {
        // Release current memory pages
        result = cleanupProgram(pid);
        if (!result)
        {
            ERROR("failed to cleanup program data for PID " << pid);
        }
        else
        {
            // Write to program
            result = rewriteProgram(pid, range.virt, num);
            if (!result)
            {
                ERROR("failed to reset data for PID " << pid);
            }
        }
    }

    // Cleanup program buffer
    const API::Result releaseResult = VMCtl(SELF, Release, &range);
    if (releaseResult != API::Success)
    {
        DEBUG("failed to unmap memory: result = " << (int) releaseResult);
        return false;
    }

    // Success
    return result;
}

bool RecoveryServer::cleanupProgram(const ProcessID pid) const
{
    const Arch::MemoryMap map;
    Memory::Range range;

    DEBUG("pid = " << pid);

    range = map.range(MemoryMap::UserData);
    const API::Result dataResult = VMCtl(pid, Release, &range);
    if (dataResult != API::Success)
    {
        ERROR("failed to release UserData region in PID " << pid <<
              ": result = " << (int) dataResult);
        return false;
    }

    range = map.range(MemoryMap::UserHeap);
    const API::Result heapResult = VMCtl(pid, Release, &range);
    if (heapResult != API::Success)
    {
        ERROR("failed to release UserHeap region in PID " << pid <<
              ": result = " << (int) heapResult);
        return false;
    }

    range = map.range(MemoryMap::UserPrivate);
    const API::Result privResult = VMCtl(pid, Release, &range);
    if (privResult != API::Success)
    {
        ERROR("failed to release UserPrivate region in PID " << pid <<
              ": result = " << (int) privResult);
        return false;
    }

    return true;
}

bool RecoveryServer::rewriteProgram(const ProcessID pid,
                                    const Address program,
                                    const Size size) const
{
    ExecutableFormat *fmt;
    ExecutableFormat::Region regions[16];
    Arch::MemoryMap map;
    Memory::Range range;
    Size numRegions = 16;
    Address entry = 0;

    DEBUG("pid = " << pid << " program = " << (void *) program << " size = " << size);

    // Attempt to read executable format
    const ExecutableFormat::Result execResult =
        ExecutableFormat::find((u8 *) program, size, &fmt);

    if (execResult != ExecutableFormat::Success)
    {
        ERROR("failed to parse executable: result = " << (int) execResult);
        return false;
    }

    // Find entry point
    const ExecutableFormat::Result entryResult = fmt->entry(&entry);
    if (entryResult != ExecutableFormat::Success)
    {
        ERROR("failed to retrieve entry point: result = " << (int) entryResult);
        delete fmt;
        return false;
    }

    // Retrieve memory regions
    const ExecutableFormat::Result regionResult = fmt->regions(regions, &numRegions);
    if (regionResult != ExecutableFormat::Success)
    {
        ERROR("failed to retrieve regions: result = " << (int) regionResult);
        delete fmt;
        return false;
    }

    // Release buffers
    delete fmt;

    // Map program regions into virtual memory of the new process
    for (Size i = 0; i < numRegions; i++)
    {
        // Setup memory range to copy region data
        range.virt   = regions[i].virt;
        range.phys   = ZERO;
        range.size   = regions[i].memorySize;
        range.access = regions[i].access;

        // Create mapping in the process
        const API::Result mapResult = VMCtl(pid, MapContiguous, &range);
        if (mapResult != API::Success)
        {
            ERROR("failed to map " << (void *) regions[i].virt <<
                  " in PID " << pid << ": result = " << (int) mapResult);
            return false;
        }

        // Map inside our process
        range.virt = ZERO;
        const API::Result selfResult = VMCtl(SELF, MapContiguous, &range);
        if (selfResult != API::Success)
        {
            ERROR("failed to map " << (void *) regions[i].virt <<
                  ": result = " << (int) selfResult);
            return false;
        }

        // Copy data bytes
        MemoryBlock::copy((void *)range.virt, (const void *)(program + regions[i].dataOffset),
                          regions[i].dataSize);

        // Nulify remaining space
        if (regions[i].memorySize > regions[i].dataSize)
        {
            MemoryBlock::set((void *)(range.virt + regions[i].dataSize), 0,
                             regions[i].memorySize - regions[i].dataSize);
        }

        // Remove temporary mapping
        const API::Result unmapResult = VMCtl(SELF, UnMap, &range);
        if (unmapResult != API::Success)
        {
            ERROR("failed to unmap " << (void *) regions[i].virt <<
                  ": result = " << (int) unmapResult);
            return false;
        }
    }

    // Reset program registers
    const API::Result resetResult = ProcessCtl(pid, Reset, entry);
    if (resetResult != API::Success)
    {
        ERROR("failed to reset PID " << pid <<
              ": result = " << (int) resetResult);
        return false;
    }

    // Success
    return true;
}

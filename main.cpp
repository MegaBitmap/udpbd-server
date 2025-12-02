#include <iostream>
#include <exception>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>

#ifdef __linux__
#include <arpa/inet.h>
#include <sys/socket.h>
#elif defined(_WIN32) || defined(__WIN32__) || defined(WIN32)
#include <conio.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include "udpbd.h"

#define BUFLEN 2048

#if defined(__APPLE__) || defined( __FreeBSD__)
#include <sys/ioctl.h>
#include <sys/disk.h>
#define lseek64 lseek
#define loff_t off_t
#endif

#if defined(__APPLE__)
#define _DARWIN_USE_64_BIT_INODE 1
#endif

std::runtime_error ErrorMessage()
{
    int errorNum = GetLastError();
    _cprintf("Error code %d\n%s\n", errorNum,
    std::system_category().message(errorNum).c_str());
    return std::runtime_error(std::to_string(errorNum));
}

/*
 * class CBlockDevice
 */
class CBlockDevice
{
public:
    CBlockDevice(const char *sFileName) : _read_only(false)
    {
        // Open the selected file.
        // This file will be used as the Block Device
        _fp = CreateFileA(sFileName, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);

        if (_fp == INVALID_HANDLE_VALUE)
        {
            _cprintf("unable to open file %s\n", sFileName);
            throw ErrorMessage();
        }

        DWORD status;
        // lock volume
        if (!DeviceIoControl(_fp, FSCTL_LOCK_VOLUME,
                             NULL, 0, NULL, 0, &status, NULL))
        {
            _cprintf("Error attempting to lock device\n");
           throw ErrorMessage();
        }

        if (!DeviceIoControl(_fp, FSCTL_DISMOUNT_VOLUME,
                             NULL, 0, NULL, 0, &status, NULL))
        {
            _cprintf("Error attempting to dismount volume\n");
            throw ErrorMessage();
        }

        // Get disk geometry for sector size
        DWORD junk = 0;
        DISK_GEOMETRY pdg;
        BOOL bResult = DeviceIoControl(_fp,                           // device to be queried
                                       IOCTL_DISK_GET_DRIVE_GEOMETRY, // operation to perform
                                       NULL, 0,                       // no input buffer
                                       &pdg, sizeof(pdg),             // output buffer
                                       &junk,                         // # bytes returned
                                       (LPOVERLAPPED)NULL);

        if (bResult)
        {
            sector_size = pdg.BytesPerSector; // Use actual sector size from disk
        }
        else
        {
            sector_size = 512; // Fallback to standard sector size
            _cprintf("Warning: Could not get disk geometry, using default sector size 512\n");
        }

        // Get the volume/partition size
        PARTITION_INFORMATION_EX partInfo;

        bResult = DeviceIoControl(_fp,                              // device to be queried
                                  IOCTL_DISK_GET_PARTITION_INFO_EX, // operation to perform
                                  NULL, 0,                          // no input buffer
                                  &partInfo, sizeof(partInfo),      // output buffer
                                  &junk,                            // # bytes returned
                                  (LPOVERLAPPED)NULL);

        if (bResult)
        {
            _fsize = partInfo.PartitionLength.QuadPart;
        }
        else
        {
            // Fallback to disk length info
            GET_LENGTH_INFORMATION lengthInfo;
            bResult = DeviceIoControl(_fp,                            // device to be queried
                                     IOCTL_DISK_GET_LENGTH_INFO,      // operation to perform
                                     NULL, 0,                         // no input buffer
                                     &lengthInfo, sizeof(lengthInfo), // output buffer
                                     &junk,                           // # bytes returned
                                     (LPOVERLAPPED)NULL);
            if (bResult)
                _fsize = lengthInfo.Length.QuadPart;
        }

        if (bResult)
        {            
            _cprintf("Opened '%s' as Block Device\n", sFileName);
            _cprintf(" - %s\n", _read_only ? "read-only" : "read/write");
            _cprintf(" - size = %lldMB / %lldMiB, sector size = %lld\n", _fsize / (1000 * 1000), _fsize / (1024 * 1024), sector_size);
        }
        else
        {
            _cprintf("Error getting volume/disk size\n");
            throw ErrorMessage();
        }
        fflush(stdout);
    }

    ~CBlockDevice()
    {
        CloseHandle(_fp);
    }

    void seek(uint32_t sector)
    {
        _off64_t offset = (_off64_t)sector * sector_size;
        LONG high, low;
        low = (offset);
        high = (offset >> 32);
        // _cprintf("seek %d * 512 = %ld\n", sector, offset);
        SetFilePointer(_fp, low, &high, FILE_BEGIN);
    }

    void read(void *data, size_t size)
    {

        size_t new_size, remainder, aux_size;
        DWORD rv;

        // Size is re-computed as factor of sector size
        new_size = (size - sector_offset);
        remainder = new_size % sector_size;
        aux_size = sector_size * (new_size / sector_size + (remainder == 0 ? 0 : 1));

        BOOL ret = ReadFile(_fp, (char *)data + sector_offset, aux_size, &rv, NULL);

        if (ret == true)
        {
            // If previous call had remaining bytes, use them in this call
            if (sector_offset)
            {
                memcpy(data, sector_buffer, sector_offset);
            }

            // If this call has remaining bytes, store them for next call
            if (remainder != 0)
            {
                memcpy(sector_buffer, (char *)data + size, sector_size - remainder);
                sector_offset = (sector_size - remainder);
            }
            else
                sector_offset = 0;
        }
        else
        {
            _cprintf("An error occured while trying to read sectors\n");
            throw ErrorMessage();
        }
    }

    // TODO: This method is not optimized, nonetheless in game write operations are not critical
    void write(const void *data, size_t size)
    {
        DWORD rv;
        LONG high, low;

        // Size is re-computed as factor of sector size
        size_t aux_size = sector_size * (size / sector_size + ((size % sector_size) != 0 ? 1 : 0));

        // First read sectors
        low = SetFilePointer(_fp, 0, &high, FILE_CURRENT);
        BOOL ret = ReadFile(_fp, sector_buffer, aux_size, &rv, NULL);

        // Then inject received data
        memcpy(sector_buffer, data, size);

        // Finally write sectors
        SetFilePointer(_fp, low, &high, FILE_BEGIN);
        ret = WriteFile(_fp, sector_buffer, aux_size, &rv, NULL);
        // _cprintf("write %ld\n", size);
        if (ret == 0)
        {
            _cprintf("An error occured while trying to write sectors\n");
            _cprintf("write error %ld != %llu\n", rv, size);
            throw ErrorMessage();
        }
    }

    uint32_t get_sector_size() { return sector_size; }
    uint32_t get_sector_count() { return _fsize / sector_size; }
    void clear_sector_offset(){sector_offset=0;}

private:
    bool _read_only;
    HANDLE _fp;
    size_t sector_size;
    size_t sector_offset;
    uint8_t sector_buffer[4 * 512];
    _off64_t _fsize;
};

/*
 * class CUDPBDServer
 */
class CUDPBDServer
{
public:
    CUDPBDServer(class CBlockDevice &bd) : _bd(bd), _block_shift(0), _total_read(0), _total_write(0)
    {
        set_block_shift(5); // 128b blocks
        struct sockaddr_in si_me;

        WORD wVersionRequested;
        WSADATA wsaData;
        int err;

        /* Use the MAKEWORD(lowbyte, highbyte) macro declared in Windef.h */
        wVersionRequested = MAKEWORD(2, 2);

        err = WSAStartup(wVersionRequested, &wsaData);
        if (err != 0)
        {
            /* Tell the user that we could not find a usable */
            /* Winsock DLL.                                  */
            _cprintf("WSAStartup failed\n");
            throw ErrorMessage();
        }

        // create a UDP socket
        if ((s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == -1)
        {
            _cprintf("Failed to create a UDP socket\n");
            throw ErrorMessage();
        }

        // bind socket to port
        memset((char *)&si_me, 0, sizeof(si_me));
        si_me.sin_family = AF_INET;
        si_me.sin_port = htons(UDPBD_PORT);
        si_me.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(s, (struct sockaddr*)&si_me, sizeof(si_me)) == -1)
        {
            _cprintf("Failed to bind the socket to a port\n");
            throw ErrorMessage();
        }

        // Enable broadcasts
        int broadcastEnable = 1;
        setsockopt(s, SOL_SOCKET, SO_BROADCAST, (char *)&broadcastEnable, sizeof(broadcastEnable));
    }

    ~CUDPBDServer()
    {
        close(s);
    }

    void run()
    {
        bool wait_network = true;
        struct sockaddr_in si_other;
        socklen_t slen = sizeof(si_other);
        int recv_len;
        char buf[BUFLEN];

        _cprintf("Server running on port %d (0x%x)\n", UDPBD_PORT, UDPBD_PORT);

        // Start server loop
        while (1)
        {
            // Receive command from ps2
            if ((recv_len = recvfrom(s, buf, BUFLEN, 0, (struct sockaddr *)&si_other, &slen)) == -1)
            {
                _cprintf("Failed to receive a message from the PS2\n");
                throw ErrorMessage();
            }

            struct SUDPBDv2_Header *hdr = (struct SUDPBDv2_Header *)buf;

            if (wait_network)
            {
                wait_network = false;
                _cprintf("Waiting for the network to fully initialize . . .\n");
                Sleep(4000);
            }

            // Process command
            switch (hdr->cmd)
            {
            case UDPBD_CMD_INFO:
                handle_cmd_info(si_other, (struct SUDPBDv2_InfoRequest *)buf);
                break;
            case UDPBD_CMD_READ:
                handle_cmd_read(si_other, (struct SUDPBDv2_RWRequest *)buf);
                break;
            case UDPBD_CMD_WRITE:
                handle_cmd_write(si_other, (struct SUDPBDv2_RWRequest *)buf);
                break;
            case UDPBD_CMD_WRITE_RDMA:
                handle_cmd_write_rdma(si_other, (struct SUDPBDv2_RDMA *)buf);
                break;
            default:
                _cprintf("Invalid cmd: 0x%x\n", hdr->cmd);
            };
        }
    }

private:
    void print_stats()
    {
        _cprintf(" Total read: %llu KiB, total write: %llu KiB\r", _total_read/1024, _total_write/1024);
        fflush(stdout);
    }

    void set_block_shift(uint32_t shift)
    {
        if (shift != _block_shift)
        {
            _block_shift       = shift;
            _block_size        = 1 << (_block_shift + 2);
            _blocks_per_packet = RDMA_MAX_PAYLOAD / _block_size;
            _blocks_per_sector = _bd.get_sector_size() / _block_size;
            _cprintf("Block size changed to %d\n", _block_size);
        }
    }

    void set_block_shift_sectors(uint32_t sectors)
    {
        // Optimize for:
        // 1 - the least number of network packets
        // 2 - the largest block size (faster on the ps2)
        uint32_t shift;
        uint32_t size = sectors * 512;
        uint32_t packetsMIN = (size + 1440 - 1) / 1440;
        uint32_t packets128 = (size + 1408 - 1) / 1408;
        uint32_t packets256 = (size + 1280 - 1) / 1280;
        uint32_t packets512 = (size + 1024 - 1) / 1024;

        if (packets512 == packetsMIN)
            shift = 7; // 512 byte blocks
        else if (packets256 == packetsMIN)
            shift = 6; // 256 byte blocks
        else if (packets128 == packetsMIN)
            shift = 5; // 128 byte blocks
        else
            shift = 3; //  32 byte blocks

        set_block_shift(shift);
    }

    void handle_cmd_info(struct sockaddr_in &si_other, struct SUDPBDv2_InfoRequest *request)
    {
        struct SUDPBDv2_InfoReply reply;

        char str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &si_other.sin_addr, str, INET_ADDRSTRLEN);

        _cprintf("UDPBD_CMD_INFO from %s     \n", str);
        print_stats();

        // Reply header
        reply.hdr.cmd = UDPBD_CMD_INFO_REPLY;
        reply.hdr.cmdid = request->hdr.cmdid;
        reply.hdr.cmdpkt = 1;
        // Reply info
        reply.sector_size = _bd.get_sector_size();
        reply.sector_count = _bd.get_sector_count();

        // Send packet to ps2
        if (sendto(s, (char *)&reply, sizeof(reply), 0, (struct sockaddr *)&si_other, sizeof(si_other)) == -1)
        {
            _cprintf("Error calling sendto in handle_cmd_info\nReady for Retry\n");
        }
    }

    void handle_cmd_read(struct sockaddr_in &si_other, struct SUDPBDv2_RWRequest *request)
    {
        struct SUDPBDv2_RDMA reply;

        _cprintf("UDPBD_CMD_READ(cmdId=%d, startSector=%d, sectorCount=%d)\n", request->hdr.cmdid, request->sector_nr, request->sector_count);

        // Optimize RDMA block size for number of sectors
        set_block_shift_sectors(request->sector_count);

        // Reply header
        reply.hdr.cmd = UDPBD_CMD_READ_RDMA;
        reply.hdr.cmdid = request->hdr.cmdid;
        reply.hdr.cmdpkt = 1;
        reply.bt.block_shift = _block_shift;

        uint32_t blocks_left = request->sector_count * _blocks_per_sector;

        _total_read += blocks_left * _block_size;
        print_stats();

        _bd.seek(request->sector_nr);
        _bd.clear_sector_offset();

        // Packet loop
        while (blocks_left > 0)
        {
            reply.bt.block_count = (blocks_left > _blocks_per_packet) ? _blocks_per_packet : blocks_left;
            blocks_left -= reply.bt.block_count;

            // read data from file
            _bd.read(reply.data2, reply.bt.block_count * _block_size);

            // Send packet to ps2
            if (sendto(s, (char *)&reply, sizeof(struct SUDPBDv2_Header) + 4 + (reply.bt.block_count * _block_size), 0, (struct sockaddr *)&si_other, sizeof(si_other)) == -1)
            {
                _cprintf("Error calling sendto in handle_cmd_read\n");
                throw ErrorMessage();
            }
            reply.hdr.cmdpkt++;
        }
    }

    void handle_cmd_write(struct sockaddr_in &si_other, struct SUDPBDv2_RWRequest *request)
    {
        _cprintf("UDPBD_CMD_WRITE(cmdId=%d, startSector=%d, sectorCount=%d)\n", request->hdr.cmdid, request->sector_nr, request->sector_count);

        _bd.seek(request->sector_nr);
        _write_size_left = request->sector_count * 512;

        _total_write += _write_size_left;
        print_stats();
    }

    void handle_cmd_write_rdma(struct sockaddr_in &si_other, struct SUDPBDv2_RDMA *request)
    {
        size_t size = request->bt.block_count * (1 << (request->bt.block_shift + 2));
        // _cprintf("UDPBD_CMD_WRITE_RDMA(cmdId=%d, BS=%d, BC=%d, size=%ld)\n", request->hdr.cmdid, request->bt.block_shift, request->bt.block_count, size);

        _bd.write(request->data2, size);
        _write_size_left -= size;
        if (_write_size_left == 0)
        {
            struct SUDPBDv2_WriteDone reply;

            // Reply header
            reply.hdr.cmd      = UDPBD_CMD_WRITE_DONE;
            reply.hdr.cmdid    = request->hdr.cmdid;
            reply.hdr.cmdpkt   = request->hdr.cmdid + 1;
            reply.result       = 0;

            // Send packet to ps2
            if (sendto(s, (char *)&reply, sizeof(reply), 0, (struct sockaddr *)&si_other, sizeof(si_other)) == -1)
            {
                _cprintf("Error calling sendto in handle_cmd_write_rdma\n");
                throw ErrorMessage();
            }
        }
    }

    class CBlockDevice &_bd;
    uint32_t _block_shift;
    uint32_t _block_size;
    uint32_t _blocks_per_packet;
    uint32_t _blocks_per_sector;
    int s;

    uint64_t _total_read;
    uint64_t _total_write;

    uint32_t _write_size_left;
};

extern "C" __declspec(dllexport) int Run_udpbd_server(char *path)
{
    _cprintf("UDPBD-Server Started\n");
    try
    {
        class CBlockDevice bd(path);
        class CUDPBDServer srv(bd);
        srv.run();
    }
    catch (std::exception &e)
    {
        return std::stoi(e.what());
    }

    return 0;
}

#ifndef _TCP_SOCKET_
#define _TCP_SOCKET_

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <numeric>

#include "TcpClient.h"
#include "TcpPullClient.h"
#include "TcpPullServer.h"
#include "TcpServer.h"
#include "base/log.h"
#include "spdlog/spdlog.h"
#include "utils/circular_buffer.h"

namespace latch {

#define MAX_RECEIVE_BUFFER_SIZE 4 * 1024
#define PACK_HEADER_IDENTIFY 0x73216A34

class TcpBase {
 public:
  TcpBase(uint64_t buffer_size) : receiveBuffer() {
    receiveBuffer = std::make_shared<CircularBufferThreadSafe<unsigned char>>(buffer_size);
  }

  uint64_t Receive(uint8_t* data, uint64_t size) { return receiveBuffer->Pop(data, size); }

  void Send(const uint8_t* data, uint64_t size) { SendData(data, size); }

  uint8_t GetChar() {
    while (receiveBuffer->Empty());
    uint8_t data = 0;
    Receive(&data, 1);
    return data;
  }

  uint64_t ReceivePacket(uint8_t* data, uint64_t size) {
    auto bufferSize = receiveBuffer->Size();
    uint64_t head_and_size = 0;
    if (bufferSize >= 8) {
      LOGCHECK(receiveBuffer->Peek((uint8_t*)&head_and_size, 8) == 8, "");
      if ((head_and_size & 0xFFFFFFFF) == PACK_HEADER_IDENTIFY) {
        auto ps = head_and_size >> 32;
        if (bufferSize - 8 >= ps && ps <= size) {
          receiveBuffer->Pop((uint8_t*)&head_and_size, 8);
          return receiveBuffer->Pop(data, ps);
        } else {
          return 0;
        }
      } else {
        spdlog::info("Pack Header Check Error!");
      }
    }
    return 0;
  }

  void SendPacket(uint8_t* data, uint64_t size, uint32_t a, uint32_t b) {
    uint32_t head[4];
    head[0] = PACK_HEADER_IDENTIFY;
    head[1] = size + 2 * 4;
    head[2] = a;
    head[3] = b;
    SendData((uint8_t*)head, 16);
    if (size > 0) SendData(data, size);
  }

  void SendPacket(uint8_t* data, uint64_t size, uint32_t a, uint32_t b, uint32_t c) {
    uint32_t head[5];
    head[0] = PACK_HEADER_IDENTIFY;
    head[1] = size + 3 * 4;
    head[2] = a;
    head[3] = b;
    head[4] = c;
    SendData((uint8_t*)head, 20);
    if (size > 0) SendData(data, size);
  }

  void SendPacket(uint8_t* data, uint64_t size) {
    uint64_t head_and_size = size;
    head_and_size = (head_and_size << 32) + PACK_HEADER_IDENTIFY;
    SendData((uint8_t*)&head_and_size, 8);
    if (size > 0) SendData(data, size);
  }

  bool IsReceiveEmpty() { return receiveBuffer->Empty(); }

  std::shared_ptr<CircularBufferThreadSafe<unsigned char>> receiveBuffer;

 protected:
  virtual EnFetchResult Fetch(BYTE* pData, int iLength) = 0;
  virtual void SendData(const uint8_t* data, uint64_t size) = 0;

  uint64_t remainToFetch = 0;
};

class TcpServer : public TcpBase {
  class ServerListenerImpl : public CTcpPullServerListener {
   public:
    ServerListenerImpl(TcpServer* server) { tcp = server; }
    virtual EnHandleResult OnPrepareListen(ITcpServer* pSender, SOCKET soListen) override { return HR_OK; }
    virtual EnHandleResult OnAccept(ITcpServer* pSender, CONNID dwConnID, UINT_PTR soClient) override {
      tcp->connId = dwConnID;
      return HR_OK;
    }
    virtual EnHandleResult OnHandShake(ITcpServer* pSender, CONNID dwConnID) override { return HR_OK; }
    virtual EnHandleResult OnReceive(ITcpServer* pSender, CONNID dwConnID, int iLength) override {
      LOGCHECK(dwConnID == tcp->connId, "Error tcp conn id.");
      ITcpPullServer* pServer = ITcpPullServer::FromS(pSender);
      unsigned char* data;
      int len = tcp->receiveBuffer->GetContinueBuffer(&data);
      int first_part = std::min(len, iLength);
      EnFetchResult result = FR_OK;
      if (first_part != 0) {
        result = pServer->Fetch(dwConnID, data, first_part);
        if (result != FR_OK) {
          spdlog::info("TCP Fetch Error");
        }
        tcp->receiveBuffer->PushWithoutData(first_part);
      }
      if (iLength > first_part) {
        len = tcp->receiveBuffer->GetContinueBuffer(&data);
        int second_part = std::min(len, iLength - first_part);
        if (second_part == 0) return HR_OK;
        result = pServer->Fetch(dwConnID, data, second_part);
        if (result != FR_OK) {
          spdlog::info("TCP Fetch Error");
        }
        tcp->receiveBuffer->PushWithoutData(second_part);
      }
      return HR_OK;
    }
    virtual EnHandleResult OnSend(ITcpServer* pSender, CONNID dwConnID, const BYTE* pData, int iLength) override {
      return HR_OK;
    }
    virtual EnHandleResult OnClose(ITcpServer* pSender, CONNID dwConnID, EnSocketOperation enOperation,
                                   int iErrorCode) override {
      tcp->connId = -1;
      return HR_OK;
    }
    virtual EnHandleResult OnShutdown(ITcpServer* pSender) override { return HR_OK; }

   private:
    TcpServer* tcp;
  };

 public:
  TcpServer(std::string ipAddr = "127.0.0.1", int port = 1335, uint64_t buffer_size = MAX_RECEIVE_BUFFER_SIZE)
      : TcpBase(buffer_size) {
    serverListener = std::make_shared<ServerListenerImpl>(this);
    server = std::make_shared<CTcpPullServer>(serverListener.get());
    server->SetWorkerThreadCount(1);
    Start(ipAddr, port);
  }
  ~TcpServer() { Stop(); }

  void Start(std::string ipAddr = "127.0.0.1", int port = 1335) {
    if (server != nullptr) {
      connId = -1;
      server->SetKeepAliveTime(60 * 1000);
      LOGCHECK(server->Start(ipAddr.c_str(), port), "Server Start Fail!");
    }
  }

  void Stop() {
    if (server != nullptr) server->Stop();
  }

 private:
  virtual EnFetchResult Fetch(BYTE* pData, int iLength) override { return server->Fetch(connId, pData, iLength); }

  virtual void SendData(const uint8_t* data, uint64_t size) override {
    if (!server->Send(connId, data, size)) spdlog::debug("Tcp send packet error!");
  };

  std::shared_ptr<ServerListenerImpl> serverListener;
  std::shared_ptr<CTcpPullServer> server;
  int connId;
};

class TcpClient : public TcpBase {
  class ClientListenerImpl : public CTcpPullClientListener {
   public:
    ClientListenerImpl(TcpClient* client) { tcp = client; }

    virtual EnHandleResult OnPrepareConnect(ITcpClient* pSender, CONNID dwConnID, SOCKET socket) override {
      return HR_OK;
    }
    virtual EnHandleResult OnConnect(ITcpClient* pSender, CONNID dwConnID) override { return HR_OK; }
    virtual EnHandleResult OnHandShake(ITcpClient* pSender, CONNID dwConnID) override { return HR_OK; }
    virtual EnHandleResult OnReceive(ITcpClient* pSender, CONNID dwConnID, int iLength) override {
      ITcpPullClient* pClient = ITcpPullClient::FromS(pSender);
      unsigned char* data;
      EnFetchResult result = FR_OK;
      int len = tcp->receiveBuffer->GetContinueBuffer(&data);
      int first_part = std::min(len, iLength);
      if (first_part >= 0) {
        result = pClient->Fetch(data, first_part);
        if (result != FR_OK) {
          spdlog::info("TCP Fetch Error");
        }
        tcp->receiveBuffer->PushWithoutData(first_part);
      }
      if (iLength > first_part) {
        len = tcp->receiveBuffer->GetContinueBuffer(&data);
        int second_part = std::min(len, iLength - first_part);
        if (second_part == 0) return HR_OK;
        result = pClient->Fetch(data, second_part);
        if (result != FR_OK) {
          spdlog::info("TCP Fetch Error");
        }
        tcp->receiveBuffer->PushWithoutData(second_part);
      }
      return HR_OK;
    }
    virtual EnHandleResult OnSend(ITcpClient* pSender, CONNID dwConnID, const BYTE* pData, int iLength) override {
      return HR_OK;
    }
    virtual EnHandleResult OnClose(ITcpClient* pSender, CONNID dwConnID, EnSocketOperation enOperation,
                                   int iErrorCode) override {
      return HR_OK;
    }

   private:
    TcpClient* tcp;
  };

 public:
  TcpClient(std::string ipAddr = "127.0.0.1", int port = 1335, uint64_t buffer_size = MAX_RECEIVE_BUFFER_SIZE)
      : TcpBase(buffer_size) {
    clientListener = std::make_shared<ClientListenerImpl>(this);
    client = std::make_shared<CTcpPullClient>(clientListener.get());
    Start(ipAddr, port);
  }
  ~TcpClient() { Stop(); }

  void Start(std::string ipAddr = "127.0.0.1", int port = 1335) {
    if (client != nullptr) {
      client->SetKeepAliveTime(60 * 1000);
      LOGCHECK(client->Start(ipAddr.c_str(), port), "Client Start Fail!");
    }
  }

  void Stop() {
    if (client != nullptr) client->Stop();
  }

 private:
  virtual EnFetchResult Fetch(BYTE* pData, int iLength) override { return client->Fetch(pData, iLength); }
  virtual void SendData(const uint8_t* data, uint64_t size) override {
    if (!client->Send(data, size)) spdlog::debug("Tcp send packet error!");
  };

  std::shared_ptr<ClientListenerImpl> clientListener;
  std::shared_ptr<CTcpPullClient> client;
};

class RdmaServer {
 public:
  RdmaServer(uint64_t maxPackSize, std::string ipAddr = "127.0.0.1", int port = 1236)
      : sokt(nullptr), cmdPack(), maxPack(maxPackSize) {
    if (ipAddr != "" && port != 0) {
      sokt = std::make_shared<TcpServer>(ipAddr, port, maxPackSize * 2);
      cmdPack = std::make_shared<std::vector<uint8_t>>(maxPackSize);
      spdlog::info("Rdma Server setup ip: {} port: {}", ipAddr, port);
    }
  }
  ~RdmaServer() {}

  virtual void ReadMem(uint64_t addr, unsigned char* dataPtr, uint64_t byteWidth) = 0;
  virtual void WriteMem(uint64_t addr, unsigned char* dataPtr, uint64_t byteWidth) = 0;

  virtual void Cycle() {
    if (sokt == nullptr) return;
    if (!sokt->IsReceiveEmpty()) {
      auto re = sokt->ReceivePacket(cmdPack->data(), cmdPack->size());
      if (re >= 4) {
        uint32_t type = *((uint32_t*)(cmdPack->data()));
        switch (type) {
          case 0: {
            break;
          }
          case 1: {
            LOGCHECK(re == 12, "RDMA Packet size Error!");
            uint32_t addr = *((uint32_t*)(cmdPack->data() + 4));
            uint32_t byteWidth = *((uint32_t*)(cmdPack->data() + 8));
            LOGCHECK(byteWidth <= (maxPack - 12), "RDMA Read size Error!");
            uint8_t* dataPtr = (uint8_t*)(cmdPack->data());
            ReadMem(addr, dataPtr, byteWidth);
            sokt->SendPacket(cmdPack->data(), byteWidth);
            break;
          }
          case 2: {
            LOGCHECK(re > 8, "RDMA Packet size Error!");
            uint32_t addr = *((uint32_t*)(cmdPack->data() + 4));
            uint32_t byteWidth = re - 8;
            uint8_t* dataPtr = ((uint8_t*)(cmdPack->data() + 8));
            WriteMem(addr, dataPtr, byteWidth);
            sokt->SendPacket(cmdPack->data(), 8);
            break;
          }
          default: {
            break;
          }
        }
      }
    }
  }

 private:
  std::shared_ptr<TcpServer> sokt;
  std::shared_ptr<std::vector<uint8_t>> cmdPack;
  uint64_t maxPack;
};

class RdmaClient {
 public:
  RdmaClient(uint64_t maxPackSize, std::string ipAddr = "127.0.0.1", int port = 1236)
      : sokt(nullptr), cmdPack(), maxPack(maxPackSize) {
    if (ipAddr != "" && port != 0) {
      sokt = std::make_shared<TcpClient>(ipAddr, port, maxPackSize * 2);
      cmdPack = std::make_shared<std::vector<uint8_t>>(maxPackSize);
      spdlog::info("Rdma Client Setup ip: {} port: {}", ipAddr, port);
    }
  }
  ~RdmaClient() {}

  void WriteMem(uint32_t addr, uint8_t* data, uint32_t size) {
    uint32_t type = 2;
    sokt->SendPacket(data, size, type, addr);

    uint64_t count = 0;
    while (sokt->IsReceiveEmpty()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      count++;
      if ((count & 0xFF) == 0) spdlog::info("RDMA Write Memory Receive Empty Delay!");
    }
    count = 0;
    while (1) {
      sokt->IsReceiveEmpty();
      uint64_t re_pack = 0;
      if (sokt->ReceivePacket((uint8_t*)&re_pack, 8) == 8) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      count++;
      if ((count & 0xFF) == 0) spdlog::info("RDMA Write Memory Receive Packet Delay!");
    }
  }

  void ReadMem(uint32_t addr, uint8_t* data, uint32_t size) {
    uint32_t type = 1;
    sokt->SendPacket(nullptr, 0, type, addr, size);

    uint64_t count = 0;
    while (sokt->IsReceiveEmpty()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      count++;
      if ((count & 0xFF) == 0) spdlog::info("RDMA Read Memory Receive Empty Delay!");
    }
    count = 0;
    while (1) {
      sokt->IsReceiveEmpty();
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      if (sokt->ReceivePacket(data, size) == size) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      count++;
      if ((count & 0xFF) == 0) spdlog::info("RDMA Read Memory Receive Packet Delay!");
    }
  }

 private:
  std::shared_ptr<TcpClient> sokt;
  std::shared_ptr<std::vector<uint8_t>> cmdPack;
  uint64_t maxPack;
};

}
#endif

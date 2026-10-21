/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <sys/socket.h>
#include <poll.h>
#include <unistd.h>
#include <endian.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace aera::mobile {
using Bytes = std::vector<uint8_t>;
using Values = std::map<uint8_t, Bytes>;
using Clock = std::chrono::steady_clock;
inline uint16_t U16(const uint8_t *p) { uint16_t x; memcpy(&x,p,2); return le16toh(x); }
inline uint32_t U32(const uint8_t *p) { uint32_t x; memcpy(&x,p,4); return le32toh(x); }
inline void Put16(Bytes &v, uint16_t x) { x=htole16(x); auto *p=reinterpret_cast<uint8_t *>(&x); v.insert(v.end(),p,p+2); }
inline void Put32(Bytes &v, uint32_t x) { x=htole32(x); auto *p=reinterpret_cast<uint8_t *>(&x); v.insert(v.end(),p,p+4); }
inline Bytes Words(std::initializer_list<uint32_t> values) { Bytes v; for (auto x:values) Put32(v,x); return v; }
inline void Tlv(Bytes &v,uint8_t tag,const Bytes &data) { v.push_back(tag); Put16(v,data.size()); v.insert(v.end(),data.begin(),data.end()); }
inline const Bytes &Field(const Values &v, uint8_t tag) { static const Bytes empty; auto it=v.find(tag); return it==v.end()?empty:it->second; }
struct Endpoint { uint32_t service=0, instance=0, node=0, port=0; };
struct QrtrAddress { uint16_t family=42, pad=0; uint32_t node=0, port=0; };

class QmiError : public std::runtime_error {
 public:
  QmiError(uint16_t code,uint16_t request):std::runtime_error("QMI error "+
      std::to_string(code)+" (request "+std::to_string(request)+")"),code(code){}
  const uint16_t code;
};

class Fd {
 public:
  explicit Fd(int fd=-1):fd_(fd) {}
  ~Fd(){if(fd_>=0)close(fd_);}
  Fd(const Fd &)=delete;
  Fd &operator=(const Fd &)=delete;
  Fd(Fd &&other) noexcept:fd_(other.fd_){other.fd_=-1;}
  void reset(int fd=-1){if(fd_>=0)close(fd_);fd_=fd;}
  int get() const{return fd_;}
 private:int fd_;
};

inline int QrtrSocket() {
  int fd=socket(42,SOCK_DGRAM|SOCK_CLOEXEC,0);
  if(fd<0)throw std::runtime_error("QRTR transport is unavailable");
  return fd;
}
inline void SendQrtr(int fd,const QrtrAddress &to,const Bytes &bytes) {
  if(sendto(fd,bytes.data(),bytes.size(),0,
      reinterpret_cast<const sockaddr *>(&to),sizeof(to))!=static_cast<ssize_t>(bytes.size()))
    throw std::runtime_error(std::string("Modem transport send failed: ")+strerror(errno));
}
inline Bytes ReceiveQrtr(int fd,QrtrAddress *from,Clock::time_point deadline,
                         const std::atomic<bool> *cancel=nullptr) {
  while(Clock::now()<deadline) {
    if(cancel && cancel->load())throw std::runtime_error("Connection cancelled");
    pollfd p{fd,POLLIN,0};
    const auto left=std::chrono::duration_cast<std::chrono::milliseconds>(deadline-Clock::now()).count();
    int n=poll(&p,1,static_cast<int>(std::min<int64_t>(200,std::max<int64_t>(1,left))));
    if(n<0 && errno==EINTR)continue;
    if(n<0)throw std::runtime_error("Modem transport poll failed");
    if(n==0)continue;
    Bytes bytes(65536);socklen_t len=sizeof(*from);
    ssize_t count=recvfrom(fd,bytes.data(),bytes.size(),0,reinterpret_cast<sockaddr *>(from),&len);
    if(count<0 && (errno==EINTR||errno==EAGAIN))continue;
    if(count<0||len!=sizeof(*from)||from->family!=42)
      throw std::runtime_error("Modem transport receive failed");
    bytes.resize(count);return bytes;
  }
  throw std::runtime_error("Modem response timed out");
}
inline std::vector<Endpoint> Discover(const std::atomic<bool> *cancel=nullptr,bool include_local=false) {
  Fd fd(QrtrSocket());QrtrAddress local;socklen_t len=sizeof(local);
  if(getsockname(fd.get(),reinterpret_cast<sockaddr *>(&local),&len))
    throw std::runtime_error("Cannot identify local QRTR node");
  local.port=0xfffffffe;SendQrtr(fd.get(),local,Words({10,0,0,0,0}));
  auto deadline=Clock::now()+std::chrono::seconds(3);
  std::vector<Endpoint> out;
  while(true) {
    QrtrAddress from;Bytes b=ReceiveQrtr(fd.get(),&from,deadline,cancel);
    if(from.node!=local.node||from.port!=local.port||b.size()!=20||U32(b.data())!=4)continue;
    Endpoint e{U32(&b[4]),U32(&b[8]),U32(&b[12]),U32(&b[16])};
    if(e.service==0&&e.node==0&&e.port==0)return out;
    // Never mistake a local WLAN helper advertising DMS for the real modem.
    if(include_local||e.node!=local.node)out.push_back(e);
  }
}

class QmiClient {
 public:
  explicit QmiClient(Endpoint endpoint):fd_(QrtrSocket()),endpoint_(endpoint){}
  Values Call(uint16_t message,const Bytes &payload={},int seconds=12,
              const std::atomic<bool> *cancel=nullptr) {
    if(payload.size()>65528)throw std::runtime_error("QMI request too large");
    const uint16_t txn=++txn_;Bytes packet{0};Put16(packet,txn);Put16(packet,message);
    Put16(packet,payload.size());packet.insert(packet.end(),payload.begin(),payload.end());
    SendQrtr(fd_.get(),QrtrAddress{42,0,endpoint_.node,endpoint_.port},packet);
    auto deadline=Clock::now()+std::chrono::seconds(seconds);
    while(true) {
      QrtrAddress from;Bytes b=ReceiveQrtr(fd_.get(),&from,deadline,cancel);
      if(from.node!=endpoint_.node||from.port!=endpoint_.port||b.size()<7||b[0]!=2||
          U16(&b[1])!=txn||U16(&b[3])!=message)continue;
      if(U16(&b[5])!=b.size()-7)throw std::runtime_error("Invalid QMI response length");
      Values values;
      for(size_t i=7;i<b.size();) {
        if(b.size()-i<3)throw std::runtime_error("Truncated QMI header");
        uint8_t tag=b[i];size_t size=U16(&b[i+1]);i+=3;
        if(size>b.size()-i||values.count(tag))throw std::runtime_error("Invalid QMI field");
        values[tag]=Bytes(b.begin()+i,b.begin()+i+size);i+=size;
      }
      const auto &result=Field(values,2);
      if(result.size()!=4)throw std::runtime_error("Missing QMI operation result");
      if(U16(result.data()))throw QmiError(U16(&result[2]),message);
      return values;
    }
  }
 private:Fd fd_;Endpoint endpoint_;uint16_t txn_=0;
};
}
